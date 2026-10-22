// Fixed native MF consumer: timestamp ReadSample return, not image-file writes.
// Drain every sample. Retain occasional raw samples in memory and write only
// after capture stops. No fps resampling, preview clock or PNG encoder.
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char message[160]{};
        sprintf_s(message, "%s: HRESULT 0x%08lX", operation, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

struct Runtime {
    Runtime() {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        const HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) { CoUninitialize(); Check(hr, "MFStartup"); }
    }
    ~Runtime() { MFShutdown(); CoUninitialize(); }
};

struct Frame {
    uint64_t number;
    int64_t qpc;
    uint64_t unixUs;
    int64_t sampleTime;
    DWORD flags;
    DWORD bytes = 0;
    std::vector<BYTE> saved;
    int64_t mfReceipt = 0;
};

ComPtr<IMFMediaSource> OpenCamera(const std::wstring& name) {
    ComPtr<IMFAttributes> attributes;
    Check(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
    Check(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "capture type");
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    Check(MFEnumDeviceSources(attributes.Get(), &devices, &count), "MFEnumDeviceSources");
    ComPtr<IMFActivate> selected;
    unsigned matches = 0;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* label = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &label, &length))) {
            std::wcout << L"Camera: " << label << L'\n';
            if (std::wstring(label).find(name) != std::wstring::npos) {
                selected = devices[i];
                ++matches;
            }
            CoTaskMemFree(label);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (matches != 1) throw std::runtime_error("Expected exactly one matching PhoneCam camera; is host running?");
    ComPtr<IMFMediaSource> source;
    Check(selected->ActivateObject(IID_PPV_ARGS(&source)), "ActivateObject");
    return source;
}

void WriteBitmap(const fs::path& path, const Frame& frame, UINT32 width, UINT32 height, LONG stride) {
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(frame.saved.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    // A positive media stride advances top-to-bottom. BMP encodes that layout
    // with a NEGATIVE height; positive BMP height would vertically invert it.
    info.biHeight = stride < 0 ? static_cast<LONG>(height) : -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(frame.saved.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(frame.saved.data()), frame.saved.size());
    if (!output) throw std::runtime_error("BMP write failed");
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 3 || argc > 5) {
            std::cerr << "Usage: phonecam-media-bench <output-directory> <seconds:1..900> [save-every-N:0..300] [read-delay-ms:0..250]\n";
            return 2;
        }
        const fs::path directory(argv[1]);
        const int seconds = std::stoi(argv[2]);
        const int saveEvery = argc >= 4 ? std::stoi(argv[3]) : 30;
        const int readDelay = argc == 5 ? std::stoi(argv[4]) : 0;
        if (seconds < 1 || seconds > 900 || saveEvery < 0 || saveEvery > 300 || readDelay < 0 || readDelay > 250)
            throw std::runtime_error("Invalid capture limits");
        // Do not overwrite a previous result.
        if (fs::exists(directory)) throw std::runtime_error("Output directory already exists");
        fs::create_directories(directory);
        Runtime runtime;
        auto source = OpenCamera(L"PhoneCam");
        ComPtr<IMFAttributes> attributes;
        Check(MFCreateAttributes(&attributes, 1), "reader attributes");
        Check(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE), "disable converters");
        ComPtr<IMFSourceReader> reader;
        Check(MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader), "source reader");
        Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "deselect streams");
        Check(reader->SetStreamSelection(kVideoStream, TRUE), "select video");

        ComPtr<IMFMediaType> type;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT hr = reader->GetNativeMediaType(kVideoStream, i, &candidate);
            if (hr == MF_E_NO_MORE_TYPES) break;
            Check(hr, "native media type");
            GUID subtype{};
            UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
            candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
            MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
            if (subtype == MFVideoFormat_RGB32 && width == 1920 && height == 1080 &&
                numerator == 30 && denominator == 1) { type = candidate; break; }
        }
        if (!type) throw std::runtime_error("Native RGB32 1920x1080@30 not offered");
        Check(reader->SetCurrentMediaType(kVideoStream, nullptr, type.Get()), "set format");
        const UINT32 width = 1920, height = 1080;
        const LONG stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
        if (stride != static_cast<LONG>(width * 4) && stride != -static_cast<LONG>(width * 4))
            throw std::runtime_error("Unexpected RGB32 stride");

        LARGE_INTEGER frequency{}, start{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        std::vector<Frame> frames;
        frames.reserve(seconds * 60 + 120);
        unsigned emptyReads = 0;
        do {
            DWORD stream = 0, flags = 0;
            LONGLONG sampleTime = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader->ReadSample(kVideoStream, 0,
                                                  &stream, &flags, &sampleTime, &sample);
            // Timestamp receipt BEFORE sample inspection/copy/serialization.
            QueryPerformanceCounter(&now);
            const auto mfReceipt = MFGetSystemTime();
            FILETIME wall{};
            GetSystemTimePreciseAsFileTime(&wall);
            ULARGE_INTEGER fileTime{};
            fileTime.LowPart = wall.dwLowDateTime;
            fileTime.HighPart = wall.dwHighDateTime;
            Check(hr, "ReadSample");
            if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM |
                         MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
                throw std::runtime_error("Stream ended, failed or changed format");
            if (!sample) { ++emptyReads; continue; }
            Frame frame{frames.size() + 1, now.QuadPart,
                        (fileTime.QuadPart - 116444736000000000ULL) / 10, sampleTime, flags};
            frame.mfReceipt = mfReceipt;
            Check(sample->GetTotalLength(&frame.bytes), "sample length");
            // Retain at most 20 images (~166 MB); nothing is encoded/written here.
            if (saveEvery && (frame.number - 1) % saveEvery == 0 &&
                (frame.number - 1) / saveEvery < 20) {
                ComPtr<IMFMediaBuffer> buffer;
                Check(sample->ConvertToContiguousBuffer(&buffer), "contiguous sample");
                BYTE* data = nullptr;
                DWORD length = 0;
                Check(buffer->Lock(&data, nullptr, &length), "lock sample");
                try {
                    if (length == width * height * 4) frame.saved.assign(data, data + length);
                } catch (...) {
                    buffer->Unlock();
                    throw;
                }
                buffer->Unlock();
                if (frame.saved.empty()) throw std::runtime_error("Unexpected RGB32 sample size");
            }
            frames.push_back(std::move(frame));
            if (readDelay) std::this_thread::sleep_for(std::chrono::milliseconds(readDelay));
        } while (now.QuadPart - start.QuadPart < static_cast<int64_t>(seconds) * frequency.QuadPart);

        reader.Reset();
        const HRESULT shutdown = source->Shutdown();
        // Releasing SourceReader may already have shut its media source down.
        if (shutdown != MF_E_SHUTDOWN) Check(shutdown, "source shutdown");
        std::ofstream csv(directory / "receipt.csv");
        csv << "frame,arrival_qpc,arrival_unix_us,sample_time_100ns,flags,bytes,image,mf_receipt_100ns\n";
        for (const auto& frame : frames) {
            const std::string image = frame.saved.empty() ? "" : "sample-" + std::to_string(frame.number) + ".bmp";
            if (!image.empty()) WriteBitmap(directory / image, frame, width, height, stride);
            csv << frame.number << ',' << frame.qpc << ',' << frame.unixUs << ',' << frame.sampleTime
                << ',' << frame.flags << ',' << frame.bytes << ',' << image << ',' << frame.mfReceipt << '\n';
        }
        if (!csv) throw std::runtime_error("CSV write failed");
        std::ofstream manifest(directory / "format.txt");
        manifest << "consumer=native MF synchronous SourceReader\nwidth=" << width << "\nheight=" << height
                 << "\nstride=" << stride << "\nqpc_frequency=" << frequency.QuadPart
                 << "\nempty_reads=" << emptyReads << "\nseconds=" << seconds
                 << "\nread_delay_ms=" << readDelay << '\n';
        if (!manifest || frames.empty()) throw std::runtime_error("No samples or manifest write failed");
        std::cout << "Captured " << frames.size() << " samples; receipt timestamps in receipt.csv.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
