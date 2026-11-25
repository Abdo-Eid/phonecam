#include <mfapi.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/AppController.h"
#include "bridge/LiveVideoBridge.h"
#include "bridge/TestPatternProducer.h"
#include "control/ConsoleControlUi.h"
#include "control/ControlChannel.h"
#include "decode/MFH264Decoder.h"
#include "log/Log.h"
#include "text/Utf8.h"
#include "transport/AdbTransport.h"
#include "transport/NetTransport.h"
#include "tray/TrayIcon.h"
#include "ui/MainWindow.h"
#include "connection/ConnectionManager.h"
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
// (dev-only): real usage gets pre-chunked packets over the wire instead.
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

// Menus underline the letter after '&'; a preset name means it literally.
std::string MenuLabel(const std::wstring& text) {
    std::wstring escaped;
    for (const wchar_t c : text) {
        escaped += c;
        if (c == L'&') escaped += c;
    }
    return phonecam::text::Narrow(escaped);
}

// The app has no console when started normally, so an error the user must act on needs a box.
void ShowStartupError(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"PhoneCam", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// Transport auto-detect: runs `adb devices` and checks for a
// line ending in a real device state ("...\tdevice"), as opposed to
// "unauthorized"/"offline"/"no permissions"/absent entirely. USB tethering
// is preferred when its RNDIS interface is available; ADB is the fallback.
// Deliberately does NOT re-check continuously: a given phone/PC setup is
// consistently debugging-on or debugging-off in practice, and if the user
// changes it, relaunching phonecam-host (easy via the tray's Exit item)
// picks up the new state -- a live-switching design would need
// restructuring LiveVideoBridge to swap transports mid-run, which the
// reconnect loop doesn't support and isn't worth building for an edge case.
bool DetectAdbDevicePresent() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"adb devices";
    const BOOL ok =
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        phonecam::log::Error("DetectAdbDevicePresent: failed to launch adb (is it on PATH?)");
        CloseHandle(readPipe);
        return false;
    }

    std::string output;
    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buf, bytesRead);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // adb's device-state vocabulary ("device", "unauthorized", "offline", "no permissions",
    // "authorizing") has no overlap after a tab, so a plain substring search is safe here.
    return output.find("\tdevice") != std::string::npos;
}

// Offline proof: decode a raw .h264 file (same shape as the
// scratchpad captures already validated with ffmpeg) with zero
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
    // how they'll arrive as one wire-protocol CONFIG packet.
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

// Live-transport proof: adb-forward + wire framing, entirely decoupled from the
// decoder. Strips the 20-byte header from each received packet and appends
// the raw Annex-B payload straight to a .h264 file -- if that file plays
// back correctly (same ffmpeg check used for the scratchpad captures), the framing is
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

// Shared-ring checkpoint: FrameGenerator's built-in test pattern is now
// replaced by a SharedFrameRing reader (windows/vcam/FrameGenerator.cpp).
// This host writes a synthetic animated NV12 pattern into that ring via
// TestPatternProducer, standing in for the real H.264-decode bridge that
// H.264 decode adds. See docs/architecture.md.
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

    // --test-pattern: the old synthetic-frame source, kept as an
    // opt-in dev tool for exercising the PC side (ring/vcam/Frame Server)
    // without a phone connected. Real usage is the default path below.
    const bool useTestPattern = argc > 1 && std::wstring(argv[1]) == L"--test-pattern";
    // Explicit transport overrides for diagnostics; normal use auto-selects
    // USB tethering first, then USB debugging (ADB).
    const bool forceAdb = argc > 1 && std::wstring(argv[1]) == L"--adb";
    const bool forceNet = argc > 1 && std::wstring(argv[1]) == L"--net";

    // Started from the Start menu or desktop, as opposed to a script or a dev run with options
    // (--background: the normal app, without opening its window).
    const bool startedByUser = argc == 1;

    // One PhoneCam per user session: two would publish into the same ring, overwrite each other's
    // settings and fight over the phone. Starting it again just brings up the running one.
    const HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\PhoneCamHost");
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        phonecam::log::Error("PhoneCam is already running");
        if (startedByUser) phonecam::tray::TrayIcon::SignalRunningInstance();
        if (instance) CloseHandle(instance);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    phonecam::connection::ConnectionManager connection;
    auto& controlChannel = connection.Control();
    auto& bridge = connection.Bridge();
    phonecam::control::ConsoleControlUi controlUi(controlChannel);
    phonecam::app::AppController app(connection);
    connection.SetMessageHandler([&](const phonecam::control::ControlMessage& message) {
        app.OnMessage(message);
        controlUi.OnMessage(message);
    });
    app.Load();

    phonecam::bridge::TestPatternProducer producer;
    using phonecam::connection::Mode;
    phonecam::connection::Target initial;
    initial.mode = forceNet ? Mode::Tethered : forceAdb ? Mode::Adb : app.SavedMode();
    app.SetInitialMode(initial.mode);
    auto resolveAutomatic = []() {
        phonecam::connection::Target target;
        target.address = phonecam::transport::DiscoverTetheredPhoneAddress();
        target.mode = !target.address.empty() ? Mode::Tethered : Mode::Adb;
        if (target.mode == Mode::Adb && !DetectAdbDevicePresent())
            phonecam::log::Info("No USB debugging device detected; waiting for USB debugging (ADB)");
        return target;
    };
    if (useTestPattern) {
        // Optional "WIDTHxHEIGHT": check one output size without a phone. The pattern itself is
        // half that size, so it fits the ring and fills the output exactly.
        unsigned outputWidth = 0, outputHeight = 0;
        const bool oneSize = argc > 2 && swscanf_s(argv[2], L"%ux%u", &outputWidth, &outputHeight) == 2;
        const bool started = oneSize ? producer.Start(outputWidth / 2, outputHeight / 2, 30, outputWidth, outputHeight)
                                     : producer.Start(1280, 960, 30);
        if (!started) {
            phonecam::log::Error("TestPatternProducer failed to start");
            MFShutdown();
            CoUninitialize();
            return 1;
        }
    } else {
        if (!connection.Start(initial, resolveAutomatic)) {
            phonecam::log::Error("LiveVideoBridge failed to start");
            if (startedByUser) {
                ShowStartupError(L"PhoneCam's background service is not running.\n\nRestart the PC, or run the PhoneCam "
                                 L"installer again.");
            }
            MFShutdown();
            CoUninitialize();
            return 1;
        }
    }

    std::thread controlUiThread([&]() { controlUi.RunCommandLoop(); });

    phonecam::vcam_ctl::VCamControl vcam;
    hr = vcam.Start(L"PhoneCam", kVCamSourceClsid);
    if (SUCCEEDED(hr)) {
        std::printf("PhoneCam virtual camera started. Ctrl+C (or close this window) to stop.\n");
        std::fflush(stdout);

        // A tray icon so phonecam-host.exe can run backgrounded/minimized instead of
        // requiring a visible console. Status text is a cheap read of atomics/getters already
        // updated by the bridge and control-channel supervise loops above -- no polling thread of
        // its own needed, TrayIcon refreshes on its own timer and on menu-open.
        phonecam::tray::TrayIcon tray;
        uint64_t lastStreamingEvent = 0;
        tray.SetNotificationProvider([&]() -> std::string {
            const auto state = connection.GetSnapshot();
            if (state.streamingEvent == lastStreamingEvent) return {};
            lastStreamingEvent = state.streamingEvent;
            return "Connected - streaming";
        });
        phonecam::vcam_ctl::OfferedSizeWatch offeredSize(bridge.GetOfferedSize());

        // Settings live in the window; the menu keeps only what is worth reaching without it.
        // Both act through AppController, so they always agree.
        phonecam::ui::MainWindow window(app, connection, [&](const std::string& message) { tray.Notify(message); });
        tray.SetMenuItems([&]() -> std::vector<phonecam::tray::MenuItem> {
            using phonecam::tray::MenuItem;
            std::vector<MenuItem> items;
            if (useTestPattern) return items;
            const auto state = connection.GetSnapshot();
            items.push_back({"Open PhoneCam", [&]() { window.Show(); }, false, {}, true});
            if (state.controlReady) {
                const bool stopped = state.state == phonecam::connection::State::Stopped;
                items.push_back({stopped ? "Start" : "Stop", [&, stopped]() { connection.SetStreaming(stopped); }, false, {}});
            }
            MenuItem presetMenu{"Presets", nullptr, false, {}};
            const auto presets = app.Presets();
            const auto active = app.ActivePreset(presets);
            for (const auto& preset : presets) {
                presetMenu.submenu.push_back(
                    {MenuLabel(preset.name), [&, name = preset.name]() { app.ApplyPreset(name); }, preset.name == active, {}});
            }
            items.push_back(std::move(presetMenu));
            return items;
        });
        if (!useTestPattern) {
            tray.SetOpenHandler([&]() { window.Show(); });
            tray.SetTick([&]() {
                app.Tick();
                const auto now = static_cast<int64_t>(GetTickCount64());
                if (!offeredSize.Due(bridge.GetOfferedSize(), bridge.GetConsumer().active, now)) return;
                vcam.Stop();
                if (SUCCEEDED(vcam.Start(L"PhoneCam", kVCamSourceClsid))) {
                    offeredSize.Created();
                } else {
                    offeredSize.Failed(now);
                }
            });
            tray.SetMessageFilter([&](void* msg) { return window.FilterMessage(msg); });
        }
        const bool trayCreated = tray.Create([&]() -> std::string {
            if (useTestPattern) return "test pattern (dev mode)";
            return connection.GetSnapshot().message;
        });
        if (trayCreated) {
            g_tray = &tray;
            SetConsoleCtrlHandler(ConsoleHandler, TRUE);
            if (startedByUser) window.Show();
            tray.RunMessageLoop();  // returns on Exit (tray menu) or Ctrl+C (ConsoleHandler)
            g_tray = nullptr;
            if (!useTestPattern) app.Flush();
        } else {
            // Fallback if Shell_NotifyIcon itself failed: the same plain event-wait used before the
            // tray icon existed,
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
        if (useTestPattern) {
            producer.ReleaseOutputSize();
        } else {
            bridge.ReleaseOfferedSize();
        }
    } else if (startedByUser) {
        ShowStartupError(L"Windows could not start the PhoneCam camera.\n\nPhoneCam needs Windows 11. If you have it, "
                         L"run the PhoneCam installer again.");
    }

    connection.Stop();
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
