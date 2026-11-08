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
