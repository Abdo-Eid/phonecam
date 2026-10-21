# Build

## Prerequisites

- Visual Studio 2022 Build Tools (Desktop development with C++) + Windows 11 SDK
- CMake, Ninja optional
- `adb` on PATH (Android platform-tools)
- `flatc` on PATH (winget `Google.flatbuffers`) -- used for FlatBuffers codegen from
  `proto/wire.fbs` and `proto/control.fbs` on both sides (see below)
- JDK 21, Android SDK (`ANDROID_HOME`) for the Gradle build
- `git submodule update --init --recursive` -- pulls in `third_party/reference/VCamSample`
  and `third_party/qr-code-generator` (Nayuki's MIT C++ QR encoder, pinned to
  `3c6d0b3cefb4e049dc337e82237c9644399716a8`), plus `third_party/flatbuffers` (the C++/Java FlatBuffers runtime, vendored because `flatc`'s
  installed version is newer than any published `flatbuffers-java` Maven artifact -- see
  `docs/architecture.md`'s Phase 4 section for why)
- Inno Setup (winget `JRSoftware.InnoSetup`) -- only needed to build the installer
  (`windows/installer/PhoneCam.iss`), not for day-to-day dev builds. Installs to
  `%LocalAppData%\Programs\Inno Setup 6\ISCC.exe`, not on PATH by default.

## Windows: host + common + svc (CMake)

```
cmake --build D:\work\phonecam\windows\build --target phonecam-host --config Debug
```

Other targets: `phonecam_common`, `phonecam-svc`. The control-protocol FlatBuffers headers
(`wire_generated.h`, `control_generated.h`) are generated automatically as part of the
`phonecam-host` build via a CMake custom command (`windows/host/CMakeLists.txt`) -- no manual
`flatc` invocation needed.

## Windows: vcam (MSBuild, not CMake)

`windows/vcam` is forked from VCamSample and needs the WIL/CppWinRT NuGet packages CMake doesn't
naturally consume, so it builds via its own MSBuild project:

```
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  D:\work\phonecam\windows\vcam\PhoneCamVCam.sln /p:Configuration=Debug /p:Platform=x64
```

**Deploying a vcam change:** the registered DLL
(`HKLM\SOFTWARE\Classes\CLSID\{d0255f4e-4471-47c4-91fc-0e74dcc93308}\InprocServer32`) points at
`C:\ProgramData\PhoneCam\phonecam-vcam.dll`, a separate deployed copy -- **not** the MSBuild
output. `PhoneCamVCam.vcxproj` now has a `PostBuildEvent` (Phase 5) that copies `$(TargetPath)`
over that ProgramData copy automatically on every build, best-effort (it silently no-ops if the
file is locked). So the remaining steps after a rebuild are just:

1. Rebuild via the MSBuild command above (the ProgramData copy refreshes automatically, unless
   locked -- see step 2-3 below).
2. Stop any running `phonecam-host.exe`.
3. If the PostBuildEvent's copy was skipped because the DLL was locked, restart both the
   `FrameServer` and `FrameServerMonitor` Windows services (elevated) to release it, then rebuild
   again.
4. Relaunch `phonecam-host.exe`.

## Windows: installer (Inno Setup)

The installer packages **Release** builds (static CRT, no VC++ redistributable dependency -- see
`docs/architecture.md`'s Phase 5 section), not the Debug builds the commands above produce:

```
cmake --build D:\work\phonecam\windows\build --target phonecam-host --config Release
cmake --build D:\work\phonecam\windows\build --target phonecam-svc --config Release
& "...\MSBuild.exe" D:\work\phonecam\windows\vcam\PhoneCamVCam.sln /p:Configuration=Release /p:Platform=x64
& "$env:LocalAppData\Programs\Inno Setup 6\ISCC.exe" D:\work\phonecam\windows\installer\PhoneCam.iss
```

Output: `windows\installer\Output\PhoneCamSetup.exe`. It bundles `adb.exe` (from
`third_party/platform-tools/`, vendored -- not resolved from PATH), so `phonecam-host.exe` works
standalone once installed, no PATH setup needed. Installing/uninstalling both require admin (the
installer requests elevation itself) since they touch `HKLM`, `ProgramData`, and the Windows
service.

**Verified on this machine, not on a fresh Windows account** (that's the installer's actual
roadmap exit criterion, and isn't verifiable here) -- see `docs/architecture.md`'s Phase 5
section for exactly what was and wasn't checked.

## Android

```
cd android
.\gradlew.bat assembleDebug
```

**Use PowerShell, not Bash**, for `gradlew.bat` in this environment -- a session hook redirects
Bash's `gradlew` invocations to a broken sandbox path missing `gradle-wrapper.jar` access.

The Kotlin FlatBuffers bindings are generated automatically as part of the build (a Gradle `Exec`
task in `android/app/build.gradle.kts` invoking `flatc --kotlin`) -- no manual step needed.

## Normal Wi-Fi use (tray GUI)

1. Run `phonecam-host.exe` normally, without flags. Open its tray menu.
2. Choose **Connect over Wi-Fi...** and scan the QR with PhoneCam's **Scan PC QR**.
3. The QR window closes after both sides save the pairing. A brief notification
   confirms actual video arrival. Next time, open both apps on the same network.

The phone hides pairing controls under **Connection settings** once paired.
**Stop** on either device turns off capture, keeps the control/pairing, and clears
the Windows output to black; **Start** resumes without rescanning. A deliberate
Stop survives app/host restart and automatic recovery. USB transport changes keep
the virtual camera registered. The QR is valid for two minutes; don't share it.

**Use code instead** opens the manual discovery/code dialog and offers RTP or TCP.
Optionally remembered manual codes and QR keys use Windows Credential Manager.
Both apps must speak **protocol v6**; rebuild/update them together.

## Developer wireless transport commands (RTP over UDP)

```
windows\build\host\Debug\phonecam-host.exe --rtp <phone-ip> [udp-port] --secret <code>
```

The phone's address is the one it shows on its Wi-Fi (or hotspot) connection; the
UDP port defaults to 27185 and is the port the *PC* binds. The control channel
goes to TCP 27184 on the same address. Both sides must be able to reach each
other — same Wi-Fi, or the phone joined to the PC's own Windows Mobile Hotspot
(one radio hop instead of two).

Windows' firewall blocks inbound connections on a "Public" network profile, so
allow `phonecam-host.exe` (or open the ports) when running over Wi-Fi.

## Wireless transport (RTP over UDP, or framed TCP over Wi-Fi)

```
windows\build\host\Debug\phonecam-host.exe --rtp            # find the phone, stream RTP
windows\build\host\Debug\phonecam-host.exe --rtp 192.168.1.5 # explicit address
windows\build\host\Debug\phonecam-host.exe --wifi            # framed TCP instead of RTP
windows\build\host\Debug\phonecam-host.exe --rtp --secret ABCD-EFGH-IJKL
windows\build\host\Debug\phonecam-host.exe --wifi --secret ABCD-EFGH-IJKL
```

With no address, the phone is found by discovery (it answers a broadcast probe),
so nothing has to be typed. `--wifi` uses the phone's framed TCP video socket
instead of RTP — useful where UDP is blocked, at the cost of TCP's retransmit
spikes.

Over a network the phone only accepts a PC that knows its **pairing code** (shown
in the phone's app). Pass it with `--secret` or the `PHONECAM_SECRET` environment
variable. The first three examples assume that environment variable is set.
ADB/AOA need no code; USB tethering uses network sockets and requires the code.
Both applications must speak protocol v6: network TCP video has its own
session-bound authentication prelude, with no unauthenticated fallback. Video
remains plaintext by explicit choice; see [security.md](security.md) for the
complete protection model and live-test procedure.

### Live UX regression (development only)

With the real phone already paired and a tray host running, build/install the debug
app and its Android test APK (never uninstall or clear app data):

```powershell
cd android
.\gradlew.bat assembleDebug assembleDebugAndroidTest testDebugUnitTest
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb install -r app\build\outputs\apk\androidTest\debug\app-debug-androidTest.apk
adb shell am instrument -w -e class io.github.abdoeid.phonecam.ConnectionUxTest#stopStaysStoppedAndHidesPairing io.github.abdoeid.phonecam.test/androidx.test.runner.AndroidJUnitRunner
# Restart the tray host and reopen the phone app; confirm it remains stopped.
adb shell am instrument -w -e class io.github.abdoeid.phonecam.ConnectionUxTest#explicitStartResumesRememberedConnection io.github.abdoeid.phonecam.test/androidx.test.runner.AndroidJUnitRunner
```

The first test exercises real Compose buttons, cancels Connecting, checks secondary
connection settings, waits four watchdog intervals after Stop, and recreates the
activity. The second requires Stopped, checks the simplified Streaming screen, and
tests Stop/Start again. Neither changes pairing keys. The Windows equivalent is
`scripts/test_stopped_gui.ps1` (Stop/Resume actions, black output, remembered reconnect,
automatic window dismissal, unchanged virtual-camera registration).

Windows' firewall blocks inbound connections on a "Public" network profile, so
allow `phonecam-host.exe` (or open the ports) when running over Wi-Fi.

## Comparing router Wi-Fi with the laptop hotspot

Keep the phone and laptop stationary, open PhoneCam, and keep USB/ADB connected
for metadata/pairing-code access (video itself uses Wi-Fi). From the repository root:

```powershell
.\scripts\measure_wireless_link.ps1 -Label router-wifi
# After this run finishes, join the phone to the laptop hotspot, then:
.\scripts\measure_wireless_link.ps1 -Label laptop-hotspot
```

Each run restarts only this repository's host, uses RTP/H.264 at 1920x1080,
30 fps, 6 Mbps, allows 15 seconds of warmup, and samples for 60 seconds.
The script records ICMP RTT percentiles while streaming, decoded frame delivery,
receiver loss-event counts, and available phone radio information. It leaves the
host running and writes JSON/raw logs under `windows/build/link-comparison/`.
Do not switch networks during a run. If startup produces no decoded frames, the
run fails rather than recording an invalid video benchmark.

ICMP RTT is **network round-trip**, not one-way video or glass-to-glass latency.
Receiver "lost" counts are loss/reassembly events, not an exact packet-loss
percentage. The host's reference-relative delay numbers are saved only as
diagnostics and must not be used to rank absolute baseline latency across runs.
Radio band, signal strength, congestion, and hotspot implementation can differ;
this compares the observed configurations, not an isolated router-hop effect.

## Checking the pairing handshake without the phone

```
windows\build\host\Debug\phonecam-host.exe --test-auth <code> <phone-nonce> <pc-nonce>
windows\build\host\Debug\phonecam-host.exe --test-video-auth <code> <phone-nonce> <pc-nonce> <video-nonce-hex>
```

Prints the HMAC the host would answer a pairing challenge with, so it can be
compared against an independent implementation (e.g. .NET's `HMACSHA256` over
`"<phone-nonce>|<pc-nonce>"` keyed with the code). It exists because the two
sides of the handshake must agree byte-for-byte, and a disagreement otherwise
looks exactly like a wrong pairing code.

## Verifying the virtual camera reaches DirectShow apps, not just Media Foundation ones

```
ffmpeg -f dshow -list_devices true -i dummy   # confirm the registered name
ffmpeg -f dshow -i "video=PhoneCam (Windows Virtual Camera)" -frames:v 1 out.png
```

The DirectShow name has " (Windows Virtual Camera)" appended -- `"PhoneCam"` alone won't match.
This is the discriminating check established in Phase 1: Zoom/OBS-classic use DirectShow, and
"works in Windows Camera" alone doesn't prove the MF→DirectShow Frame Server bridge is intact.
