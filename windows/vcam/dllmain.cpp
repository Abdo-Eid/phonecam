#include <windows.h>

#include "log/Log.h"

// Phase 0 skeleton: proves the DLL target builds and links against the
// common library. The real IMFMediaSourceEx COM media source (registration,
// DllGetClassObject/DllRegisterServer, NV12+RGB32 streaming) is forked from
// VCamSample in Phase 1 — see docs/architecture.md.
BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        phonecam::log::Info("phonecam-vcam.dll skeleton build (Phase 0) attached");
    }
    return TRUE;
}
