// phonecam-usbdriver.exe: elevated helper that binds a libusbK driver to a
// phone's normal-mode USB device, so the AOA transport (windows/host/transport/
// AoaTransport.cpp) can talk to it without the user ever running Zadig
// themselves. See docs/architecture.md's Phase 7 driver-auto-install sections
// for the full investigation this is built on -- in particular, the finding
// that the binding must target the phone's CURRENT (not accessory-mode)
// device, and that a composite device (USB debugging ON, exposing ADB/MTP
// sub-interfaces) must never be touched here, since replacing its composite
// driver would break `adb` system-wide.
//
// This is a separate executable (not linked into phonecam-host.exe) so
// libwdi's own internal elevation (it spawns an embedded installer_x64.exe
// per wdi_install_driver call) only ever has to happen once, already inside
// an already-elevated process, instead of prompting per call.

#include <libwdi.h>
#include <newdev.h>
#include <setupapi.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// AOA accessory-mode PIDs are standardized across every Android device (see
// source.android.com/docs/core/interaction/accessories/aoa) -- always under
// Google's VID 0x18D1, regardless of the phone's own normal-mode VID (e.g.
// this Redmi Note 8's 0x18D1, or a Xiaomi/Redmi device that instead
// enumerates under 0x2717 in normal mode). Confirmed live this session: the
// Note 8 re-enumerated as exactly 0x2D00 after ACCESSORY_START. Unlike the
// present-device install above, these devices don't exist yet at helper-run
// time, so they're pre-staged into the driver store instead of installed.
constexpr unsigned short kGoogleVid = 0x18D1;
constexpr unsigned short kAccessoryPidLow = 0x2D00;
constexpr unsigned short kAccessoryPidHigh = 0x2D05;

// Refuses to touch a composite device (ADB/MTP sub-interfaces present, i.e.
// USB debugging is on) -- see this file's header comment. Exit code 11.
constexpr int kExitComposite = 11;
constexpr int kExitNoDevice = 12;
constexpr int kExitInstallFailed = 13;
constexpr int kExitBadArgs = 2;

bool ParseHex16(const char* s, unsigned short* out) {
    if (!s) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 16);
    if (end == s || *end != '\0' || v > 0xFFFF) return false;
    *out = static_cast<unsigned short>(v);
    return true;
}

// Finds the currently-present device matching vid/pid, refuses composite
// devices, generates and installs a self-signed libusbK driver package for
// it. Mirrors the exact shape already proven manually against the Note 8's
// accessory-mode node (see docs/architecture.md's "Resolved, not just
// diagnosed" section) -- same driver type, same wdi_prepare_driver +
// wdi_install_driver pair, just automated and targeting whatever VID/PID is
// passed in rather than a hardcoded one.
int InstallForPresentDevice(unsigned short vid, unsigned short pid) {
    wdi_options_create_list createOpts{};
    createOpts.list_all = TRUE;    // the device already has a driver (wpdmtp.inf) -- not driverless
    createOpts.list_hubs = TRUE;   // composite parent devices only show up with this on
    createOpts.trim_whitespaces = TRUE;

    wdi_device_info* list = nullptr;
    int rc = wdi_create_list(&list, &createOpts);
    if (rc != WDI_SUCCESS) {
        std::printf("wdi_create_list failed: %s\n", wdi_strerror(rc));
        return kExitInstallFailed;
    }

    wdi_device_info* target = nullptr;
    for (wdi_device_info* d = list; d != nullptr; d = d->next) {
        if (d->vid == vid && d->pid == pid) {
            target = d;
            break;
        }
    }
    if (!target) {
        std::printf("Device VID_%04X&PID_%04X is not currently present.\n", vid, pid);
        wdi_destroy_list(list);
        return kExitNoDevice;
    }

    std::printf("Found device: desc=\"%s\" driver=\"%s\" is_composite=%d\n",
                target->desc ? target->desc : "(null)", target->driver ? target->driver : "(none)",
                target->is_composite);

    if (target->is_composite) {
        std::printf(
            "Device is composite (has active sub-interfaces, e.g. ADB -- USB debugging appears to be "
            "on). Refusing to install: doing so would replace the driver adb depends on. Nothing to do "
            "here -- the adb transport works without any driver.\n");
        wdi_destroy_list(list);
        return kExitComposite;
    }

    wdi_options_prepare_driver prepOpts{};
    prepOpts.driver_type = WDI_LIBUSBK;

    char tempPath[MAX_PATH];
    GetTempPathA(static_cast<DWORD>(sizeof(tempPath)), tempPath);
    char driverDir[MAX_PATH];
    std::snprintf(driverDir, sizeof(driverDir), "%sphonecam_driver", tempPath);

    rc = wdi_prepare_driver(target, driverDir, "phonecam.inf", &prepOpts);
    if (rc != WDI_SUCCESS) {
        std::printf("wdi_prepare_driver failed: %s\n", wdi_strerror(rc));
        wdi_destroy_list(list);
        return kExitInstallFailed;
    }
    std::printf("Driver package prepared at %s\\phonecam.inf\n", driverDir);

    wdi_options_install_driver installOpts{};
    installOpts.pending_install_timeout = 30000;
    rc = wdi_install_driver(target, driverDir, "phonecam.inf", &installOpts);
    wdi_destroy_list(list);
    if (rc != WDI_SUCCESS) {
        std::printf("wdi_install_driver failed: %s\n", wdi_strerror(rc));
        return kExitInstallFailed;
    }

    std::printf("Driver installed successfully for VID_%04X&PID_%04X.\n", vid, pid);
    return 0;
}

// Generates a self-signed libusbK package for one not-yet-present accessory
// PID and stages it into the driver store via SetupCopyOEMInfW, so Windows
// can bind it automatically the moment the device actually appears -- no
// second elevation, no second prompt. Returns false (logged, non-fatal to
// the caller's loop) on failure for a single PID; a phone that only ever
// negotiates one or two of the six doesn't need every one to succeed.
bool PrestageOnePid(unsigned short pid) {
    wdi_device_info device{};
    device.vid = kGoogleVid;
    device.pid = pid;
    device.desc = const_cast<char*>("PhoneCam USB Accessory");
    device.is_composite = FALSE;

    wdi_options_prepare_driver prepOpts{};
    prepOpts.driver_type = WDI_LIBUSBK;

    char tempPath[MAX_PATH];
    GetTempPathA(static_cast<DWORD>(sizeof(tempPath)), tempPath);
    char driverDir[MAX_PATH];
    std::snprintf(driverDir, sizeof(driverDir), "%sphonecam_driver_%04x", tempPath, pid);
    char infName[MAX_PATH];
    std::snprintf(infName, sizeof(infName), "phonecam_aoa_%04x.inf", pid);

    int rc = wdi_prepare_driver(&device, driverDir, infName, &prepOpts);
    if (rc != WDI_SUCCESS) {
        std::printf("  PID_%04X: wdi_prepare_driver failed: %s\n", pid, wdi_strerror(rc));
        return false;
    }

    char infPath[MAX_PATH];
    std::snprintf(infPath, sizeof(infPath), "%s\\%s", driverDir, infName);
    wchar_t infPathW[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, infPath, -1, infPathW, MAX_PATH) == 0) {
        std::printf("  PID_%04X: path conversion failed\n", pid);
        return false;
    }

    // SPOST_PATH: infPathW is already a full path to the .inf, no separate
    // source-media directory to search.
    if (!SetupCopyOEMInfW(infPathW, nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
        std::printf("  PID_%04X: SetupCopyOEMInfW failed: %lu\n", pid, GetLastError());
        return false;
    }

    std::printf("  PID_%04X: staged.\n", pid);
    return true;
}

// Pre-stages all six standardized AOA accessory PIDs. Called once, in the
// same elevated pass as InstallForPresentDevice, so the whole two-part
// binding (present-device install + accessory-mode pre-stage) only ever
// costs the user one UAC prompt total.
void PrestageAccessoryDrivers() {
    std::printf("Pre-staging AOA accessory-mode drivers (VID_%04X, PID_%04X..%04X):\n", kGoogleVid,
                kAccessoryPidLow, kAccessoryPidHigh);
    for (unsigned short pid = kAccessoryPidLow; pid <= kAccessoryPidHigh; ++pid) {
        PrestageOnePid(pid);
    }
}

void PrintUsage() {
    std::printf(
        "phonecam-usbdriver.exe --install --vid <hex> --pid <hex>\n"
        "  Installs a libusbK driver binding for the given, currently-attached,\n"
        "  non-composite USB device, then pre-stages drivers for all six\n"
        "  standardized AOA accessory-mode PIDs so the phone's post-handshake\n"
        "  re-enumeration also has a driver ready, with no second prompt.\n"
        "  Requires administrator privileges.\n"
        "\n"
        "Exit codes: 0 ok, 2 bad args, 11 device is composite (refused), 12 device\n"
        "not present, 13 install failed.\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool doInstall = false;
    unsigned short vid = 0, pid = 0;
    bool haveVid = false, havePid = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--install") == 0) {
            doInstall = true;
        } else if (std::strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            haveVid = ParseHex16(argv[++i], &vid);
        } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            havePid = ParseHex16(argv[++i], &pid);
        }
    }

    if (!doInstall || !haveVid || !havePid) {
        PrintUsage();
        return kExitBadArgs;
    }

    wdi_set_log_level(WDI_LOG_LEVEL_INFO);
    int rc = InstallForPresentDevice(vid, pid);
    if (rc != 0) return rc;

    // Only reachable after the present-device install succeeded -- order
    // matters here: that call is what creates and trusts the self-signed
    // cert (Root + TrustedPublisher stores) the accessory-mode packages
    // below get signed with too, via the same libwdi process-wide state.
    PrestageAccessoryDrivers();
    return 0;
}
