# Build

## Prerequisites

- Visual Studio 2022 Build Tools (Desktop development with C++) + Windows 11 SDK
- CMake, Ninja optional
- `adb` on PATH (Android platform-tools)
- `flatc` on PATH (winget `Google.flatbuffers`) -- used for FlatBuffers codegen from
  `proto/wire.fbs` and `proto/control.fbs` on both sides (see below)
- JDK 21, Android SDK (`ANDROID_HOME`) for the Gradle build
- `git submodule update --init --recursive` -- pulls in `third_party/reference/VCamSample`
  and `third_party/flatbuffers` (the C++/Java FlatBuffers runtime, vendored because `flatc`'s
  code generator/runtime versions must match; generated code references an
  exact runtime version rather than an arbitrary Maven release)
- Inno Setup (winget `JRSoftware.InnoSetup`) -- only needed to build the installer
  (`windows/installer/PhoneCam.iss`), not for day-to-day dev builds. Installs to
  `%LocalAppData%\Programs\Inno Setup 6\ISCC.exe`, not on PATH by default.

## Windows: host + common + svc (CMake)

```powershell
cmake -S windows -B windows/build -A x64
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
output. `PhoneCamVCam.vcxproj` has a `PostBuildEvent` that copies `$(TargetPath)`
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

The installer packages **Release** builds (static CRT, no VC++ redistributable
dependency), not the Debug builds the commands above produce:

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

The older installer was verified on the development machine, not a fresh Windows
account. The new USB-only installer has not been rebuilt/deployed in the current
validation run; see [deployment status](usb-validation-2026-11-08.md#deployment-status-checked-after-validation).

### Updating an installed Windows host

**Build, test, and install are separate operations.** Launching
`windows/build/host/Debug/phonecam-host.exe` tests that executable; it does not
replace `%ProgramFiles%\PhoneCam\phonecam-host.exe`, which the installed shortcut
normally launches. The installer also consumes **Release**, not Debug, output;
an old Release directory is not refreshed by a Debug build.

For a host-only update on a PC that already has the ring service and virtual
camera installed:

1. Rebuild `phonecam-host` with `--config Release` using the command above.
2. Exit PhoneCam through its tray menu. Do not run the installed and development
   hosts at the same time.
3. Back up the installed host executable, then use an elevated copy operation to
   replace it with `windows/build/host/Release/phonecam-host.exe`. Use a newly
   built Release host, not a stale binary or the Debug executable.
4. Compare source/destination SHA-256 hashes with `Get-FileHash`, then launch the
   installed shortcut and confirm the running executable path. Verify protocol
   v8 control and video against the updated phone app.

This host-only update does not require reinstalling the ring service or
re-registering the unchanged virtual-camera DLL. For a complete installation,
build all Release components and rebuild/run the Inno Setup package instead.
Building an installer alone still does not install it.

The Android disconnected-background fix changes only the phone app; it needs no
additional Windows code change. However, the earlier USB-only/protocol-v8 change
requires the matching new Windows host, not the older installed host.

## Android

```
cd android
.\gradlew.bat assembleDebug
```

Use PowerShell for the Windows `gradlew.bat` commands.

The Kotlin FlatBuffers bindings are generated automatically as part of the build (a Gradle `Exec`
task in `android/app/build.gradle.kts` invoking `flatc --kotlin`) -- no manual step needed.

## USB connections and tests

The host starts in automatic mode: it prefers a detected USB-tethered network
adapter and otherwise uses USB debugging (ADB). The Windows tray menu can select
either mode explicitly. On the phone, open PhoneCam and connect by either path:

- **USB tethering:** connect the cable and enable USB tethering. Windows uses its
  built-in RNDIS/NCM driver. The host finds the phone at the adapter gateway; no
  IP entry, QR scan, pairing, or custom driver is required.
- **USB debugging (ADB):** enable Developer Options → USB debugging, connect the
  cable, and authorize the host. The app's abstract sockets are reached via ADB
  forwarding.

The phone's tethered TCP listeners bind only to USB-tether interface addresses.
There is no application-level authentication, and control/video are plaintext;
use only a trusted host and link. See [security.md](security.md).

Both applications must speak **protocol v8**; rebuild/update them together.
Developer overrides are `phonecam-host.exe --net` (require tethering) and
`phonecam-host.exe --adb` (require ADB).

Available local checks:

```powershell
cd android
.\gradlew.bat testDebugUnitTest assembleDebug
cd ..
cmake --build windows/build --target phonecam-host phonecam-video-tests --config Debug
windows/build/host/Debug/phonecam-video-tests.exe
```

`TetheredTcpListenerTest` checks the interface-name allowlist. Device tests are
needed to validate tether interface detection and end-to-end USB behavior; an
attached phone is not assumed by the local build/test commands above.

Reference-device results and remaining limits are recorded in
[usb-validation-2026-11-08.md](usb-validation-2026-11-08.md).

### Android background lifecycle regression

Only active, connected video streaming may continue with the phone app hidden.
Leaving Ready, Connecting, or retry/error exits its service and notification;
disconnecting a hidden stream also exits instead of retrying in the background.
Reopen PhoneCam to connect again. This does not change the saved Stop intent.

With the Windows host stopped, run the device regression (no app-data reset):

```powershell
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb install -r android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb shell am instrument -w -e class io.github.abdoeid.phonecam.CaptureServiceLifecycleTest io.github.abdoeid.phonecam.test/androidx.test.runner.AndroidJUnitRunner
```

The tests cover idle, waiting for video, a real listener-bind failure (retry),
and deliberate Stop followed by UI unbind. A live host is needed separately to
verify that connected streaming continues when backgrounded and ends after
connection loss.

## Verifying the virtual camera reaches DirectShow apps, not just Media Foundation ones

```
ffmpeg -f dshow -list_devices true -i dummy   # confirm the registered name
ffmpeg -f dshow -i "video=PhoneCam (Windows Virtual Camera)" -frames:v 1 out.png
```

The DirectShow name has " (Windows Virtual Camera)" appended -- `"PhoneCam"` alone won't match.
This is the discriminating check established in Phase 1: Zoom/OBS-classic use DirectShow, and
"works in Windows Camera" alone doesn't prove the MF→DirectShow Frame Server bridge is intact.
