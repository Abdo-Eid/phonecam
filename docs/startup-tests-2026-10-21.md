# Startup and connection tests — 2026-10-21

Device: Redmi Note 8, Android 11 / MIUI. Windows Release tray host launched
normally, without transport command-line flags. Wi-Fi uses the laptop hotspot;
USB video uses ADB over the attached cable. Each passing case required decoded
video on the expected transport and a frame from the registered DirectShow camera.

| Stored state | Wi-Fi app first | Wi-Fi tray first | USB app first | USB tray first |
|---|---|---|---|---|
| Existing pairing/data | PASS | PASS | PASS | PASS |
| Cleared phone cache | PASS | PASS | PASS | PASS |
| Cleared app data on both ends | PASS | PASS | PASS | PASS |

All 12 combinations passed. Existing/cache USB results are in
`windows/build/startup-matrix/20261021-210146/`; the corrected Wi-Fi rerun is in
`20261021-210519/`; clean-data results are in `20261021-210619/`. The clean-data
run restored and verified the original phone data, pairing, Windows credentials
and settings. Fresh manual-code Wi-Fi setup dismissed its tray dialog and delivered
video in both startup orders. Camera permission was allowed manually after resets.

The native tray has no separate disk cache. Its persistent settings/credentials
are retained in cache cases and cleared in data cases. Wi-Fi existing/cache cases
use remembered QR pairing; clean-data Wi-Fi cases use the real tray's manual-code
fallback. ADB remains connected for test control during Wi-Fi cases, but passing
Wi-Fi cases must report RTP video, not USB fallback.

Two harness problems were corrected before counting Wi-Fi results: selecting one
ADB executable when several are installed, and avoiding PowerShell registry
`New-Item -Force`, which erased the remembered-PC settings and caused USB fallback.
These failed setup runs are not counted as Wi-Fi product failures.

Data-reset tests use the app's opt-in `DataResetTest` if MIUI denies shell clearing.
The user chose manual camera-permission taps, leaving USB security settings alone.
Original phone files/preferences and Windows settings/credentials are backed up
with current-user DPAPI encryption and restored with content verification.

UI: Scan PC QR / Use pairing code / Connect with USB are visible directly when
disconnected, even with remembered PCs. Verified visually on the real phone;
20 Android JVM tests pass, including the new visibility-policy regressions.

Limitations: this matrix does not test physical QR scanning, AOA, physical cable
unplug/replug, or a new camera open while the phone is asleep. An already-running
stream continuing through screen lock is established behavior, not a failed case.

Reproduction: see [build.md](build.md#startup-order-matrix-real-phone-and-tray).
