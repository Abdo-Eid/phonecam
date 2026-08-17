# Architecture

High-level map of the system and the state of each piece. For wire-level
detail see [`wire-protocol.md`](wire-protocol.md) and
[`control-protocol.md`](control-protocol.md).

## Overview

```
[Android APK]  Camera2 → MediaCodec H.264 (low-latency, zero-copy Surface)
                       │            (+ AudioRecord → Opus, Phase 8)
                       ▼
[USB transport]  MVP: ADB-forward socket   →   Phase 7: AOA (no USB-debugging)
                       ▼
[phonecam-host.exe]  MF H.264 decode → SharedFrameRing (shared memory)
                       ▲ created by
[phonecam-svc.exe]  LocalSystem service, owns ring creation only
                       ▼
[phonecam-vcam.dll]  MFCreateVirtualCamera media source → seen by ALL apps
                       ▲
[control channel]  bidirectional: PC → phone commands, phone → PC capabilities/telemetry
```

The phone captures and hardware-encodes video, streams it over USB, and
`phonecam-host.exe` decodes it and writes NV12 frames into a cross-process
shared-memory ring (created ahead of time by `phonecam-svc.exe` — see
"Elevation" below). `phonecam-vcam.dll` — a Media Foundation virtual camera
media source registered via `MFCreateVirtualCamera` — reads that ring and
serves frames to whichever app (or Windows service) has it open. Windows'
Frame Server bridges the same registered source to both Media Foundation
consumers (Chrome, Windows Camera, new Teams) and DirectShow consumers (Zoom,
classic OBS) — this is what makes "a real camera in every app" true rather
than an OBS-only plugin.

## Components

| Component | Build system | Role |
|---|---|---|
| `android/` | Gradle (Kotlin) | The installed phone app. `capture/CameraCapture.kt` + `encode/H264Encoder.kt` + `capture/CaptureController.kt` are the Phase 2 Camera2→MediaCodec pipeline (auto exposure/focus/WB only — manual controls deferred, see Phase 2 status below). `transport/`, `control/`, `service/` not yet started (Phase 3+). |
| `windows/common/` | CMake, static lib | Shared code between host and... conceptually vcam too, but see note below. Currently: `log/` (cross-process logging via stderr + `OutputDebugString`) and `shm/SharedFrameRing.h` (the frame-handoff contract). |
| `windows/host/` | CMake → `phonecam-host.exe` | `vcam_ctl/` calls `MFCreateVirtualCamera`/`Start`/`Remove`. `bridge/TestPatternProducer` is a Phase-1b dev tool standing in for the real H.264-decode bridge that Phase 3 adds. |
| `windows/vcam/` | **own MSBuild project** (`PhoneCamVCam.sln`/`.vcxproj`), not CMake | Forked from [VCamSample](https://github.com/smourier/VCamSample) (MIT) — see [`NOTICE.md`](../windows/vcam/NOTICE.md). The `IMFMediaSourceEx` COM media source. `FrameGenerator::Generate()` now reads the latest frame from `SharedFrameRing` (NV12→RGB32, blitted via the existing D2D1 render target) instead of the sample's built-in test pattern — everything else (D3D/CPU dual-path handling, NV12/RGB32 output, sample construction) is untouched. |
| `windows/svc/` | CMake → `phonecam-svc.exe` | A LocalSystem Windows Service whose only job is creating and holding open the `SharedFrameRing` — see "Elevation" below for why this exists as its own tiny component rather than living in `phonecam-host`. Supports `--install`/`--uninstall` (SCM API) and `--console` (runs the same logic as a plain process, for dev iteration without installing a real service). |
| `third_party/reference/VCamSample` | git submodule | Unmodified upstream, kept for comparison. |

**Why `vcam/` isn't a CMake target:** it needs NuGet-distributed WIL and
CppWinRT, which CMake doesn't naturally consume, and it's proven to build via
its own MSBuild project as-is. Since `phonecam-host` and `phonecam-vcam.dll`
are separate processes that talk *only* through `SharedFrameRing.h` (a
header-only contract both include directly), the two build systems never need
to interoperate. See `windows/CMakeLists.txt` for the build command.

## Status: Phase 1 complete (vertical slice proven)

Both checkpoints of the highest-risk item in the whole project — does a frame
actually reach a virtual camera that real apps render, through **both** the
Media Foundation and DirectShow paths — are done, with captured evidence:

- **Phase 1a** (stock, unmodified VCamSample, relocated and re-registered
  under our own CLSID): confirmed via a captured DirectShow frame
  (`ffmpeg -f dshow`) showing the sample's built-in gradient/text test
  pattern rendered as `Format: NV12 (CPU)`.
- **Phase 1b** (our fork, `FrameGenerator` reading from `SharedFrameRing`
  instead of drawing its own pattern; `phonecam-host` writing a synthetic
  animated frame via `TestPatternProducer`): confirmed via a captured
  DirectShow frame showing our host's actual scrolling-bar pattern, proving
  the full chain — host write → shared memory → vcam read → NV12→RGB32 →
  D2D1 → MF sample → Frame Server → DirectShow consumer.

## Status: Phase 2 complete (Camera2 → MediaCodec H.264, on-device proven)

`capture/CameraCapture.kt` (Camera2, `TEMPLATE_RECORD`, `SessionConfiguration`
API since minSdk 30 > 28) streams straight into `encode/H264Encoder.kt`'s
`MediaCodec` input `Surface` — zero-copy, no CPU frame ever exists.
`capture/CaptureController.kt` wires the two together and owns the output
file. Auto exposure/focus/white-balance only: manual controls (ISO, exposure
time, manual WB) are **deferred by request** — no need to run a
`CameraCapabilityProbe` for `MANUAL_SENSOR` presence until that scope is
picked back up (`docs/control-protocol.md`'s `SetManualExposure` etc. stay
defined in the schema, just unused for now).

**A real device finding, not a guess:** the initial encoder config (constrained-
baseline profile via `KEY_PROFILE`, plus the API-29+ `KEY_MAX_B_FRAMES`/
`KEY_LATENCY` low-latency knobs) made `MediaCodec.configure()` throw
`CodecException Error 0x80001001` ("unsupported setting") on this phone's
Snapdragon 665 vendor encoder — confirmed via on-device logcat, not
speculation. Likely cause: those newer format keys aren't honored by this
chip's older HAL, or `KEY_PROFILE` needs a paired `KEY_LEVEL` (a known
Qualcomm quirk). Fix was to strip to a minimal, maximally-portable config
(mime/size/color-format/bitrate/framerate/i-frame-interval/CBR-mode only) and
get that working before reintroducing anything else — the encoder's own
chosen default turned out to be **High profile**, not Baseline. Reintroducing
the low-latency knobs one at a time, with on-device verification each time,
is follow-up work whenever that matters (Phase 3 latency budget).

**Verified end-to-end on the real Redmi Note 8**, not just "it builds":
`.h264` pulled via `adb pull` and validated with `ffprobe`/`ffmpeg -f null -`
(full decode pass, zero errors) plus an extracted frame confirming real,
correctly-exposed camera content (not corrupted/garbage data) at both:
- **720p**: ~28-30fps sustained (climbs from ~15fps as the fixed camera-open
  + session-configure startup cost amortizes — a measurement artifact of
  `elapsed-time-since-start`, not a real throughput ramp).
- **1080p**: 29.7fps sustained (658 frames) — essentially the 30fps target,
  matching what 720p converges to. 720p is the current UI default (the more
  thoroughly measured of the two) but both resolutions are proven to work.

**Device-specific testing note:** this MIUI device blocks `adb shell input
tap` entirely (`SecurityException: Injecting to another application requires
INJECT_EVENTS permission`) — not just for system permission dialogs.
UI-driving automation isn't viable here; verification requires the user's
physical taps, paired with `android screen capture -a` (annotated
screenshots) and `android layout` for observation. Also: MIUI's own install
confirmation (separate from Developer Options → USB debugging) requires the
screen to be unlocked, and can silently report `INSTALL_FAILED_USER_RESTRICTED`
if the screen is asleep when `android run`/`adb install` fires.

Next: Phase 3 (USB transport — ADB-forward first — replacing
`TestPatternProducer` with the real H.264-decode bridge from this pipeline).

## Elevation: why `phonecam-svc` exists

`SharedFrameRing` must live in the `Global\` kernel-object namespace with an
SDDL granting access to `LocalService`/`LocalSystem` (not just the current
user) — this is required because Windows 11's Frame Server / Frame Server
Monitor services load `phonecam-vcam.dll` **in-process into themselves**,
running as `LocalService`/`LocalSystem` in **Session 0**, a different session
than the interactive host process. `Local\` (session-relative) objects are
invisible there, and the default DACL on an object created by the interactive
user doesn't grant those service SIDs access either.

**Creating a `Global\` object with a broadened (cross-session) DACL requires
`SeCreateGlobalPrivilege` — confirmed empirically:** the identical
`CreateFileMappingW` call with our SDDL returns `ERROR_ACCESS_DENIED` (5) from
a normal (non-elevated) process, even for an administrator account, because
UAC token-filtering disables that privilege until elevated; the same call
succeeds (`SeCreateGlobalPrivilege: Enabled`) once elevated.

**The key follow-up fact that shapes the design: only *creating* the object
requires the privilege.** Once it exists, a second, non-elevated process
attaching to the same name — via `OpenFileMapping`, or `CreateFileMapping`
with an existing name (which returns `ERROR_ALREADY_EXISTS`, not a failure)
— succeeds under a plain DACL check, no privilege needed. Confirmed
empirically: an elevated process created and held the mapping open; a
concurrent non-elevated process opened it successfully both ways
(`err=0` and `err=183`).

**Resolved: option 2 (Windows Service), implemented.** `phonecam-svc.exe`
is a tiny LocalSystem service (`windows/svc/`) whose *only* job is calling
`SharedFrameRing::OpenOrCreate()` on start and holding it open until stopped
— it has `SeCreateGlobalPrivilege` inherently, so no manual elevation is
needed at runtime. Everything else stays exactly where it was and exactly as
unprivileged as before:

- `phonecam-host.exe` (`vcam_ctl` + `TestPatternProducer`, soon the real
  decode bridge) now calls `SharedFrameRing::Open()` instead of
  `OpenOrCreate()`, and runs as a normal process — `MFCreateVirtualCamera`
  itself never needed elevation; only ring *creation* did.
- `phonecam-vcam.dll` is unchanged; it already only ever called `Open()`.

Verified end-to-end with the service installed (`phonecam-svc.exe --install`,
one-time, elevated) and running, then `phonecam-host.exe` launched as a
**plain, non-elevated process** — no UAC prompt at runtime — producing the
identical live-animated captured frame as the earlier elevated-host proof.

This was chosen over running `phonecam-host` elevated always (the simpler
but recurring-UX-cost alternative) because it matches the Phase 5 "tray app"
direction already in the roadmap — the elevated piece is a true
fire-and-forget background service (install once, `SERVICE_AUTO_START`,
invisible thereafter), while the process the user actually sees and
interacts with stays a normal app. `windows/svc/CMakeLists.txt`'s
`--console` mode (run the same ring-owning logic as a plain process) exists
purely so this doesn't require an install/start/stop cycle on every code
change during development.

**Still open for Phase 5:** the installer needs to run `phonecam-svc.exe
--install` (one elevated step, same shape as the existing vcam DLL
`regsvr32` registration) and `phonecam-host.exe` needs a startup retry/wait
for the service rather than failing immediately if it races the service on
boot — both are robustness polish, not architecture, so deferred to Phase 5
as planned.
