// Deterministic offline replay through the actual host decoder. Synthetic H.264,
// not a live-phone or end-to-end benchmark. Compare decoded NV12 with a reference.
#include <windows.h>
#include <mfapi.h>
#include "decode/MFH264Decoder.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "RtpDecoderReplay.h"

using Bytes = std::vector<uint8_t>;
struct Nal { size_t begin, payload, end; unsigned type; };

std::vector<Nal> Nals(const Bytes& data) {
    std::vector<Nal> nals;
    for (size_t i = 0; i + 3 < data.size();) {
        size_t prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) prefix = 3;
            else if (i + 4 < data.size() && data[i + 2] == 0 && data[i + 3] == 1) prefix = 4;
        }
        if (!prefix) { ++i; continue; }
        if (!nals.empty()) nals.back().end = i;
        nals.push_back({i, i + prefix, data.size(), static_cast<unsigned>(data[i + prefix] & 31)});
        i += prefix + 1;
    }
    return nals;
}

std::vector<Bytes> AccessUnits(const Bytes& data) {
    std::vector<size_t> delimiters;
    for (const auto& nal : Nals(data)) if (nal.type == 9) delimiters.push_back(nal.begin);
    if (delimiters.size() < 90) throw std::runtime_error("Need at least 90 AUD-delimited frames");
    delimiters[0] = 0;
    delimiters.push_back(data.size());
    std::vector<Bytes> units;
    for (size_t i = 0; i + 1 < delimiters.size(); ++i)
        units.emplace_back(data.begin() + delimiters[i], data.begin() + delimiters[i + 1]);
    return units;
}

Bytes Parameters(const Bytes& unit, bool onlyParameters) {
    Bytes filtered;
    for (const auto& nal : Nals(unit)) {
        const bool parameter = nal.type == 7 || nal.type == 8;
        if (parameter == onlyParameters)
            filtered.insert(filtered.end(), unit.begin() + nal.begin, unit.begin() + nal.end);
    }
    return filtered;
}

using Pictures = std::map<uint64_t, uint64_t>;
Pictures Decode(const std::vector<Bytes>& units, bool repeatHeaders, bool reset, bool cacheHeaders, bool drop) {
    phonecam::decode::MFH264Decoder decoder;
    if (!decoder.Initialize()) throw std::runtime_error("Decoder initialization failed");
    Pictures pictures;
    const auto headers = Parameters(units[0], true);
    if (headers.empty()) throw std::runtime_error("No initial SPS/PPS");
    auto capture = [&](const phonecam::decode::DecodedFrame& frame) {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < frame.nv12Size; ++i) hash = (hash ^ frame.nv12[i]) * 1099511628211ULL;
        pictures[frame.timestampUs] = hash;
    };
    for (size_t i = 0; i < units.size(); ++i) {
        const uint64_t pts = i * 1000000ULL / 30;
        if (i == 45 && reset) {
            if (!decoder.Initialize()) throw std::runtime_error("Decoder reset failed");
            if (cacheHeaders) decoder.Feed(headers.data(), headers.size(), pts, capture);
        }
        if (i == 45 && drop) continue;
        const auto payload = !repeatHeaders && i > 0 ? Parameters(units[i], false) : units[i];
        // A decoder lacking configuration may reject intermediate packets. Keep
        // feeding so the next self-contained IDR has the opportunity to recover.
        decoder.Feed(payload.data(), payload.size(), pts, capture);
    }
    return pictures;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: phonecam-decoder-recovery <AUD-H264-file> <new-output-directory> [--rtp]\n";
        return 2;
    }
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) return 1;
    const HRESULT startup = MFStartup(MF_VERSION);
    if (FAILED(startup)) { CoUninitialize(); return 1; }
    int status = 0;
    try {
        const std::filesystem::path output(argv[2]);
        if (std::filesystem::exists(output)) throw std::runtime_error("Output directory already exists");
        std::filesystem::create_directories(output);
        std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open input bitstream");
        Bytes bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto units = AccessUnits(bytes);
        const auto reference = Decode(units, true, false, false, false);
        if (reference.size() < units.size() - 3) throw std::runtime_error("Reference decode incomplete");
        if (argc == 4) {
            if (std::wstring(argv[3]) != L"--rtp") throw std::runtime_error("Unknown replay option");
            VerifyRtpRecovery(units, reference, output);
        } else {
        struct Scenario { const char* name; bool repeat, reset, cache; };
        const Scenario scenarios[] = {
            {"drop-one-AU-initial-headers-only", false, false, false},
            {"reset-initial-headers-only", false, true, false},
            {"reset-cached-headers", false, true, true},
            {"reset-repeated-headers", true, true, false},
        };
        std::ofstream summary(output / "summary.csv");
        summary << "case,reference_frames,decoded_frames,mismatched_or_missing_after_loss,sustained_match_frame,recovery_media_ms\n";
        for (const auto& scenario : scenarios) {
            const auto pictures = Decode(units, scenario.repeat, scenario.reset, scenario.cache, true);
            int lastBad = 45, bad = 0;
            std::ofstream detail(output / (std::string(scenario.name) + ".csv"));
            detail << "frame,pts_us,reference_hash,decoded_hash,matches\n";
            for (const auto& [pts, hash] : reference) {
                const auto frame = static_cast<int>((pts * 30 + 999999) / 1000000);
                const auto found = pictures.find(pts);
                const bool matches = found != pictures.end() && found->second == hash;
                if (frame > 45 && !matches) { lastBad = frame; ++bad; }
                detail << frame << ',' << pts << ',' << hash << ',';
                if (found != pictures.end()) detail << found->second;
                detail << ',' << matches << '\n';
            }
            const int recovered = lastBad + 1;
            summary << scenario.name << ',' << reference.size() << ',' << pictures.size() << ',' << bad << ',';
            if (recovered < static_cast<int>(units.size()) - 2) summary << recovered << ',' << (recovered - 45) * 1000.0 / 30;
            summary << '\n';
        }
        std::cout << "Synthetic MF decoder recovery replay completed; results in summary.csv\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    MFShutdown();
    CoUninitialize();
    return status;
}
