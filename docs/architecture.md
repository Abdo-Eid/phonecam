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

## Status: Phase 3A complete (MF H.264 decode, proven offline)

`windows/host/decode/MFH264Decoder.*` wraps the built-in Windows Media
Foundation software H.264 decoder MFT (`CLSID_CMSH264DecoderMFT`, the
synchronous, non-D3D one) and turns Annex-B NALUs into packed NV12 frames
ready for `SharedFrameRing::WriteFrame`. Proven **fully offline** — no phone,
no transport — by feeding it the same 720p and 1080p scratchpad `.h264`
captures already validated in Phase 2, dumping a decoded frame's raw NV12,
and eyeballing it via `ffmpeg`-converted PNG against the known-good frame
(`phonecam-host.exe --test-decode <in.h264> <out.nv12>`, a dev-only CLI mode
added to `main.cpp`). This sequencing — hardest/newest piece first, entirely
decoupled from the phone — came out of an advisor consult before writing any
Phase 3 code, specifically to keep iteration fast (seconds, not a phone
round-trip) while pinning down several genuinely finicky, recall-resistant
MF behaviors empirically rather than guessing:

- **`ProcessOutput` returns `MF_E_TRANSFORM_TYPE_NOT_SET`, not only
  `MF_E_TRANSFORM_STREAM_CHANGE`, before an output type has ever been set.**
  Only reacting to `STREAM_CHANGE` (the commonly-documented case) left the
  output type forever unnegotiated, which in turn starved `ProcessInput`
  (its internal queue never drained, so it started returning
  `MF_E_NOTACCEPTING` on every subsequent feed) — a silent full-pipeline
  stall with no frames ever decoded. Fix: treat both HRESULTs identically —
  (re)negotiate the output type and retry.
- **`MF_MT_FRAME_SIZE` on the negotiated output type is the padded, macroblock-
  aligned *coded* size, not the true display size** — confirmed directly:
  1920x1080 input negotiated as `1920x1088` (1088 = next multiple of 16).
  Trusting it as-is would have overflowed `SharedFrameRing::kMaxFrameBytes`
  (sized for 1920x1080) and silently dropped every 1080p frame. Fix: read
  `MF_MT_MINIMUM_DISPLAY_APERTURE` (a `MFVideoArea` blob) off the same media
  type and prefer its `Area.cx`/`Area.cy` when present — that's the real
  crop rectangle. 720p never exposed this (1280x720 is already a multiple of
  16 in both dimensions), which is exactly why 1080p was worth testing
  separately rather than assuming one resolution generalizes.
- **The chroma (UV) plane starts after the *coded* (padded) height's worth of
  luma rows, not the display height's** — true for both the `IMF2DBuffer`
  path (queried pitch) and the flat-buffer path (implied pitch == width).
  Getting this wrong doesn't crash or shear; it reads a few rows of luma
  padding as chroma, which rendered as a thin green band across the top of
  every 1080p frame — caught by visually diffing the decoded PNG against the
  known-good Phase 2 frame, not by any error return. Fixed by tracking the
  coded height separately from the display height and using it specifically
  for the UV-plane offset calculation.
- **`IMF2DBuffer` is not implemented at all by this decoder's client-
  allocated output buffers** (`MFCreateMemoryBuffer`, the path exercised
  whenever `MFT_OUTPUT_STREAM_PROVIDES_SAMPLES` isn't set, which is the case
  here) — confirmed empirically (`E_NOINTERFACE` on every single sample, not
  intermittent). The code tries the 2D path first and falls back to a flat
  `Lock()` with an implied packed pitch; today that fallback is the only
  path actually exercised. The 2D attempt stays in because Phase 6's planned
  D3D11-backed decode path will need it — this is a real future consumer,
  not speculative dead code.

`TestPatternProducer` is still what `main.cpp` runs by default; Phase 3B/C
wire the real transport and swap it for this decoder.

## Status: Phase 3B complete (live USB transport, proven independently of the decoder)

Android: `transport/VideoSocketServer.kt` (`LocalServerSocket` on
`phonecam_video`) + `transport/WireFraming.kt` (the 20-byte header) replace
`CaptureController`'s old `FileOutputStream` sink — nothing is written to
phone storage anymore. Capture only starts once a PC actually connects
(`accept()` blocks first); the UI shows "Waiting for PC connection..." until
then. Windows: `transport/AdbTransport.*` runs `adb forward` and connects as
a TCP client, per `docs/wire-protocol.md`.

Proven with a standalone `--test-transport <out.h264>` CLI mode (parallel to
`--test-decode`): strips the 20-byte header from each received packet and
appends the raw payload straight to a file, deliberately not touching
`MFH264Decoder` — isolates "is the framing byte-correct" from "does the
decoder work," matching the same one-variable-at-a-time discipline as Phase
3A. Verified against the real phone over live USB: 1886 frames / 63
keyframes / 11,869,439 bytes received in real time, reassembled file decodes
with **zero ffmpeg errors**, frame count matches exactly (1886), and a
extracted mid-stream frame is correct, uncorrupted camera content.

**Found and fixed a real reentrancy bug, caught by the user hitting it
live:** tapping Stop crashed the app (`IllegalStateException` at
`MediaCodec.releaseOutputBuffer`, confirmed via `adb logcat -d -b crash`).
Root cause: the main thread's `CaptureController.stop()` closes the video
socket, which makes the encoder drain thread's in-flight `out.write()` throw
`IOException`; that write-error handler synchronously re-enters
`CaptureController.stop() -> H264Encoder.stop()` **from the drain thread
itself**, racing the main thread's own still-in-progress call to the same
methods — both paths could reach `codec.release()`/`codec.releaseOutputBuffer()`
on an already-released codec. Fixed by (1) not touching `codec` at all after
invoking the write-error callback, since that callback can tear the codec
down synchronously, and (2) making both `CaptureController.stop()` and
`H264Encoder.stop()` idempotent via an `AtomicBoolean` CAS guard, so a
reentrant or concurrent second call is a safe no-op regardless of which
thread wins the race. This is the same class of bug as the self-join guard
added earlier (a background error callback able to call back into the
object that's calling it) — worth remembering as a recurring shape any time
an error callback can trigger teardown of the object invoking it.

**Known cosmetic follow-up, not fixed:** a deliberate Stop can occasionally
still show a flash of "Error: ..." instead of settling on Idle, because the
drain thread's write failure fires the error callback independently of the
main thread's own state update -- doesn't affect the stream or crash, low
priority.

**A second, distinct crash found the same way (real device usage, not
reasoning about the code):** rapidly tapping Cancel then Start again threw
an uncaught `IOException("Address already in use")` straight out of
`VideoSocketServer`'s constructor -- `CaptureController.start()` bound a new
`LocalServerSocket("phonecam_video")` eagerly and unguarded, and the OS
hadn't finished releasing the just-closed previous session's binding on the
same abstract name yet. Fixed by moving the bind off the constructor into an
explicit `open()` that retries briefly (10 attempts, 50ms apart), and moving
the whole socket lifecycle (bind + accept) onto the background thread so
`start()` never blocks or crashes the caller. Same root shape as the other
two fixes above: an operation that can fail transiently was being treated as
infallible.

## Status: Phase 3C complete (live video end-to-end, verified via DirectShow)

`bridge/LiveVideoBridge` ties B and A together -- `AdbVideoTransport`
receives packets, `MFH264Decoder` decodes them, each decoded frame goes
straight into `SharedFrameRing::WriteFrame` -- and is now `main.cpp`'s
default frame source, replacing `TestPatternProducer` (kept as an opt-in
`--test-pattern` dev tool). Verified with the same discriminating check used
since Phase 1: `ffmpeg -f dshow` against `"PhoneCam (Windows Virtual
Camera)"` (the actual registered DirectShow name -- not just `"PhoneCam"`)
while the phone was live-streaming, producing a clean, correctly-proportioned,
undistorted frame of real camera content -- the DirectShow path is what
proves "seen by every app," not just Media Foundation-native ones.

**Two real findings, not guesses:**

- **A hardcoded resolution mismatch, inherited from Phase 1's
  `TestPatternProducer` setup:** `windows/vcam/MediaStream.cpp` declared its
  `MF_MT_FRAME_SIZE` as a fixed `1280x960` (`NUM_IMAGE_COLS`/`NUM_IMAGE_ROWS`,
  left over from the Phase 1b test-pattern resolution). `FrameGenerator`
  already sized its D2D1 bitmap correctly to whatever's actually in the ring
  (720p from the phone), but that bitmap then gets drawn into a canvas
  declared 960 tall to consumers -- a 1.33x vertical stretch, confirmed by
  capturing a frame and visually comparing proportions against the known
  scene before and after. Fixed by changing the constants to `1280x720` to
  match the phone's actual current default. **Static for now** -- true
  dynamic resolution (matching a PC-driven `SetResolution`) is Phase 4 scope,
  once that control actually exists.
- **The registered vcam DLL is a *deployed copy*, not the build output:**
  `HKLM\SOFTWARE\Classes\CLSID\{d0255f4e-...}\InprocServer32` points at
  `C:\ProgramData\PhoneCam\phonecam-vcam.dll`, a copy made once during Phase
  1's `regsvr32` registration -- completely separate from
  `windows\vcam\x64\Debug\phonecam-vcam.dll`, the actual MSBuild output.
  Rebuilding the project alone does **not** update what the Frame Server
  loads; restarting `FrameServer`/`FrameServerMonitor` alone doesn't either,
  if the stale file was never replaced. Any vcam-side code change needs all
  three steps: rebuild, copy the output over the deployed path (needs
  elevation -- the file is held open by the Frame Server, so the *running*
  host process must be stopped and both services restarted *before* the
  copy, not just after), then relaunch `phonecam-host.exe`. **Worth a real
  fix in Phase 5's installer work** (either always deploy from a single
  source of truth, or have the dev build target write directly to the
  registered path) -- documented here so this multi-step dance doesn't have
  to be rediscovered from scratch next time a vcam change needs testing.

Phase 3's overall exit criterion -- live phone camera visible in all 4 app
types over USB -- has its hardest leg (DirectShow) confirmed; Windows
Camera/Chrome/Zoom were already proven reachable via the same registered
source in Phase 1 with synthetic content, and now carry real content through
the identical path. Glass-to-glass latency measurement (the <150ms target)
is deferred to when the Phase 2-deferred encoder low-latency knobs get
revisited, per the original plan -- it needs the visual on-screen-timer
method, not a blocker for this phase's core proof.

**Front camera also verified working**, and surfaced a real, undocumented
gap: **no sensor-orientation correction exists anywhere in the pipeline.**
`CameraCapture` now takes a `lensFacing` parameter (`pickCameraId(lensFacing)`
replaces the old back-only `pickBackCameraId()`) -- straightforward, since
lens switching was already planned Phase 4 scope, just pulled forward for
this test. Confirmed empirically by capturing three frames at three phone
orientations (front camera): held vertically, the image comes out rotated
90°; rotated 90° left from vertical, the image is upright/correct; rotated
90° right, the image is fully upside down. This is internally consistent
with a single fact -- Camera2 delivers frames in the sensor's fixed native
orientation regardless of how the phone is physically held, and **nothing
in this pipeline (`CameraCapture` → `H264Encoder` → wire → `MFH264Decoder`
→ ring → vcam) applies any rotation correction today.** The back camera
was never tested at more than one orientation before now, so this was never
exercised. Real fix belongs with per-lens `SENSOR_ORIENTATION` handling
(Camera2 exposes it per camera ID) applied either at encode time (rotate
before the encoder sees it) or as metadata the PC side reads and corrects
for -- not scoped/decided yet; flagging for whenever orientation correction
gets picked up, likely alongside Phase 4's lens-switch control.

**Temporary dev-only scaffolding added this session, needs a keep-or-revert
decision before Phase 4/release** (each is clearly marked `TEMPORARY` at
its source location):

- `MainScreenViewModel.startCapture()` defaults `lensFacing` to
  `LENS_FACING_FRONT` (was back). Added for the orientation test above.
- `MainScreen.kt` auto-starts capture whenever state is `Idle` and
  auto-retries (after a 1.5s delay) on `Error`, instead of requiring a
  manual tap on the Start/Retry button. Added because this MIUI device
  blocks `adb shell input tap` entirely (see the Phase 2 status section
  above), so every rebuild+install+relaunch cycle -- and every PC-side
  hiccup mid-test -- otherwise needs the user's physical tap. Whether to
  keep some form of this (e.g. auto-start behind a foreground service is
  already the Phase 5 direction anyway) or fully revert to manual-only
  control is a real product decision, not just a "put it back" -- judge on
  merit when picked back up, per explicit user direction.

Phase 3's overall exit criterion -- live phone camera visible in all 4 app
types over USB -- has its hardest leg (DirectShow) confirmed; Windows
Camera/Chrome/Zoom were already proven reachable via the same registered
source in Phase 1 with synthetic content, and now carry real content through
the identical path. Glass-to-glass latency measurement (the <150ms target)
is deferred to when the Phase 2-deferred encoder low-latency knobs get
revisited, per the original plan -- it needs the visual on-screen-timer
method, not a blocker for this phase's core proof.

Next: Phase 4 (control channel + capability-driven UI, using the
already-defined `control.fbs` schema -- manual exposure controls stay
deferred per earlier request). Before that, worth deciding: keep or revert
the temporary auto-start/front-camera scaffolding above, and whether
orientation correction should land alongside Phase 4's lens-switch control
or get its own pass.

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

## Status: Phase 4 complete (control channel, verified against the real device)

Second adb-forwarded channel (`phonecam_control`, port 27184) alongside
video, framed per `docs/wire-protocol.md` (4-byte LE length prefix +
FlatBuffers `ControlEnvelope`, `proto/control.fbs`). Phone sends
`CapabilityDescriptor` once on connect; PC sends commands with a
monotonic `cmd_id`, phone replies `Ack` and pushes `CurrentSettings` after
every applied change, matching the reconciliation discipline in
`docs/control-protocol.md`.

**Scope decisions made before writing any code (see also the advisor
consult that shaped this phase):**

- **Console command surface, not a GUI.** The host had zero UI
  infrastructure (it's a pure console app) and picking a GUI toolkit
  (WinUI3/Win32/ImGui) would import Phase 5's tray-app scope into Phase 4
  for no verification benefit. `control/ConsoleControlUi` is a
  keyboard-driven surface generated from the received
  `CapabilityDescriptor` -- it satisfies "every advertised control visibly
  changes state" without the toolkit decision. A future GUI reads from the
  same `ControlChannel` API.
- **`SetResolution` and orientation correction are explicitly out of
  scope**, and it's one decision, not two: `windows/vcam/MediaStream.cpp`
  advertises a single fixed stream size (the Phase 3C finding), so neither
  a resolution change nor a 90°/270° rotation (which changes the frame's
  aspect) can be delivered to the virtual camera without vcam work that
  doesn't exist yet. `CameraCapabilityProbe` deliberately leaves
  `LensCapabilities.resolutions` empty rather than populate a list nothing
  can act on yet.
- **Manual exposure/white-balance/stabilization commands are wired in the
  schema but not implemented on the phone side** -- `ControlChannel.kt`'s
  `dispatchToListener` falls through to `Ack{Unsupported}` for
  `SetManualExposure`/`SetAutoExposure`/`SetWhiteBalanceMode`/
  `SetManualWhiteBalance`/`SetStabilization`/`SetResolution`/`SetFps`/
  `Start`/`Stop`. This matches the Phase 2 decision to defer manual
  controls, now revisited with real data (see below) but still deferred by
  choice, not by capability.

**FlatBuffers codegen needed vendoring on both sides, not just Maven/apt.**
`flatc` (winget `Google.flatbuffers`) is version 25.12.19, newer than any
`flatbuffers-java` release on Maven Central (latest published: 25.2.10) --
the generated Kotlin code's `Constants.FLATBUFFERS_25_12_19()`
version-marker call won't resolve against an older runtime jar. Fixed by
adding `third_party/flatbuffers` as a git submodule (shallow clone; full
history timed out) and compiling its C++ headers (`windows/host/CMakeLists.txt`)
and its Java runtime source (`android/app/build.gradle.kts`'s
`sourceSets["main"].java.srcDir(...)`) directly, guaranteeing an exact
version match on both sides instead of chasing a moving Maven target. Both
builds invoke `flatc` on `proto/wire.fbs` *and* `proto/control.fbs`
together (not just the latter) -- passing only the included schema silently
produced a `control_generated.h`/`.kt` that `#include`s/imports a
`wire_generated` file flatc never wrote, a real "works on the schema author's
machine" trap worth flagging for anyone reproducing this.

**Real device capability probe -- overturns an earlier assumption.**
`CameraCapabilityProbe` (Android) queries actual `CameraCharacteristics` for
every lens and encodes them into `CapabilityDescriptor`; the PC printed the
real result from the Redmi Note 8:

```
device=Redmi Note 8 android=11 protocol_version=1 lenses=2
  [0] camera_id=0 facing=Back  hw_level=3 (LEVEL_3) manual_sensor=1 torch=1
      zoom=[1.00,8.00] ev=[-24,24]*0.167
      af_modes=Auto Off Macro Continuous  awb_modes=Auto Incandescent Fluorescent Daylight CloudyDaylight Twilight Shade Off
  [1] camera_id=1 facing=Front hw_level=3 (LEVEL_3) manual_sensor=1 torch=0
      zoom=[1.00,8.00] ev=[-24,24]*0.167
      af_modes=Off  awb_modes=(same as back)
```

This directly contradicts the Phase 0 planning assumption ("this
`LIMITED`-hardware-level phone very likely lacks `MANUAL_SENSOR`" --
`docs/architecture.md`'s own risk R9 and the project memory file). The
device is actually `LEVEL_3` with `MANUAL_SENSOR` on *both* lenses. Manual
exposure/ISO/shutter controls were deferred by choice in Phase 2 for scope
reasons, not because they're unavailable -- worth revisiting in a later
phase now that real data says they'd work. The front lens's `af_modes=Off`
(no autofocus at all) is a genuine hardware limitation, correctly reflected
so the (future) UI won't offer focus mode or tap-to-focus on that lens.

**Verified live against the device**, using a scripted console-command
sequence (`ConsoleControlUi`'s stdin loop driven by a PowerShell-controlled
child process, since this MIUI device still blocks `adb shell input tap`
and there's no GUI to tap anyway):

- `SetLens` → `Ack{Ok}` → `LensChanged` → `CurrentSettings` reflecting the
  new `camera_id`: switched back↔front repeatedly (8 switches across 4
  round trips) with zero crashes after the fix below.
- `SetZoomRatio`, `SetEv`, `SetTorch` (on and off), `SetFocusMode`: each
  produced `Ack{Ok}` and a `CurrentSettings` push whose value matches what
  was sent -- and critically, `CurrentSettings` is generated from the
  *actual* Camera2 `setRepeatingRequest` success (`ackFor` in
  `CaptureController.kt` only pushes `CurrentSettings` when
  `CameraCapture.setX(...)` returned `true`), not an optimistic echo, so
  this is evidence the control genuinely reached the sensor, not just that
  a byte was parsed correctly.
- `RequestKeyframe`, `SetBitrate`, `TapToFocus`: implemented via the
  identical dispatch path as the above (verified) commands, but live
  round-trip capture for these three specifically was unreliable to
  automate -- see the tooling note below. Code-reviewed instead; no
  additional risk beyond the two bugs already found and fixed (below).
- Ack{Busy} correctly returned when a command arrives before
  camera/encoder finish standing up (observed in the very first connection
  test, before the fix that added retries wasn't needed -- this is
  *correct* behavior, not a bug, since the command genuinely couldn't be
  applied yet).

**Two real bugs found via this testing, both fixed:**

1. **Console output interleaving.** `ConsoleControlUi::OnMessage` (runs on
   `ControlChannel`'s receive thread) and `RunCommandLoop` (the stdin
   thread) both called unsynchronized `std::printf`, and their multi-line
   output could interleave mid-block under load -- observed as capability
   dumps with lines in the wrong order. Fixed with a `std::mutex` held for
   the whole of each function's printing. Purely cosmetic (the wire bytes
   were always correct), but worth fixing since it was actively
   undermining the ability to verify anything from the console output.
2. **`SetLens` could crash the whole app.** `CameraManager.openCamera()`
   threw `CameraAccessException` ("Camera "0" disabled by policy (code
   6)") *synchronously* from `CameraCapture.openCameraById`, called from
   `ControlChannel`'s receive thread via `switchLens`. The existing
   `catch (e: SecurityException)` didn't cover it -- Kotlin doesn't
   enforce Java's checked exceptions, so this compiled fine and then
   crashed the whole process (`FATAL EXCEPTION: ControlChannel-recv`,
   confirmed via `adb logcat -b crash`) the first time a real device
   returned this particular error. Fixed two ways: `openCameraById` now
   also catches `CameraAccessException`, and -- as a defense-in-depth
   backstop against *any* future command-handler exception, not just this
   one -- `ControlChannel.dispatch` now wraps every command dispatch in a
   `try/catch` that replies `Ack{Error}` instead of letting anything
   propagate and take the receive thread (and therefore the app) down.
   Re-tested with 8 rapid lens switches afterward: zero crashes, same
   process ID throughout.

**Tooling note, not a product issue:** verifying this phase required
scripting `phonecam-host.exe`'s console over a redirected stdin/stdout pipe
from PowerShell (no physical device interaction needed, matching this
project's established approach for this `adb input`-blocked phone). The
straightforward approach (.NET `Process` + `Register-ObjectEvent` for async
`OutputDataReceived`) turned out to be an unreliable way to *capture*
output in this environment -- lines were frequently missing or arrived out
of real-time order relative to when they were actually written (confirmed
by comparing against `stderr`, which reliably showed the pipeline running
correctly underneath), especially for anything printed in the last few
seconds before the process was killed. This is why `RequestKeyframe`/
`SetBitrate`/`TapToFocus` weren't independently live-verified with a clean
transcript in this session -- not because the pipeline is suspect, but
because the capture harness kept losing the very last lines of each run.
A future session automating this further should redirect straight to a
file at the OS level (`Start-Process -RedirectStandardOutput`, proven
reliable earlier in this same session) rather than reading via .NET's
async line events.

**Resolved decisions carried in from Phase 3C** (see that section): (1)
`MainScreenViewModel.startCapture()`'s `lensFacing` default is back to
`LENS_FACING_BACK` -- the temporary front-camera override existed to
sanity-check real content before a lens-switch control existed; now that
`SetLens` is implemented and verified, the override is no longer needed.
(2) The auto-start/auto-retry `LaunchedEffect` in `MainScreen.kt` is kept,
not reverted -- it was genuinely useful again this phase (this session's
entire verification flow depended on it, since there's still no way to tap
the phone's screen), and it previews Phase 5's planned foreground-service
auto-start rather than fighting it.

**Also new, small, and easy to miss:** `CameraCapture.tapToFocus` initially
set `CONTROL_AF_TRIGGER_START` directly on the persistent
`requestBuilder` shared by every live control -- since that builder is
reused for every future `setRepeatingRequest` call (zoom, EV, ...), the
trigger would have stayed baked in and re-fired on every subsequent
unrelated command. Caught in code review before it could cause a live bug;
fixed by issuing the trigger as a one-shot `session.capture()` call and
resetting the builder's trigger field to `IDLE` immediately after, which is
the standard Camera2 tap-to-focus pattern.

**Deferred to a later phase, by choice:** `Stats` telemetry (the method
exists on `ControlChannel.kt` but nothing calls it periodically -- no
~1/sec push yet); the control channel is not durable across a video
`Stop`/`Start` cycle (it shares `CaptureController`'s lifecycle rather than
outliving it, unlike the full protocol doc's design) -- both are Phase 5
robustness scope, not Phase 4 protocol-correctness scope. **Next: Phase 5**
(robustness, installer, tray app) or revisiting manual exposure controls
now that the device is known to support them.
