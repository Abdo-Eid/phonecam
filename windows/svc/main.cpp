#include <windows.h>

#include <cstdio>
#include <string>

#include "log/Log.h"
#include "shm/SharedFrameRing.h"

// phonecam-svc: the ONLY thing this process does is create and hold open the
// cross-session SharedFrameRing (see windows/common/shm/SharedFrameRing.h).
// That creation requires SeCreateGlobalPrivilege, which a normal interactive
// process doesn't have -- see docs/architecture.md. Running as a LocalSystem
// Windows Service gets that privilege for free, so phonecam-host (which
// writes frames) and phonecam-vcam.dll (which reads them) can both stay
// ordinary unprivileged processes: once this service has created the ring,
// they just attach to the already-existing object, which is a normal DACL
// check, not a privileged create.
//
// Everything else (vcam_ctl, the USB transport, frame decode) stays in
// phonecam-host -- there's no reason to move it here, and keeping this
// service's job to exactly one thing keeps it easy to reason about.

namespace {

constexpr wchar_t kServiceName[] = L"PhoneCamRingService";
constexpr wchar_t kServiceDisplayName[] = L"PhoneCam Frame Ring Service";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stopEvent = nullptr;
phonecam::shm::SharedFrameRing g_ring;

void UpdateServiceStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    static DWORD checkPoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    ::SetServiceStatus(g_statusHandle, &g_status);
}

void WINAPI ServiceControlHandler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP) {
        UpdateServiceStatus(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
}

// Shared between real-service mode (ServiceMain) and --console debug mode,
// so the one thing this process does is exercised identically either way.
DWORD RunRingOwner(HANDLE stopEvent) {
    if (!g_ring.OpenOrCreate()) {
        phonecam::log::Error("phonecam-svc: failed to create SharedFrameRing (must run elevated / as a service)");
        return ERROR_ACCESS_DENIED;
    }
    phonecam::log::Info("phonecam-svc: SharedFrameRing created, holding open");
    WaitForSingleObject(stopEvent, INFINITE);
    g_ring.Close();
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_statusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceControlHandler);
    if (!g_statusHandle) {
        return;
    }

    UpdateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        UpdateServiceStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    UpdateServiceStatus(SERVICE_RUNNING);
    RunRingOwner(g_stopEvent);

    CloseHandle(g_stopEvent);
    UpdateServiceStatus(SERVICE_STOPPED);
}

bool InstallService() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        phonecam::log::Error("OpenSCManager failed (run as administrator)");
        return false;
    }

    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
                                    SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path,
                                    nullptr, nullptr, nullptr, L"LocalSystem", nullptr);

    bool ok = svc != nullptr;
    if (!ok) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            phonecam::log::Info("Service already installed");
            ok = true;
        } else {
            phonecam::log::Error("CreateService failed");
        }
    } else {
        phonecam::log::Info("Service installed");
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        phonecam::log::Error("OpenSCManager failed (run as administrator)");
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP);
    bool ok = false;
    if (svc) {
        SERVICE_STATUS status{};
        ControlService(svc, SERVICE_CONTROL_STOP, &status);  // best-effort; ignore failure if already stopped
        ok = DeleteService(svc) != 0;
        phonecam::log::Info(ok ? "Service uninstalled" : "DeleteService failed");
        CloseServiceHandle(svc);
    } else {
        phonecam::log::Error("OpenService failed (not installed?)");
    }
    CloseServiceHandle(scm);
    return ok;
}

BOOL WINAPI ConsoleModeHandler(DWORD /*ctrlType*/) {
    SetEvent(g_stopEvent);
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) {
        const std::wstring arg = argv[1];
        if (arg == L"--install") {
            return InstallService() ? 0 : 1;
        }
        if (arg == L"--uninstall") {
            return UninstallService() ? 0 : 1;
        }
        if (arg == L"--console") {
            std::printf("phonecam-svc running in console mode (not a real service). Ctrl+C to stop.\n");
            std::fflush(stdout);
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            SetConsoleCtrlHandler(ConsoleModeHandler, TRUE);
            const DWORD result = RunRingOwner(g_stopEvent);
            CloseHandle(g_stopEvent);
            return result == NO_ERROR ? 0 : 1;
        }
    }

    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        phonecam::log::Error("StartServiceCtrlDispatcher failed (run with --console for interactive debug mode)");
        return 1;
    }
    return 0;
}
