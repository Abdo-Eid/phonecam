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
#include <wincrypt.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Every package this run (or a previous one) installed/staged is recorded
// here, one "oemNN.inf" name per line, so --revert-all knows exactly what to
// undo without having to guess or re-derive it -- see RecordInstalledPackage.
// %ProgramData%, not %LocalAppData%, since this helper only ever runs
// elevated and the file must be readable/deletable by a later elevated
// --revert-all run regardless of which interactive user triggered either.
const wchar_t* kStateDir = L"C:\\ProgramData\\PhoneCam";
const wchar_t* kStateFile = L"C:\\ProgramData\\PhoneCam\\usbdriver_packages.txt";

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

// Appends one "oemNN.inf vid pid" line to the state file (creating
// C:\ProgramData\PhoneCam if needed) -- best-effort: a failure to record is
// logged but never fails the install itself, since the driver binding
// already succeeded by the time this runs. Duplicate lines are harmless
// (revert just re-attempts an already-gone package, which
// SetupUninstallOEMInfW treats as success -- see RevertAll). vid/pid are
// recorded alongside the package name -- not just the name alone -- because
// removing a package from the driver store does NOT make an
// already-present device drop its current binding to it (confirmed live:
// a replug after SetupUninstallOEMInfW left the device still showing the
// now-deleted oem*.inf as its driver); reverting a *currently attached*
// device needs a separate, targeted per-device uninstall, which needs the
// hardware ID to find it again.
void RecordInstalledPackage(const wchar_t* oemInfName, unsigned short vid, unsigned short pid) {
    CreateDirectoryW(kStateDir, nullptr);  // ok if it already exists (GetLastError == ERROR_ALREADY_EXISTS)
    std::wofstream f(kStateFile, std::ios::app);
    if (!f) {
        std::wprintf(L"  (warning: could not open %ls to record %ls for later revert)\n", kStateFile, oemInfName);
        return;
    }
    f << oemInfName << L" " << vid << L" " << pid << L"\n";
}

// Looks up the "oemNN.inf" name Windows actually bound to a present device,
// by hardware ID -- needed because InstallForPresentDevice's own
// wdi_install_driver call does the SetupCopyOEMInfW-equivalent internally
// and doesn't hand the resulting name back. Reads it the same way `pnputil
// /enum-devices ... /drivers` does: SPDRP_DRIVER gives a driver registry
// key (e.g. "{class-guid}\0001"), and that key's "InfPath" value under
// HKLM\SYSTEM\CurrentControlSet\Control\Class is the published inf name.
bool GetBoundInfName(unsigned short vid, unsigned short pid, wchar_t* outInfName, size_t outInfNameCount) {
    wchar_t hwidPrefix[64];
    std::swprintf(hwidPrefix, 64, L"USB\\VID_%04X&PID_%04X", vid, pid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(devInfo, i, &data); ++i) {
        wchar_t hwid[512]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &data, SPDRP_HARDWAREID, nullptr,
                                                reinterpret_cast<PBYTE>(hwid), sizeof(hwid), nullptr)) {
            continue;
        }
        if (wcsncmp(hwid, hwidPrefix, wcslen(hwidPrefix)) != 0) continue;

        wchar_t driverKey[512]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &data, SPDRP_DRIVER, nullptr,
                                                reinterpret_cast<PBYTE>(driverKey), sizeof(driverKey), nullptr)) {
            continue;
        }

        std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
        regPath += driverKey;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;
        DWORD size = static_cast<DWORD>(outInfNameCount * sizeof(wchar_t));
        if (RegQueryValueExW(hKey, L"InfPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(outInfName), &size) ==
            ERROR_SUCCESS) {
            found = true;
        }
        RegCloseKey(hKey);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

// Forces any currently-present device matching vid/pid to drop its current
// driver binding, via DiUninstallDevice (newdev.h) -- NOT just removing the
// package from the driver store (SetupUninstallOEMInfW alone, confirmed
// live, leaves an already-bound device still pointing at the now-deleted
// package instead of falling back to its next-best driver, e.g. wpdmtp.inf,
// even across a physical replug). DiUninstallDevice un-assigns the device's
// current driver and lets PnP re-run driver selection for it immediately,
// which is what actually restores normal file-transfer/MTP behavior. A
// device that isn't currently present (the common case for a --revert-all
// run days later) is simply skipped here -- nothing to force, its next
// connection has no package left to bind to now that the store copy is
// gone, so ordinary PnP driver selection at that point picks wpdmtp.inf on
// its own.
void ForceUninstallDeviceDriver(unsigned short vid, unsigned short pid) {
    wchar_t hwidPrefix[64];
    std::swprintf(hwidPrefix, 64, L"USB\\VID_%04X&PID_%04X", vid, pid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &data); ++i) {
        wchar_t hwid[512]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &data, SPDRP_HARDWAREID, nullptr,
                                                reinterpret_cast<PBYTE>(hwid), sizeof(hwid), nullptr)) {
            continue;
        }
        if (wcsncmp(hwid, hwidPrefix, wcslen(hwidPrefix)) != 0) continue;

        BOOL needReboot = FALSE;
        if (DiUninstallDevice(nullptr, devInfo, &data, 0, &needReboot)) {
            std::wprintf(L"  Uninstalled current driver from present device %ls (PnP will re-select now)%ls\n",
                         hwidPrefix, needReboot ? L" -- a reboot may be needed" : L"");
        } else {
            std::wprintf(L"  DiUninstallDevice failed for %ls: %lu\n", hwidPrefix, GetLastError());
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
}

bool ParseHex16(const char* s, unsigned short* out) {
    if (!s) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 16);
    if (end == s || *end != '\0' || v > 0xFFFF) return false;
    *out = static_cast<unsigned short>(v);
    return true;
}

// Known Android-device VIDs, used only for --install's auto-detect mode
// (no --vid/--pid given). Deliberately a short, project-specific list, not
// a general OEM database -- matches "for now on my Note 8", not a
// commitment to support every Android phone brand. Xiaomi's own VID (this
// project's reference device's normal-mode VID) plus Google's (used by
// some devices even in normal mode, and always for the accessory-mode
// re-enumeration -- see kGoogleVid above).
constexpr unsigned short kXiaomiVid = 0x2717;

// Finds the currently-present device matching vid/pid (or, if both are 0,
// auto-detects the first present, non-composite device matching a known
// Android VID -- accessory-mode PIDs are excluded from the scan, since a
// device already in accessory mode isn't what this function installs for),
// refuses composite devices, generates and installs a self-signed libusbK
// driver package for it. Mirrors the exact shape already proven manually
// against the Note 8's accessory-mode node (see docs/architecture.md's
// "Resolved, not just diagnosed" section) -- same driver type, same
// wdi_prepare_driver + wdi_install_driver pair.
int InstallForPresentDevice(unsigned short vid, unsigned short pid) {
    const bool autoDetect = (vid == 0 && pid == 0);

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
        if (autoDetect) {
            bool knownVid = d->vid == kXiaomiVid || d->vid == kGoogleVid;
            bool isAccessoryPid = d->vid == kGoogleVid && d->pid >= kAccessoryPidLow && d->pid <= kAccessoryPidHigh;
            if (knownVid && !isAccessoryPid && !d->is_composite) {
                target = d;
                vid = d->vid;
                pid = d->pid;
                break;
            }
        } else if (d->vid == vid && d->pid == pid) {
            target = d;
            break;
        }
    }
    if (!target) {
        if (autoDetect) {
            std::printf(
                "No present, non-composite Android device found (checked VID_%04X and VID_%04X).\n",
                kXiaomiVid, kGoogleVid);
        } else {
            std::printf("Device VID_%04X&PID_%04X is not currently present.\n", vid, pid);
        }
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

    wchar_t oemName[MAX_PATH]{};
    if (GetBoundInfName(vid, pid, oemName, MAX_PATH)) {
        std::wprintf(L"  Bound as %ls -- recorded for later --revert-all.\n", oemName);
        RecordInstalledPackage(oemName, vid, pid);
    } else {
        std::printf(
            "  (warning: could not determine the published oem*.inf name -- --revert-all won't be able "
            "to undo this one automatically; it can still be removed manually via "
            "`pnputil /enum-devices /instanceid ... /drivers` to find the name, then "
            "`pnputil /delete-driver <name> /uninstall /force`)\n");
    }
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
    // source-media directory to search. destInfName captures the published
    // "oemNN.inf" name directly from the call that creates it -- simpler
    // than the registry lookup InstallForPresentDevice needs, since this is
    // a case where we control the copy ourselves instead of going through
    // wdi_install_driver's internal, opaque equivalent.
    wchar_t destInfName[MAX_PATH]{};
    DWORD destInfNameSize = MAX_PATH;
    wchar_t* destInfNameComponent = nullptr;
    if (!SetupCopyOEMInfW(infPathW, nullptr, SPOST_PATH, 0, destInfName, destInfNameSize, nullptr,
                           &destInfNameComponent)) {
        std::printf("  PID_%04X: SetupCopyOEMInfW failed: %lu\n", pid, GetLastError());
        return false;
    }

    std::printf("  PID_%04X: staged as %ls.\n", pid, destInfNameComponent ? destInfNameComponent : destInfName);
    RecordInstalledPackage(destInfNameComponent ? destInfNameComponent : destInfName, kGoogleVid, pid);
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

// Deletes every certificate in one system store whose subject was
// generated by our own installs -- libwdi's default cert_subject format
// (never overridden here) is exactly "CN=USB\VID_####&PID_####[&MI_##]
// (libwdi autogenerated)", which is specific enough that matching on the
// "(libwdi autogenerated)" suffix alone can't plausibly collide with an
// unrelated certificate already on the machine. Collects matches into a
// vector before deleting any of them, since CertEnumCertificatesInStore's
// contract is that continuing enumeration with a just-deleted context is
// undefined.
void RemoveSelfSignedCertsFromStore(const wchar_t* storeName) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                      CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, storeName);
    if (!store) return;

    std::vector<PCCERT_CONTEXT> toDelete;
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
        wchar_t subject[512]{};
        if (CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, 512) > 1 &&
            wcsstr(subject, L"(libwdi autogenerated)") != nullptr) {
            toDelete.push_back(CertDuplicateCertificateContext(cert));
        }
    }
    for (PCCERT_CONTEXT c : toDelete) {
        wchar_t subject[512]{};
        CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, 512);
        std::wprintf(L"  Removing certificate '%ls' from '%ls' store\n", subject, storeName);
        CertDeleteCertificateFromStore(c);  // also frees c, regardless of success
    }
    CertCloseStore(store, 0);
}

// Forces a full USB rescan, the API equivalent of `pnputil /scan-devices` --
// confirmed live this was the missing piece after DiUninstallDevice:
// removing a device's current driver via DiUninstallDevice doesn't just
// unbind it, it can remove the devnode entirely (the physically-still-
// attached phone briefly vanished from `pnputil /enum-devices /connected`
// entirely, not just lost its driver), and Windows does not always
// re-discover an already-connected device on its own afterward. Re-
// enumerating the root devnode is what makes Windows immediately re-scan
// and rebuild it -- confirmed live this alone (no physical replug) restored
// the device with Driver Name: wpdmtp.inf.
void TriggerFullPnpRescan() {
    DEVINST rootNode = 0;
    if (CM_Locate_DevNodeW(&rootNode, nullptr, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return;
    CM_Reenumerate_DevNode(rootNode, CM_REENUMERATE_SYNCHRONOUS);
}

// Undoes everything --install (across every run, not just the most recent
// one) has done: force-uninstalls the driver from any currently-present
// device that was one of ours, uninstalls every recorded driver package
// from the store, removes the self-signed certificates from both trust
// stores, and clears the state file.
int RevertAll() {
    std::wifstream f(kStateFile);
    if (!f) {
        std::printf("No recorded driver packages to revert (nothing installed yet, or already reverted).\n");
    } else {
        std::wstring line;
        int count = 0, failed = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::wstringstream ss(line);
            std::wstring oemName;
            unsigned int vid = 0, pid = 0;
            ss >> oemName >> vid >> pid;
            if (oemName.empty()) continue;

            // Present-device case first: a package removed from the store while a device is
            // still actively bound to it doesn't drop that binding on its own -- confirmed live,
            // a replug alone left the device pointing at the just-deleted package instead of
            // falling back to its normal driver. Only meaningful if vid/pid parsed (older or
            // malformed lines just skip this and fall through to the package-only uninstall).
            if (vid != 0 && pid != 0) {
                ForceUninstallDeviceDriver(static_cast<unsigned short>(vid), static_cast<unsigned short>(pid));
            }

            std::wprintf(L"Uninstalling %ls... ", oemName.c_str());
            if (SetupUninstallOEMInfW(oemName.c_str(), SUOI_FORCEDELETE, nullptr)) {
                std::printf("ok\n");
            } else {
                DWORD err = GetLastError();
                // Already gone (e.g. a duplicate line, or manually removed) counts as success --
                // revert's job is "make sure it's not there", not "prove it was there to begin with".
                if (err == ERROR_FILE_NOT_FOUND || err == ERROR_INF_IN_USE_BY_DEVICES) {
                    std::printf("already gone or in use, skipping\n");
                } else {
                    std::printf("failed: %lu\n", err);
                    ++failed;
                }
            }
            ++count;
        }
        std::printf("Processed %d recorded package(s), %d failure(s).\n", count, failed);
    }

    std::printf("Removing self-signed certificates...\n");
    RemoveSelfSignedCertsFromStore(L"Root");
    RemoveSelfSignedCertsFromStore(L"TrustedPublisher");

    DeleteFileW(kStateFile);

    std::printf("Rescanning for hardware changes...\n");
    TriggerFullPnpRescan();

    std::printf(
        "Done. A device shown above as 'Uninstalled current driver' should now be back to its normal\n"
        "driver (e.g. file-transfer/MTP) with no replug needed -- confirmed live: DiUninstallDevice can\n"
        "remove the devnode entirely, not just unbind it, and this rescan is what makes Windows\n"
        "immediately rediscover an already-connected device rather than leaving it briefly invisible.\n"
        "If it still looks wrong, unplug and replug it once.\n");
    return 0;
}

void PrintUsage() {
    std::printf(
        "phonecam-usbdriver.exe --install [--vid <hex> --pid <hex>]\n"
        "  Installs a libusbK driver binding for a currently-attached, non-composite\n"
        "  USB device, then pre-stages drivers for all six standardized AOA\n"
        "  accessory-mode PIDs so the phone's post-handshake re-enumeration also has\n"
        "  a driver ready, with no second prompt. With no --vid/--pid, auto-detects\n"
        "  the first present, non-composite device matching a known Android VID.\n"
        "  Requires administrator privileges.\n"
        "\n"
        "phonecam-usbdriver.exe --revert-all\n"
        "  Undoes every driver package and self-signed certificate this tool has\n"
        "  ever installed (recorded in C:\\ProgramData\\PhoneCam), restoring normal\n"
        "  file-transfer/MTP behavior. Requires administrator privileges.\n"
        "\n"
        "Exit codes: 0 ok, 2 bad args, 11 device is composite (refused), 12 device\n"
        "not present, 13 install failed.\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool doInstall = false;
    bool doRevert = false;
    unsigned short vid = 0, pid = 0;
    bool haveVid = false, havePid = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--install") == 0) {
            doInstall = true;
        } else if (std::strcmp(argv[i], "--revert-all") == 0) {
            doRevert = true;
        } else if (std::strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            haveVid = ParseHex16(argv[++i], &vid);
        } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            havePid = ParseHex16(argv[++i], &pid);
        }
    }

    if (doRevert) {
        return RevertAll();
    }

    // --vid/--pid are optional: given neither, InstallForPresentDevice auto-detects the
    // first present, non-composite device matching a known Android VID. Given one but not
    // the other is treated as a usage error (an incomplete manual override, not a request
    // to auto-detect).
    if (!doInstall || (haveVid != havePid)) {
        PrintUsage();
        return kExitBadArgs;
    }
    if (!haveVid) {
        vid = 0;
        pid = 0;
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
