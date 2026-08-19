#include <mfapi.h>
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bridge/LiveVideoBridge.h"
#include "bridge/TestPatternProducer.h"
#include "control/ConsoleControlUi.h"
#include "control/ControlChannel.h"
#include "decode/MFH264Decoder.h"
#include "log/Log.h"
#include "transport/AdbTransport.h"
#include "transport/AoaTransport.h"
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

// Static storage duration, not a block-scoped local: the elevated helper it guards runs on a
// detached worker thread (see RunUsbDriverHelperElevated's launchHelper wiring below) that can
// outlive the block it was started in if the app shuts down mid-install -- a block-scoped
// std::atomic captured by reference would dangle in that case. This is the same reasoning as
// g_tray/g_stopEvent above, just for a different piece of cross-thread shutdown-adjacent state.
std::atomic<bool> g_driverSetupInFlight{false};

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

// Phase 4 (tray controls): small label tables, matching ConsoleControlUi.cpp's own -- duplicated
// intentionally rather than shared, same reasoning as that class's own capability-cache
// duplication (see ControlChannel.h's Phase 4 comment): different consumer, not worth coupling.
const char* AfModeName(int m) {
    switch (m) {
        case 0: return "Off";
        case 1: return "Auto";
        case 2: return "Continuous";
        case 3: return "Macro";
        case 4: return "Edge Capture";
        default: return "?";
    }
}

const char* FacingName(int f) {
    switch (f) {
        case 0: return "Back";
        case 1: return "Front";
        case 2: return "External";
        default: return "?";
    }
}

const phonecam::control::LensInfo* FindLens(const phonecam::control::CapabilityInfo& caps,
                                             const std::string& cameraId) {
    for (const auto& lens : caps.lenses) {
        if (lens.cameraId == cameraId) return &lens;
    }
    return nullptr;
}

// A handful of human-recognizable zoom stops, filtered to the current lens's actual supported
// range -- a tray menu needs discrete picks, not a continuous slider. zoomMin is always included
// even if it doesn't land exactly on a candidate, so "no zoom" is always offered.
std::vector<float> ZoomPresets(float zoomMin, float zoomMax) {
    static constexpr float kCandidates[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f};
    std::vector<float> out;
    for (float c : kCandidates) {
        if (c >= zoomMin - 0.01f && c <= zoomMax + 0.01f) out.push_back(c);
    }
    if (out.empty() || out.front() > zoomMin + 0.01f) out.insert(out.begin(), zoomMin);
    return out;
}

// Same idea for EV compensation, whose native range is already small integer "steps" (not a raw
// EV value -- see SendSetEv's own doc comment) -- enumerate every step if there are few enough to
// stay a sane menu size, else fall back to ~7 evenly spaced picks across the full range.
std::vector<int32_t> EvPresets(int32_t evMin, int32_t evMax) {
    std::vector<int32_t> out;
    if (evMax <= evMin) {
        out.push_back(evMin);
        return out;
    }
    const int32_t span = evMax - evMin;
    if (span <= 8) {
        for (int32_t v = evMin; v <= evMax; ++v) out.push_back(v);
    } else {
        constexpr int kSteps = 6;
        for (int i = 0; i <= kSteps; ++i) {
            const int32_t v = evMin + static_cast<int32_t>(static_cast<int64_t>(span) * i / kSteps);
            if (out.empty() || out.back() != v) out.push_back(v);
        }
    }
    return out;
}

// Phase 1 (transport auto-detect): runs `adb devices` and checks for a
// line ending in a real device state ("...\tdevice"), as opposed to
// "unauthorized"/"offline"/"no permissions"/absent entirely. Used once at
// startup to decide adb vs. AOA -- see the useAoa computation in wmain.
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
        phonecam::log::Error("DetectAdbDevicePresent: failed to launch adb (is it on PATH?) -- assuming AOA");
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

// Phase 2 UX: launches windows/usbdriver/phonecam-usbdriver.exe (built to sit next to this
// exe, same directory PhoneCam.iss installs both into) elevated via ShellExecuteExW's "runas"
// verb -- this is the ONE UAC prompt covering the whole install-or-revert operation, not
// phonecam-host.exe itself, which stays asInvoker throughout (see its own build.md notes on
// why elevating the main process would be the wrong shape). Blocks the CALLING thread until
// the helper exits -- callers run this on a dedicated worker thread (see the tray menu wiring
// below), never on the tray's own message-pump thread, so a UAC prompt or a slow install never
// freezes the tray icon.
//
// Returns the helper's exit code, or -1 if it couldn't even be launched (missing binary, or
// ShellExecuteExW itself failing -- e.g. ERROR_CANCELLED if the user clicks "No" on the UAC
// prompt, which is the expected, non-error way to decline).
int RunUsbDriverHelperElevated(const wchar_t* args) {
    wchar_t exePath[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        phonecam::log::Error("RunUsbDriverHelperElevated: GetModuleFileNameW failed");
        return -1;
    }
    std::wstring helperPath(exePath, len);
    const size_t lastSlash = helperPath.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) return -1;
    helperPath = helperPath.substr(0, lastSlash + 1) + L"phonecam-usbdriver.exe";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = helperPath.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            phonecam::log::Info("USB driver helper: user declined the admin prompt");
        } else {
            phonecam::log::Error("RunUsbDriverHelperElevated: ShellExecuteExW failed (is phonecam-usbdriver.exe "
                                  "next to phonecam-host.exe?)");
        }
        return -1;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    return static_cast<int>(exitCode);
}

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
    // --aoa / --adb: explicit dev overrides, unchanged from Phase 7. Video
    // channel only either way -- control still runs over adb regardless of
    // which video transport is picked (see AoaVideoTransport's class doc
    // comment for why).
    const bool forceAoa = argc > 1 && std::wstring(argv[1]) == L"--aoa";
    const bool forceAdb = argc > 1 && std::wstring(argv[1]) == L"--adb";
    // Phase 1: with no explicit flag, auto-detect -- prefer adb when a
    // debugging-enabled phone is actually present (the proven, no-extra-
    // setup path), fall back to AOA otherwise (which needs the driver from
    // windows/usbdriver/ to have been set up; if it hasn't, AoaVideoTransport
    // ::Connect() just fails and retries, same as adb failing to find a
    // phone streaming -- see LiveVideoBridge::Run()'s existing backoff).
    const bool useAoa = forceAoa || (!forceAdb && !useTestPattern && !DetectAdbDevicePresent());
    if (!forceAoa && !forceAdb && !useTestPattern) {
        phonecam::log::Info(useAoa ? "No debugging-enabled phone detected -- using AOA"
                                    : "Debugging-enabled phone detected -- using adb");
    }

    phonecam::bridge::TestPatternProducer producer;
    phonecam::bridge::LiveVideoBridge bridge(
        useAoa ? std::unique_ptr<phonecam::transport::VideoTransport>(
                     std::make_unique<phonecam::transport::AoaVideoTransport>())
               : std::unique_ptr<phonecam::transport::VideoTransport>(
                     std::make_unique<phonecam::transport::AdbVideoTransport>()));
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

        // Phase 2 UX: g_driverSetupInFlight guards against a second click launching a second
        // elevated helper (and a second UAC prompt) while one is still running; the actual
        // RunUsbDriverHelperElevated() call always happens on a fresh detached worker thread,
        // never on the tray's own message-pump thread, so a slow install or an unanswered UAC
        // prompt never freezes the tray icon or its menu.
        auto launchHelper = [](const wchar_t* args, const char* verb) {
            if (g_driverSetupInFlight.exchange(true)) return;  // already running one
            std::thread([args, verb]() {
                phonecam::log::Info(std::string("USB driver helper: launching (") + verb + ")...");
                const int rc = RunUsbDriverHelperElevated(args);
                phonecam::log::Info(std::string("USB driver helper: ") + verb + " finished, exit code " +
                                     std::to_string(rc));
                g_driverSetupInFlight.store(false);
            }).detach();
        };

        // Phase 4 (tray controls): rotation/mirror are pure PC-side rendering settings -- the
        // phone is never involved, so this local state (read/written only on the tray's own
        // message-pump thread: itemsProvider and every onClick below all run there, see
        // TrayIcon.cpp) is the single source of truth, applied to the shared ring so the vcam DLL
        // picks it up on its next draw. Default 90 clockwise, not 0: the reference mounting for
        // this project is the phone held vertically, and a landscape sensor frame needs exactly
        // this rotation to come out normal (see FrameGenerator's rotation math) -- 0 would look
        // sideways for the common case, matching nobody's actual setup out of the box.
        phonecam::shm::Rotation currentRotation = phonecam::shm::Rotation::Deg90;
        bool currentMirror = false;
        bridge.SetTransform(currentRotation, currentMirror);

        // Phase 5: a tray icon so phonecam-host.exe can run backgrounded/minimized instead of
        // requiring a visible console. Status text is a cheap read of atomics/getters already
        // updated by the bridge and control-channel supervise loops above -- no polling thread of
        // its own needed, TrayIcon refreshes on its own timer and on menu-open.
        phonecam::tray::TrayIcon tray;
        tray.SetMenuItems([&]() -> std::vector<phonecam::tray::MenuItem> {
            using phonecam::tray::MenuItem;
            std::vector<MenuItem> items;

            // Camera-content controls (lens/zoom/exposure/torch/focus): these are PC->phone
            // commands over the control channel (still adb-only today -- see AoaVideoTransport's
            // class doc -- so these send successfully but have no visible effect until the
            // control channel also runs over AOA). Sourced from the last-seen Capabilities +
            // CurrentSettings, scoped to whichever lens is currently active -- nothing shown at
            // all until the phone has actually reported capabilities once, matching the existing
            // "never show a control the phone doesn't support" rule.
            if (controlChannel.HasCapabilities()) {
                const auto caps = controlChannel.GetLastCapabilities();
                const bool hasSettings = controlChannel.HasSettings();
                const auto settings = hasSettings ? controlChannel.GetLastSettings() : phonecam::control::SettingsInfo{};
                const phonecam::control::LensInfo* current =
                    hasSettings ? FindLens(caps, settings.cameraId) : nullptr;
                if (!current && !caps.lenses.empty()) current = &caps.lenses[0];

                if (!caps.lenses.empty()) {
                    MenuItem cameraMenu{"Camera", nullptr, false, {}};
                    for (const auto& lens : caps.lenses) {
                        const std::string label = std::string(FacingName(lens.facing)) + " (camera " + lens.cameraId + ")";
                        const bool checked = hasSettings && settings.cameraId == lens.cameraId;
                        const std::string cameraId = lens.cameraId;
                        cameraMenu.submenu.push_back(
                            {label, [&controlChannel, cameraId]() { controlChannel.SendSetLens(cameraId); }, checked, {}});
                    }
                    items.push_back(std::move(cameraMenu));
                }

                if (current && current->zoomMax > current->zoomMin) {
                    MenuItem zoomMenu{"Zoom", nullptr, false, {}};
                    for (float z : ZoomPresets(current->zoomMin, current->zoomMax)) {
                        char label[16];
                        std::snprintf(label, sizeof(label), "%.1fx", z);
                        const bool checked = hasSettings && std::fabs(settings.zoomRatio - z) < 0.05f;
                        zoomMenu.submenu.push_back(
                            {label, [&controlChannel, z]() { controlChannel.SendSetZoomRatio(z); }, checked, {}});
                    }
                    items.push_back(std::move(zoomMenu));
                }

                if (current && current->evMax > current->evMin) {
                    MenuItem evMenu{"Exposure", nullptr, false, {}};
                    for (int32_t ev : EvPresets(current->evMin, current->evMax)) {
                        char label[24];
                        std::snprintf(label, sizeof(label), "EV %+d", ev);
                        const bool checked = hasSettings && settings.evSteps == ev;
                        evMenu.submenu.push_back(
                            {label, [&controlChannel, ev]() { controlChannel.SendSetEv(ev); }, checked, {}});
                    }
                    items.push_back(std::move(evMenu));
                }

                if (current && current->hasTorch) {
                    const bool torchOn = hasSettings && settings.torchOn;
                    items.push_back({"Torch", [&controlChannel, torchOn]() { controlChannel.SendSetTorch(!torchOn); },
                                      torchOn, {}});
                }

                if (current && !current->afModes.empty()) {
                    MenuItem focusMenu{"Focus", nullptr, false, {}};
                    for (int mode : current->afModes) {
                        const bool checked = hasSettings && settings.focusMode == mode;
                        focusMenu.submenu.push_back({AfModeName(mode),
                                                      [&controlChannel, mode]() { controlChannel.SendSetFocusMode(mode); },
                                                      checked, {}});
                    }
                    items.push_back(std::move(focusMenu));
                }
            }

            // Rotation/mirror: pure PC-side, always available regardless of phone capabilities.
            {
                MenuItem rotationMenu{"Rotation", nullptr, false, {}};
                static constexpr std::pair<const char*, phonecam::shm::Rotation> kRotations[] = {
                    {"0", phonecam::shm::Rotation::Deg0},
                    {"90 clockwise", phonecam::shm::Rotation::Deg90},
                    {"180", phonecam::shm::Rotation::Deg180},
                    {"270 clockwise", phonecam::shm::Rotation::Deg270},
                };
                for (const auto& [label, rotation] : kRotations) {
                    rotationMenu.submenu.push_back({label, [&, rotation]() {
                                                         currentRotation = rotation;
                                                         bridge.SetTransform(currentRotation, currentMirror);
                                                     }, currentRotation == rotation, {}});
                }
                items.push_back(std::move(rotationMenu));
            }
            items.push_back({"Mirror", [&]() {
                                  currentMirror = !currentMirror;
                                  bridge.SetTransform(currentRotation, currentMirror);
                              }, currentMirror, {}});

            // Resolution: read-only display -- there is no PC-side "set resolution" mechanism,
            // the consuming app (Zoom/OBS/Chrome) picks it via its own device settings (see
            // docs/architecture.md's Phase 3 section). This just shows what was actually
            // negotiated, sourced from FrameGenerator publishing it into the ring on every draw.
            {
                uint32_t width = 0, height = 0;
                bridge.GetNegotiatedSize(width, height);
                char label[64];
                if (width > 0 && height > 0) {
                    std::snprintf(label, sizeof(label), "Resolution: %ux%u (set from the app)", width, height);
                } else {
                    std::snprintf(label, sizeof(label), "Resolution: not negotiated yet");
                }
                items.push_back({label, nullptr, false, {}});
            }

            if (g_driverSetupInFlight.load()) {
                items.push_back({"USB driver helper running...", nullptr, false, {}});
            } else {
                // Only offered in AOA mode -- the adb path never needs this driver at all, and
                // offering it there would be actively misleading (see AoaVideoTransport's own
                // composite-device guard: installing this while debugging is on is refused anyway).
                if (useAoa) {
                    items.push_back({"Set up USB driver for AOA...",
                                      [&]() { launchHelper(L"--install", "install"); }, false, {}});
                }
                items.push_back(
                    {"Remove USB driver (restore file transfer)", [&]() { launchHelper(L"--revert-all", "revert"); },
                     false, {}});
            }
            return items;
        });
        const bool trayCreated = tray.Create([&]() -> std::string {
            if (useTestPattern) return "test pattern (dev mode)";
            const bool video = bridge.IsConnected();
            const bool control = controlConnected.load();
            const char* videoLabel = useAoa ? "video via AOA" : "video";
            if (video && control) return std::string("streaming (") + videoLabel + " + control)";
            if (video) return std::string("streaming (") + videoLabel + " only)";
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
