# Contributing to PhoneCam

> **بالعربية:** [`CONTRIBUTING.ar.md`](CONTRIBUTING.ar.md)

PhoneCam turns an Android phone into a real USB webcam on Windows. It works end to end on real
hardware, and it is not finished. Both halves of that sentence matter — there is a working
pipeline to build on, and there is genuinely useful work left.

This guide covers what you need to know before your first change.

## Start with the current architecture

[`docs/architecture.md`](docs/architecture.md) summarizes the USB-only pipeline,
background-service rule, shared-memory/elevation boundary and known portrait
limitation. It links to the original development journal in Git history instead
of mixing obsolete implementations into current instructions.

Use [`docs/build.md`](docs/build.md) for build/deployment and tests,
[`plan.md`](plan.md) for remaining checks, and dated validation records for what
was actually observed. Update the relevant document alongside a behavior change.

## What the project is trying to be

- **USB only.** Tethering and USB debugging (ADB), not a wireless product.
- **No root, no vendor lock.** It has to work on an ordinary phone a normal person owns.
- **Free, with no paid tier.** No watermark, no feature held back.
- **Honest about its state.** The README and this site say what is unbuilt. Please keep it that way
  rather than describing something as done when it half-works.

## Repository layout

| Path | What it is |
| --- | --- |
| `android/` | Kotlin. Camera2 capture, MediaCodec encode, the transports, on-phone UI. |
| `windows/host/` | C++. `phonecam-host.exe` — transports, H.264 decode, tray UI and controls. |
| `windows/vcam/` | C++/COM. The `MFCreateVirtualCamera` media source, loaded by Windows' Frame Server. |
| `windows/common/` | Shared C++: cross-process frame ring, wire framing, logging. |
| `windows/svc/` | A tiny service that owns the shared-memory ring's lifetime. |
| `proto/` | FlatBuffers schemas — the contract both sides compile against. |
| `docs/` | Architecture log, protocol specs, build instructions, and this project's website. |

The Windows and Android halves **share no code**. Their only contract is the wire format in
`proto/` and the framing described in [`docs/wire-protocol.md`](docs/wire-protocol.md). If you
change either, you must change both sides and regenerate.

## Setting up

Full detail is in [`docs/build.md`](docs/build.md). In short you need Visual Studio 2022 Build
Tools with the C++ workload, the Windows 11 SDK, CMake, `flatc` and `adb` on `PATH`, and JDK 21
plus the Android SDK for the phone side.

```
git clone https://github.com/Abdo-Eid/phonecam
cd phonecam
git submodule update --init --recursive

cmake -S windows -B windows/build -A x64
cmake --build windows/build --target phonecam-host --config Debug
cd android && ./gradlew assembleDebug
```

Two things that catch everyone once:

1. **The virtual camera builds with MSBuild, not CMake**, because it needs NuGet packages CMake
   does not consume naturally.
2. **The registered DLL is a deployed copy**, not your build output. Rebuilding alone does not
   update what Windows loads. `docs/build.md` explains the deploy step and how to unstick it when
   the file is locked.

## Testing your change

Run the Android JVM checks and Windows local test executables:

```powershell
cmake --build windows/build --target phonecam-video-tests phonecam-media-settings-tests --config Debug
windows/build/host/Debug/phonecam-video-tests.exe
windows/build/host/Debug/phonecam-media-settings-tests.exe
cd android
.\gradlew.bat testDebugUnitTest assembleDebug assembleDebugAndroidTest
```

For offline measurement analyzers, run `python -m unittest discover -s bench -p
'test_*.py'` from the repository root. These automated checks do not replace
real-device transport/lifecycle validation; report what was and was not tested.
Use the targeted instrumentation commands in `docs/build.md`, not an unfiltered
device-test run that could include unrelated opt-in data-reset tests.

Some checks worth knowing:

```powershell
# Does the virtual camera reach DirectShow apps, not just Media Foundation ones?
# (Zoom's classic client and older OBS are DirectShow. "Works in Windows Camera" proves nothing.)
ffmpeg -f dshow -list_devices true -i dummy
ffmpeg -f dshow -list_options true -i video="PhoneCam (Windows Virtual Camera)"
ffmpeg -f dshow -i video="PhoneCam (Windows Virtual Camera)" -frames:v 1 -update 1 out.png
```

If you touch a transport, test it on real hardware and say which phone and which Android version.
Everything currently verified was verified on one device — a Redmi Note 8 on Android 11 — so a
report from different hardware is a genuinely useful contribution even with no code attached.

## Pull requests

- **One concern per pull request.** A transport fix and a UI change are two pull requests.
- **Say what you verified and what you did not.** "Built, not tested against a real phone" is a
  perfectly good thing to write, and much more useful than silence.
- **Update the docs in the same change.** If you fix something `docs/architecture.md` describes as
  broken, or make something it describes as impossible possible, change it there too.
- **Match the surrounding code.** This codebase comments the *why* — especially where something
  looks odd because of a real bug that was hit. Terse code with no explanation of a
  non-obvious choice will get review comments asking for it.
- **Preserve evidence when distilling history.** Link to the original commit and
  keep measured outcomes and caveats. Do not turn historical results into
  current-build claims or silently convert incomplete checks into passes.

## Code style

- **C++**: C++20, MSVC. Four spaces, `/W4` clean. Prefer the existing ADB and
  tethered-transport patterns before adding another abstraction.
- **Kotlin**: two spaces, standard Android conventions.
- **Comments explain why, not what.** The bar is: could someone six months from now tell whether
  this line is deliberate or accidental?

## Reporting bugs

Include your phone model and Android version, your Windows version, which transport was in use,
and the relevant output from `phonecam-host.exe`. A log line saying which transport was chosen is
usually the most useful single piece of information.

If it involves the virtual camera not appearing, say which app you tried it in — Media Foundation
and DirectShow apps fail differently and that distinction narrows things down fast.

## Licence

PhoneCam is [GPL-3.0-or-later](LICENSE). By contributing you agree your work is licensed the same
way.
