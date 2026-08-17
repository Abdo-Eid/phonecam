#include <mfapi.h>
#include <windows.h>

#include <cstdio>

#include "bridge/TestPatternProducer.h"
#include "log/Log.h"
#include "vcam_ctl/VCamControl.h"

namespace {

// d0255f4e-4471-47c4-91fc-0e74dcc93308 -- must match windows/vcam/dllmain.cpp
constexpr GUID kVCamSourceClsid = {
    0xd0255f4e, 0x4471, 0x47c4, {0x91, 0xfc, 0x0e, 0x74, 0xdc, 0xc9, 0x33, 0x08}};

HANDLE g_stopEvent;

// Console control handler rather than std::getchar(): works whether the
// process is run interactively or headless/backgrounded (no stdin/console
// attached), which is closer to how phonecam-host actually runs once it's a
// background process, and doesn't silently no-op on EOF the way getchar()
// does when stdin isn't a real interactive console.
BOOL WINAPI ConsoleHandler(DWORD /*ctrlType*/) {
    SetEvent(g_stopEvent);
    return TRUE;
}

}  // namespace

// Phase 1b checkpoint 2: FrameGenerator's built-in test pattern is now
// replaced by a SharedFrameRing reader (windows/vcam/FrameGenerator.cpp).
// This host writes a synthetic animated NV12 pattern into that ring via
// TestPatternProducer, standing in for the real H.264-decode bridge that
// Phase 3 adds. See docs/architecture.md.
int main() {
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

    phonecam::bridge::TestPatternProducer producer;
    if (!producer.Start(1280, 960, 30)) {
        phonecam::log::Error("TestPatternProducer failed to start");
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    phonecam::vcam_ctl::VCamControl vcam;
    hr = vcam.Start(L"PhoneCam", kVCamSourceClsid);
    if (SUCCEEDED(hr)) {
        std::printf("PhoneCam virtual camera started. Ctrl+C (or close this window) to stop.\n");
        std::fflush(stdout);

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        WaitForSingleObject(g_stopEvent, INFINITE);
        CloseHandle(g_stopEvent);

        vcam.Stop();
    }

    producer.Stop();
    MFShutdown();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
