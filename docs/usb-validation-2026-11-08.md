# USB-only validation — 2026-11-08

Tested the uncommitted USB-only working tree on `main` with a Redmi Note 8
(Android 11) and the Windows Debug host. The phone was updated with `adb install
-r`; no app data was cleared. Its initial saved intent was Stopped and was
restored after testing. `archive` remains at `253bd2d`.

| Check | Result |
|---|---|
| ADB control startup | PASS — protocol v8 capabilities and heartbeat replies |
| ADB video | PASS — host decoded 1920×1080; DirectShow consumed 90 frames at 30 fps |
| ADB Stop | PASS — persisted Stop and black, available virtual-camera output |
| USB tethering automatic selection | PASS — host selected the RNDIS gateway |
| Tethered control/video | PASS — both established connections used the RNDIS subnet, with an empty ADB forwarding list |
| Tethered video output | PASS — host decoded video; DirectShow consumed 90 frames at 30 fps |
| Phone listener addresses | PASS — phone logs show control and video bound to the IPv4 address of `rndis0`, not wildcard addresses |
| Wi-Fi reachability, before tethering | PASS — phone answered ping on Wi-Fi, but TCP ports 27183 and 27184 were unreachable while ADB video was running |
| Live transport switching | PASS — tethering → ADB → tethering streamed in one host process with only one virtual-camera registration |
| Tethered Stop/restart | PASS — no watchdog resume after Stop, black output, and automatic host restart retained Stopped |
| Android service Stop/unbind | PASS — `CaptureServiceLifecycleTest`, one test |

Video checks discarded frames rather than saving camera images. Black-output
checks inspected one frame in memory and found peak RGB 0. Diagnostic host logs
are local under `%LocalAppData%\Temp\opencode\phonecam-usb-validation`.

## Limits

- USB debugging stayed enabled for diagnostics. An empty forwarding list and
  established RNDIS connections demonstrate that the tethered stream did not
  use ADB; debugging-off operation was not separately exercised.
- Wi-Fi was not active during the tethered stream, so simultaneous Wi-Fi/tether
  isolation was not independently tested.
- Physical unplug/replug, tether-address changes, other OEM interface names,
  and the full Compose UI instrumentation suite were not tested in this run.
- These results do not imply encryption or authentication: tethered control and
  video remain plaintext with no application-level login.

## Follow-up: disconnected background shutdown

Validated the subsequent Android lifecycle change on the same phone:

- JVM background-eligibility checks and both Android APK builds passed.
- All four `CaptureServiceLifecycleTest` cases passed in three consecutive
  runs: idle, waiting for a video client, real video-listener bind failure
  (retry/error), and deliberate Stop. Each checks service exit and notification
  removal after unbind; the disconnected cases also check that exit does not
  latch a user Stop.
- Opening Ready with no host, then going Home, left no active PhoneCam service.
- Connected tethered streaming continued after Home: DirectShow consumed 90
  frames at 1080p/30. Reopening and backgrounding again retained the connected
  foreground service with no UI binding.
- Terminating the test host while the phone UI was hidden ended the phone
  service; the saved Stop intent remained false until test cleanup restored
  the initial Stopped state.

Device testing caught and fixed a late-notification race: notification updates,
foreground promotion and removal now share a lock, and disconnected/retired
services cannot repost notifications. Test logs are local under
`%LocalAppData%\Temp\opencode\phonecam-background-validation`. No app data was
cleared or camera images saved. These checks used Home/backgrounding and service
unbind, not a separate physical Recents swipe or screen-lock test.

## Deployment audit before installation

- **Phone:** the latest Debug app, including the background-shutdown fix, was
  installed in place. App data was preserved.
- **PC test executable:** tests launched
  `windows/build/host/Debug/phonecam-host.exe` directly from this checkout.
- **Installed PC app at that audit:** `C:\Program Files\PhoneCam\phonecam-host.exe`
  had **not yet been updated**. Its timestamp was 2026-10-22 and its SHA-256
  matched the old `windows/build/host/Release/phonecam-host.exe`, not the new
  Debug host. No PC installer or elevated host replacement was performed.
- The unchanged, previously installed Windows ring service and virtual-camera
  DLL were used by the development host during validation.

Those Debug-host live passes did not validate the then-installed PC copy. The
subsequent authorized installation below supersedes its deployment status.

## Installed Release host update

At the user's request, rebuilt the Release host from source revision `04ee348`
and replaced only `C:\Program Files\PhoneCam\phonecam-host.exe` with administrator
approval. Installed SHA-256 matches the new Release output:

```text
7924F2420492F505FF2265FF46F78C764EF49C4919EAD03C48B416545714CE4F
```

- Release video-pacing and media-settings test executables passed.
- The **installed executable**, launched with `--test-pattern`, delivered 90
  DirectShow frames at 1920×1080/30 fps without a phone or saved images.
- That test process exited, and the installed host was launched normally from
  `C:\Program Files\PhoneCam`; it was left running and waiting for the phone.
- The ring service and deployed virtual-camera DLL were unchanged, with hashes
  verified. Connection settings, service configuration and the COM registration
  snapshots were unchanged. No driver, bundled ADB, service/DLL reinstall or
  full Inno Setup package installation was performed.
- The old host executable was backed up and hash-verified. Backup and deployment
  manifest: `%LocalAppData%\Temp\opencode\phonecam-install-20261108-100806`.
- The phone was absent from ADB while a tether adapter was present. The phone
  application was not accepting connections; the user chose to finish without
  a live phone check. **Installed Release control/video verification remains
  pending**; do not substitute the earlier Debug-host results for it.

This is an in-place host-only update, not a newly validated complete installer
or a package-version bump. See [build.md](build.md#updating-an-installed-windows-host).
