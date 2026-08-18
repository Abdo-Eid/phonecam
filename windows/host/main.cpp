#include <mfapi.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "bridge/LiveVideoBridge.h"
#include "bridge/TestPatternProducer.h"
#include "control/ConsoleControlUi.h"
#include "control/ControlChannel.h"
#include "decode/MFH264Decoder.h"
#include "log/Log.h"
#include "transport/AdbTransport.h"
#include "tray/TrayIcon.h"
#include "vcam_ctl/VCamControl.h"

namespace {

// d0255f4e-4471-47c4-91fc-0e74dcc93308 -- must match windows/vcam/dllmain.cpp
constexpr GUID kVCamSourceClsid = {
    0xd0255f4e, 0x4471, 0x47c4, {0x91, 0xfc, 0x0e, 0x74, 0xdc, 0xc9, 0x33, 0x08}};

// Set once, right before TrayIcon::RunMessageLoop() -- ConsoleHandler runs on a separate
// OS-created thread (per SetConsoleCtrlHandler's contract), so Ctrl+C and the tray's own Exit
// menu item both have to be able to reach the same TrayIcon instance and fall through the same
// shutdown path below, not two separate ones. g_stopEvent is the fallback used only if the tray
// icon itself fails to create (Shell_NotifyIcon isn't expected to fail on a normal desktop, but
// isn't guaranteed either) -- exactly one of the two is non-null at a time.
phonecam::tray::TrayIcon* g_tray = nullptr;
HANDLE g_stopEvent = nullptr;

BOOL WINAPI ConsoleHandler(DWORD /*ctrlType*/) {
    if (g_tray) {
        g_tray->Close();
    } else if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    return TRUE;
}

// Splits a raw Annex-B buffer into per-NALU spans (each beginning at a
// 00 00 01 start code, running up to the next one or EOF). Dev/test-only
// (Phase 3A): real usage gets pre-chunked packets over the wire instead.
std::vector<std::vector<uint8_t>> SplitAnnexBNalus(const std::vector<uint8_t>& bytes) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            starts.push_back(i);
        }
    }
    std::vector<std::vector<uint8_t>> nalus;
    for (size_t k = 0; k < starts.size(); ++k) {
        const size_t begin = starts[k];
        const size_t end = (k + 1 < starts.size()) ? starts[k + 1] : bytes.size();
        nalus.emplace_back(bytes.begin() + begin, bytes.begin() + end);
    }
    return nalus;
}

uint8_t NaluType(const std::vector<uint8_t>& nalu) { return nalu.size() < 4 ? 0xFF : (nalu[3] & 0x1F); }

// Phase 3A offline proof: decode a raw .h264 file (same shape as the
// scratchpad captures already validated with ffmpeg in Phase 2) with zero
// phone/transport involvement, and dump one decoded frame's packed NV12 so
// it can be diffed against the known-good ffmpeg-extracted frame. See
// docs/architecture.md.
int RunTestDecode(const std::wstring& inputPath, const std::wstring& outputPath) {
    std::ifstream file(inputPath, std::ios::binary);
    if (!file) {
        std::fwprintf(stderr, L"Cannot open input file: %ls\n", inputPath.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    std::printf("Read %zu bytes\n", bytes.size());

    auto nalus = SplitAnnexBNalus(bytes);
    std::printf("Split into %zu NALUs\n", nalus.size());
    if (nalus.empty()) {
        std::fprintf(stderr, "No NALUs found\n");
        return 1;
    }

    phonecam::decode::MFH264Decoder decoder;
    if (!decoder.Initialize()) {
        std::fprintf(stderr, "Decoder Initialize() failed\n");
        return 1;
    }

    int decodedCount = 0;
    bool dumped = false;
    auto onFrame = [&](const phonecam::decode::DecodedFrame& f) {
        decodedCount++;
        std::printf("Decoded frame %d: %ux%u pts=%llu\n", decodedCount, f.width, f.height,
                     static_cast<unsigned long long>(f.timestampUs));
        if (!dumped && decodedCount == 5) {
            std::ofstream out(outputPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(f.nv12), static_cast<std::streamsize>(f.nv12Size));
            std::wprintf(L"Dumped frame %d (%ux%u, %zu bytes) to %ls\n", decodedCount, f.width, f.height, f.nv12Size,
                         outputPath.c_str());
            dumped = true;
        }
    };

    // Group leading SPS/PPS (types 7/8) into one CONFIG-style feed, matching
    // how they'll arrive as one wire-protocol CONFIG packet in Phase 3C.
    std::vector<uint8_t> config;
    size_t idx = 0;
    while (idx < nalus.size()) {
        const uint8_t t = NaluType(nalus[idx]);
        if (t != 7 && t != 8) break;
        config.insert(config.end(), nalus[idx].begin(), nalus[idx].end());
        ++idx;
    }
    if (!config.empty()) {
        std::printf("Feeding CONFIG (%zu bytes)\n", config.size());
        decoder.Feed(config.data(), config.size(), 0, onFrame);
    }

    uint64_t pts = 0;
    for (; idx < nalus.size(); ++idx) {
        decoder.Feed(nalus[idx].data(), nalus[idx].size(), pts, onFrame);
        pts += 33'333;
    }

    std::printf("Total decoded frames: %d\n", decodedCount);
    return dumped ? 0 : 1;
}

// Phase 3B proof: adb-forward + wire framing, entirely decoupled from the
// decoder. Strips the 20-byte header from each received packet and appends
// the raw Annex-B payload straight to a .h264 file -- if that file plays
// back correctly (same ffmpeg check used in Phase 2), the framing is
// byte-correct end to end, independent of whether MFH264Decoder is right.
// Runs until the phone side disconnects (e.g. the user presses Stop).
int RunTestTransport(const std::wstring& outputPath) {
    phonecam::transport::AdbVideoTransport transport;
    if (!transport.Connect()) {
        std::fprintf(stderr, "Failed to connect (is capture running on the phone?)\n");
        return 1;
    }
    std::printf("Connected. Streaming to %ls -- stop capture on the phone to end this test.\n", outputPath.c_str());
    std::fflush(stdout);

    std::ofstream out(outputPath, std::ios::binary);
    int configCount = 0, frameCount = 0, keyframeCount = 0;
    size_t totalBytes = 0;

    transport.RunReceiveLoop([&](const phonecam::transport::VideoPacket& pkt) {
        out.write(reinterpret_cast<const char*>(pkt.payload), static_cast<std::streamsize>(pkt.payloadSize));
        totalBytes += pkt.payloadSize;
        if (pkt.type == phonecam::transport::PacketType::Config) {
            ++configCount;
        } else {
            ++frameCount;
            if (pkt.keyframe) ++keyframeCount;
        }
        if ((configCount + frameCount) % 30 == 0) {
            std::printf("... %d config, %d frames (%d keyframes), %zu bytes\n", configCount, frameCount,
                        keyframeCount, totalBytes);
            std::fflush(stdout);
        }
    });

    std::printf("Disconnected. Total: %d config, %d frames (%d keyframes), %zu bytes -> %ls\n", configCount,
                frameCount, keyframeCount, totalBytes, outputPath.c_str());
    return 0;
}

}  // namespace

// Phase 1b checkpoint 2: FrameGenerator's built-in test pattern is now
// replaced by a SharedFrameRing reader (windows/vcam/FrameGenerator.cpp).
// This host writes a synthetic animated NV12 pattern into that ring via
// TestPatternProducer, standing in for the real H.264-decode bridge that
// Phase 3 adds. See docs/architecture.md.
int wmain(int argc, wchar_t* argv[]) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        phonecam::log::Error("CoInitializeEx failed");
        return 1;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        phonecam::log::Error("MFStartup failed");
        CoUninitialize();
        return 1;
    }

    if (argc > 1 && std::wstring(argv[1]) == L"--test-decode") {
        int result = 1;
        if (argc > 3) {
            result = RunTestDecode(argv[2], argv[3]);
        } else {
            std::fprintf(stderr, "usage: phonecam-host --test-decode <input.h264> <output.nv12>\n");
        }
        MFShutdown();
        CoUninitialize();
        return result;
    }

    if (argc > 1 && std::wstring(argv[1]) == L"--test-transport") {
        int result = 1;
        if (argc > 2) {
            result = RunTestTransport(argv[2]);
        } else {
            std::fprintf(stderr, "usage: phonecam-host --test-transport <output.h264>\n");
        }
        MFShutdown();
        CoUninitialize();
        return result;
    }

    // --test-pattern: the old Phase 1b synthetic-frame source, kept as an
    // opt-in dev tool for exercising the PC side (ring/vcam/Frame Server)
    // without a phone connected. Real usage is the default path below.
    const bool useTestPattern = argc > 1 && std::wstring(argv[1]) == L"--test-pattern";

    phonecam::bridge::TestPatternProducer producer;
    phonecam::bridge::LiveVideoBridge bridge;
    if (useTestPattern) {
        if (!producer.Start(1280, 960, 30)) {
            phonecam::log::Error("TestPatternProducer failed to start");
            MFShutdown();
            CoUninitialize();
            return 1;
        }
    } else {
        if (!bridge.Start()) {
            phonecam::log::Error("LiveVideoBridge failed to start");
            MFShutdown();
            CoUninitialize();
            return 1;
        }
    }

    // Phase 4: control channel (proto/control.fbs) on a second adb-forwarded
    // port, alongside the video channel started above. Phase 5: this now
    // supervises its own reconnect, mirroring LiveVideoBridge::Run() -- a USB
    // replug or adb-server restart kills both channels' TCP connections
    // together, so control needs the same connect-retry loop video got. A
    // connect failure is still non-fatal to the video pipeline (e.g. an older
    // phone app build predates control-channel support): the console UI just
    // keeps retrying quietly in the background.
    phonecam::control::ControlChannel controlChannel;
    phonecam::control::ConsoleControlUi controlUi(controlChannel);
    std::atomic<bool> controlRunning{true};
    std::atomic<bool> controlConnected{false};  // Phase 5: tray-icon status text reads this
    // Backoff applies after every disconnect, not just an outright Connect()
    // failure -- adb forward's local TCP proxy can accept the PC-side
    // connect() immediately and only then discover nothing is listening on
    // the phone's abstract socket, so "connected" followed by an instant
    // disconnect is a real case (confirmed live via LiveVideoBridge hitting
    // exactly this), not just Connect() returning false outright. Without
    // backing off there too, this busy-loops: no sleep, `adb forward` (a
    // whole new adb.exe process per attempt) launched many times a second.
    std::thread controlSuperviseThread([&]() {
        while (controlRunning.load()) {
            if (controlChannel.Connect()) {
                controlConnected.store(true);
                // Every (re)connect implies the video side may also just have
                // reconnected onto a fresh encoder session (new SPS/PPS, no
                // guaranteed keyframe yet) -- ask for one explicitly rather
                // than waiting for the phone's own keyframe interval.
                controlChannel.SendRequestKeyframe();
                controlChannel.RunReceiveLoop(
                    [&](const phonecam::control::ControlMessage& msg) { controlUi.OnMessage(msg); });
                controlConnected.store(false);
                controlChannel.Disconnect();
                if (!controlRunning.load()) break;
                phonecam::log::Info("Control channel disconnected, reconnecting");
            }
            for (int waitedMs = 0; waitedMs < 2000 && controlRunning.load(); waitedMs += 200) {
                Sleep(200);
            }
        }
    });
    std::thread controlUiThread([&]() { controlUi.RunCommandLoop(); });

    phonecam::vcam_ctl::VCamControl vcam;
    hr = vcam.Start(L"PhoneCam", kVCamSourceClsid);
    if (SUCCEEDED(hr)) {
        std::printf("PhoneCam virtual camera started. Ctrl+C (or close this window) to stop.\n");
        std::fflush(stdout);

        // Phase 5: a tray icon so phonecam-host.exe can run backgrounded/minimized instead of
        // requiring a visible console. Status text is a cheap read of atomics/getters already
        // updated by the bridge and control-channel supervise loops above -- no polling thread of
        // its own needed, TrayIcon refreshes on its own timer and on menu-open.
        phonecam::tray::TrayIcon tray;
        const bool trayCreated = tray.Create([&]() -> std::string {
            if (useTestPattern) return "test pattern (dev mode)";
            const bool video = bridge.IsConnected();
            const bool control = controlConnected.load();
            if (video && control) return "streaming (video + control)";
            if (video) return "streaming (video only)";
            return "waiting for phone...";
        });
        if (trayCreated) {
            g_tray = &tray;
            SetConsoleCtrlHandler(ConsoleHandler, TRUE);
            tray.RunMessageLoop();  // returns on Exit (tray menu) or Ctrl+C (ConsoleHandler)
            g_tray = nullptr;
        } else {
            // Fallback if Shell_NotifyIcon itself failed: same plain event-wait Phase 1-4 used,
            // so Ctrl+C still cleanly shuts everything down even without a tray icon.
            phonecam::log::Error("TrayIcon: failed to create, falling back to a plain wait (Ctrl+C still works)");
            HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_stopEvent = stopEvent;
            SetConsoleCtrlHandler(ConsoleHandler, TRUE);
            WaitForSingleObject(stopEvent, INFINITE);
            g_stopEvent = nullptr;
            CloseHandle(stopEvent);
        }

        vcam.Stop();
    }

    controlRunning.store(false);
    controlChannel.Disconnect();  // unblocks a pending connect retry or the receive loop's blocking recv()
    if (controlSuperviseThread.joinable()) controlSuperviseThread.join();
    // controlUiThread is blocked in std::getline(std::cin, ...), which
    // nothing here can interrupt -- detach it and let process exit reclaim
    // it, rather than hanging shutdown on an unblockable join.
    if (controlUiThread.joinable()) controlUiThread.detach();

    if (useTestPattern) {
        producer.Stop();
    } else {
        bridge.Stop();
    }
    MFShutdown();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
