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
API, requires API 28) streams straight into `encode/H264Encoder.kt`'s
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
robustness scope, not Phase 4 protocol-correctness scope.

## Status: Phase 5 in progress (host reconnect robustness), 2026-08-18

Scoped down from the full Phase 5 roadmap item (robustness + installer +
tray app) to the one piece with a real, testable exit condition this
session: **`phonecam-host.exe` surviving USB unplug/replug and phone-side
backgrounding without a restart.** The installer and tray app are
deliberately deferred -- an installer's own exit criterion ("clean install
on a fresh Windows account") isn't verifiable without one, and a tray app
reopens the GUI-framework decision that was correctly kept out of Phase 4's
console UI.

**What changed:** `LiveVideoBridge::Start()` no longer requires the phone
connection to succeed synchronously -- it now only fails on truly
structural setup problems (`SharedFrameRing::Open()`, decoder
`Initialize()`); the phone connection itself is owned by a new supervising
loop in `LiveVideoBridge::Run()` that connects-or-retries indefinitely, so
the virtual camera starts and exists (serving stale/black frames) even
before the phone is ready, instead of the whole process exiting if it
raced the phone on startup. The control channel got the same treatment,
as a supervising loop added directly in `main.cpp` (mirroring
`LiveVideoBridge::Run()`) rather than changing `ControlChannel`'s own
API -- and every (re)connect now sends `RequestKeyframe`, since a fresh
phone-side encoder session has no guaranteed keyframe yet, matching
`docs/control-protocol.md`'s stated reconnect behavior.

**A real concurrency bug found via live testing, not by inspection:** the
reconnect loop's first version had no backoff after a connect that
succeeded but then immediately dropped -- confirmed live that `adb
forward`'s local TCP proxy accepts the PC-side `connect()` right away and
only discovers moments later that nothing is listening on the phone's
abstract socket, so "connected" immediately followed by "receive loop
ended" is a common real case, not just Connect() failing outright. Without
a backoff on that path too, this busy-looped: `RunAdbForward` spawns a
whole new `adb.exe` process per attempt, and the log showed multiple
connect/disconnect cycles per second before this was caught and fixed
(backoff now applies uniformly after any disconnect, not only an outright
Connect() failure).

**A second bug, caught before it shipped rather than live:** the reconnect
loop makes `AdbVideoTransport`/`ControlTransport::Connect()` and
`Disconnect()` reachable concurrently from two different threads for the
first time -- previously `Connect()` only ever ran once, synchronously,
before any worker thread existed. The original per-call `WSAStartup()`/
`WSACleanup()` pairing was unsafe under that: a `Disconnect()` arriving
mid-`Connect()` could `WSACleanup()` out from under a connect attempt still
using Winsock, or a `Connect()` finishing just after `Disconnect()` could
publish a live socket nobody would ever tear down, hanging `Stop()`'s
`join()`. Fixed by moving `WSAStartup`/`WSACleanup` to the transport's
constructor/destructor (called once, no cross-thread pairing needed) and
adding a mutex-guarded socket handle plus a `cancelled` flag that
`Connect()` checks at each blocking step, so a concurrent `Disconnect()`
always wins and no socket is ever published or leaked after shutdown was
requested.

**Verified live** (both scenarios, by the user, not simulated): (1)
started `phonecam-host.exe` with the phone app not yet streaming --
confirmed retrying quietly at the fixed backoff interval, then connecting
automatically the moment the phone app was opened, with capabilities/
lens/settings all re-synced, no host restart. (2) unplugged the USB cable
mid-stream -- phone showed "Waiting for PC connection..." -- host logged
`adb forward exited with code 1` and retried at the same backoff interval
(no busy-loop) -- replugging reconnected automatically with no host
restart.

**Also fixed, cheaply, while in the area:** the vcam DLL
deployment-path gotcha noted in Phase 3C -- `PhoneCamVCam.vcxproj` now has
a `PostBuildEvent` that copies the built DLL over the registered
`C:\ProgramData\PhoneCam\phonecam-vcam.dll` copy automatically (best-effort;
silently no-ops if the file is locked by a running host/Frame Server, same
as before). `docs/build.md` updated to match -- the manual "copy the
rebuilt DLL" step is gone; only the "restart FrameServer services if
locked" step remains, and only when needed.

**Still open for Phase 5:** the installer; the tray app. Also still open,
unclear if it's a real gap: if the phone app is ever sitting in its own
Idle screen (nobody bound to the video socket) when the host tries to
reconnect, the host will retry forever with no way to ask the phone to
start capture again -- there's no PC->phone "wake" command yet. Not hit in
this session's testing (the Error-path auto-retry from Phase 4's stability
fix kept the phone side coming back on its own), but worth deciding on
purpose rather than discovering it live.

## Status: Phase 5 continued (Android foreground service), 2026-08-18

Added the second piece of host-independent reconnect robustness: a camera-
type foreground service (`CaptureService`, `android:foregroundServiceType=
"camera"`) that now owns `CaptureController` instead of
`MainScreenViewModel` owning it directly. This is what makes
backgrounding/screen-off survival possible at all -- a plain
Activity-owned controller dies with the Activity, and Android is free to
freeze or kill a backgrounded process (Doze/App Standby) with no foreground
service holding it open. `MainScreenViewModel` now just sends
`ACTION_START`/`ACTION_STOP` intents to the service and polls its bound
state (`state`, `elapsedSeconds`, `measuredFps`, `lastError`) the same way
it used to poll `CaptureController` directly -- `CaptureController` itself
is unchanged. The service acquires a `PARTIAL_WAKE_LOCK` (capped at 12h,
not indefinite) alongside `startForeground()` so the CPU keeps running with
the screen off, and posts a low-importance ongoing notification (tapping it
reopens `MainActivity`); `POST_NOTIFICATIONS` is requested proactively on
API 33+ so that notification is actually visible, though the service runs
regardless of whether it's granted.

**A real bug found via live testing, not simulated -- the user tapped
Stop/Cancel themselves and reported "socket closed":**
`CaptureController.stop()` closes the video socket before it stops the
encoder (documented in its own comments, from Phase 3B), so the encoder's
drain thread hitting a write failure is an expected, routine part of any
stop -- not just a mid-stream error. The old `MainScreenViewModel` was
accidentally safe from this: `stopCapture()` called `controller.stop()`
*synchronously* and then unconditionally overwrote the UI state to `Idle`
afterward, so a same-thread write-order guarantee always made Idle win
over any reentrant error write. Routing stop through `CaptureService`
broke that guarantee -- `ACTION_STOP` is handled asynchronously (the next
message on the service's main-thread queue), so the ViewModel's `Idle`
write now happens *before* the actual `controller.stop()` teardown (and
its potential reentrant `onError` call) has even run, and a subsequent
poll (or a fresh poll from an immediate restart) could pick up the stale
`lastError` and surface a spurious "Error: Socket closed" -- which then
fed MainScreen's existing Error-path auto-retry, visible in logcat as
repeated unexplained camera/encoder re-setup cycles after a single Cancel
tap. Fixed with a `stopRequested` flag in `CaptureService` (mirroring
`CaptureController`'s own stop-idempotency guard, but one layer up):
`onCaptureError` now ignores any error that arrives while a stop was
already requested, so expected teardown noise is distinguished from a
genuine mid-stream failure, which still surfaces normally.

**Verified live by the user, not simulated, after that fix:** Stop then
Start capture again -- clean, no spurious error. Screen off for ~20-30s
mid-stream, then back on -- still connected. Home button (backgrounding
the app) for a similar interval, then back -- still connected. Both are
exactly Phase 5's original roadmap exit criterion ("survives unplug/replug
and screen-off").

**Still open for Phase 5 (until now):** the installer; the tray app. The
known host-side reconnect gap (no PC->phone "wake" command if the phone is
sitting idle) is now slightly more relevant, since a backgrounded/
screen-off phone that later gets its capture stopped by something outside
this app's control would hit exactly that gap -- still not decided on
purpose.

## Status: Phase 5 complete (tray icon + installer), 2026-08-18

Finished the deferred half of Phase 5.

**Tray icon (`windows/host/tray/TrayIcon.*`):** plain Win32
`Shell_NotifyIcon` + a hidden `HWND_MESSAGE` window -- no GUI framework,
matching the project's fork-working-samples stance. Deliberately additive,
not a replacement: `ConsoleControlUi`'s stdin command loop is untouched,
still running on its own thread exactly as Phase 4 left it; the tray just
replaces the old `WaitForSingleObject(stopEvent, INFINITE)` wait in
`main()`'s shutdown path with `TrayIcon::RunMessageLoop()`. Ctrl+C and the
tray's own "Exit" menu item both funnel into the same `DestroyWindow`->
`WM_DESTROY`->`PostQuitMessage` teardown, not two separate paths -- see
`ConsoleHandler` in `main.cpp`. A plain event-wait fallback is kept for the
(unexpected but not impossible) case `Shell_NotifyIcon` itself fails to
create. Status text ("streaming (video + control)" / "waiting for
phone...") is a cheap read of `LiveVideoBridge::IsConnected()` (new getter)
and a control-channel-connected atomic already maintained by the existing
reconnect supervise loops -- no new polling thread. Verified live: the
user captured a screenshot of the tray flyout showing the icon with a
live, correct "PhoneCam - waiting for phone..." tooltip.

**Installer (`windows/installer/PhoneCam.iss`, built with Inno Setup --
newly installed via winget as a Phase 0-style prerequisite, now in
`docs/build.md`):** wires together pieces that already had working
implementations rather than reimplementing any of them -- `phonecam-svc.exe
--install`/`--uninstall` (existing, commit `af36ed3`), `regsvr32`
register/unregister of the vcam DLL, and file placement. Two things had to
be fixed first for this to be viable at all:

1. **Debug-CRT distribution.** The existing build was Debug-only, which
   dynamically links a debug CRT (`ucrtbased.dll`, etc.) that end-user
   machines don't have and Microsoft doesn't intend for distribution.
   Fixed by adding a Release config with a *static* CRT
   (`CMAKE_MSVC_RUNTIME_LIBRARY` = `MultiThreaded` for `phonecam-host`/
   `phonecam-svc`, matching `PhoneCamVCam.vcxproj`'s Release config which
   was already static) -- confirmed via `dumpbin /dependents` on all three
   Release binaries that none of them reference `MSVCP140`/`VCRUNTIME140`/
   `UCRTBASE`, only OS-shipped DLLs. No VC++ redistributable dependency at
   all, by construction.
2. **Bundling adb** (the most likely fresh-machine failure mode, per
   review -- `RunAdbForward` in both transports invokes bare `adb` via
   `CreateProcessW`, which fails silently-ish, indistinguishable from an
   unplugged phone, if adb isn't on PATH). Fixed by vendoring `adb.exe` +
   its two required DLLs into `third_party/platform-tools/` (plain
   committed files, not a submodule -- this was always the plan's intent,
   see the repo-structure section above) and installing them next to
   `phonecam-host.exe`. **No source change was needed for this to work**:
   `CreateProcessW`'s documented search order already checks the calling
   process's own directory *before* PATH when `lpApplicationName` is
   `nullptr` (which is how `RunAdbForward` calls it) -- confirmed live,
   the installed `phonecam-host.exe` connected to the phone using its
   bundled `adb.exe` with no PATH changes.

Also caught while wiring the installer: the vcxproj `PostBuildEvent` added
earlier this session (the vcam-DLL-deployment fix) used `--` inside an XML
comment, which is invalid XML and made `PhoneCamVCam.vcxproj` fail to load
in MSBuild entirely -- this had been silently broken since that commit
because nobody had rebuilt vcam via MSBuild since. Fixed (single hyphen)
and confirmed the vcxproj loads and builds again.

**What the installer does, matching `PhoneCam.iss`'s own header comment:**
installs `phonecam-host.exe`/`phonecam-svc.exe`/bundled adb to
`{app}` (Program Files), the vcam DLL to `C:\ProgramData\PhoneCam`
(deliberately the *same* path the dev-build `PostBuildEvent` already
refreshes -- two different "the registered copy" locations is exactly the
two-copies bug this project already hit once), runs `phonecam-svc.exe
--install` and `regsvr32` on install, and the inverse plus a
`C:\ProgramData\PhoneCam` cleanup on uninstall.

**Verified live on this machine (the user ran the installer/uninstaller;
I verified via `sc query`, `reg query`, and ffmpeg dshow):** install ->
service `PhoneCamRingService` running, CLSID registered pointing at the
ProgramData copy, installed `phonecam-host.exe` connects to the phone
using its bundled adb and streams -> `ffmpeg -f dshow -i "video=PhoneCam
(Windows Virtual Camera)"` captures real live content (confirmed with the
phone's camera deliberately covered then uncovered, to rule out a stale
frame). Uninstall -> service and CLSID registration both cleanly removed,
`Program Files\PhoneCam` removed; `ProgramData\PhoneCam`'s DLL briefly
resisted deletion (Frame Server hadn't released it yet -- the same known
lock behavior documented in `docs/build.md`, not a new bug) but deleted
fine moments later on its own, and everything else in that directory
(including unrelated leftover files from much earlier Phase 1 testing)
was cleaned up correctly by `[UninstallDelete]`.

**Not verified, and not claimed to be:** "clean install on a fresh Windows
account" -- Phase 5's actual roadmap exit criterion -- since this machine
isn't one. Everything above is the strongest evidence obtainable without
one. Also not de-risked: what happens if `phonecam-host.exe` runs on a
machine where the phone's USB driver situation differs (this project has
only ever tested against a machine where `adb devices` already worked).

**Phase 5 is now fully complete**: host-side reconnect robustness,
Android foreground service, tray icon, and installer are all landed and
verified live to the extent this machine allows. The known host-side
"phone sitting idle, no PC->phone wake command" gap remains open, along
with the pre-existing carryover items (sensor-orientation correction,
`Stats` telemetry, manual exposure controls) -- none of these were Phase 5
scope.

## Status: Phase 6 (quality/perf pass), 2026-08-18

Scoped down deliberately, per review before starting: D3D11 GPU
zero-copy decode (the other originally-planned Phase 6 item) is **not**
attempted this pass -- it's a much larger undertaking (rewriting
`MFH264Decoder` to output DXGI surfaces instead of CPU NV12 buffers,
extending `SharedFrameRing` to carry a shared texture handle) that doesn't
fit alongside the rest of this session's scope, and is flagged explicitly
rather than silently dropped. Three items landed and verified live instead:

**Fixed stream size, bumped to 1080p (`windows/vcam/MediaStream.cpp`,
`MainScreenViewModel.kt`):** the vcam's declared `MF_MT_FRAME_SIZE` was a
compile-time `#define` (1280x720, matching Phase 3C's default) --
consumers negotiate against this declared size, so any mismatch with what
the ring actually carries silently stretches the image (the exact bug
Phase 3C already hit once). Bumped `NUM_IMAGE_COLS`/`NUM_IMAGE_ROWS` to
1920x1080 and the Android default capture resolution/bitrate to match
(1920x1080, 6 Mbps, up from 4 Mbps at 720p) so both sides stay in sync.
True dynamic resolution (a PC-driven `SetResolution` mid-session) would
need MF stream-descriptor renegotiation -- a materially larger change,
still deliberately out of scope; this fix only makes the *declared*
resolution correct and stable, not runtime-changeable.

**NV12/RGB32 color matrix, BT.601 -> BT.709 for HD content
(`windows/vcam/Tools.cpp`):** the CPU-path color conversion (used
whenever a consumer doesn't call `SetD3DManager` -- the common case
observed live) was hardcoded to BT.601 coefficients regardless of
content, which is wrong for this project's content (always >=720p, never
SD) -- the documented "washed out" color bug. Both `RGB32ToNV12` and its
inverse `NV12ToRGB32` now select BT.709 coefficients for height>=720,
falling back to the original BT.601 set below that (kept for
correctness/generality, though never exercised in practice here since
this project never streams SD). The BT.709 integer coefficients were
independently derived and cross-checked against the already-correct
BT.601 code's derivation method (each coefficient set's Y-row terms
equal `(219/255)*Kr,Kg,Kb*256` for that standard's primaries), not
copied from an unverified source.

**Stats telemetry + adaptive bitrate
(`ControlChannel.kt`/`CaptureController.kt`):** `Stats` was defined in
`proto/control.fbs` since Phase 4 but nothing ever sent it. Added
`ControlChannel.sendStats()` and a new per-session `StatsLoop` thread in
`CaptureController` (started right when `STREAMING` begins, same
cancellation-token pattern as the accept/control threads) that ticks
every ~1s: computes `measuredFps`, estimates dropped frames (expected
frames at the target fps minus frames actually encoded that interval --
MediaCodec's surface-input path exposes no real drop counter), reads
`PowerManager.currentThermalStatus`, and applies a simple adaptive-
bitrate policy (halve under `Severe` thermal, back off 25% under
`Moderate`, ramp up 20%/tick toward the target ceiling once thermal
clears, floored at 1 Mbps). A PC-issued `SetBitrate` re-anchors the
ceiling the adaptive loop ramps toward, so an explicit user choice always
wins over the next adaptive tick rather than being silently overwritten.

**Verified live:** capabilities/current-settings/lens exchange all still
correct; a captured DirectShow frame confirmed real, correctly
proportioned 1920x1080 content with natural-looking colors (checked
directly, not just inferred from the fix); `ffmpeg -f dshow` and the
user's own OBS session both showed a working feed at 1080p. Stats
telemetry confirmed end-to-end via the host console's `Stats` line,
showing genuinely stable performance: **fps ramped from 17 to a steady
29.4, with dropped frames dropping to 0** after the first second of
startup ramp-up -- this is the actual, measured "push stable 1080p30"
exit criterion, not just "it doesn't crash at 1080p." The adaptive-
bitrate *reduction* path is implemented but not exercised under real
thermal pressure in this session (thermal stayed at `Nominal` throughout
the test window) -- noted honestly rather than claimed as proven.

## Status: Phase 7 in progress (AOA transport), 2026-08-18/19

Goal: replace the adb-forward transport with a direct USB accessory
connection (Android's AOA framework + `libusb` on the host), so the
product no longer requires Developer Options -> USB debugging. Scoped
down to a single checkpoint before touching the real transport: prove a
host-readable bulk pipe exists at all, via a throwaway diagnostic tool
(`windows/tools/aoa_probe/`, explicitly not shipping product) and a
minimal Android side (`transport/AoaAccessoryTransport.kt`, a 500ms
heartbeat write over `openAccessory()`'s pipe -- a heartbeat rather than
a one-shot write specifically so host-side read failures can't be
timing-race artifacts, see the class's own doc comment).

**Android side: fully proven, not in question.** `accessory_filter.xml` +
`MainActivity`'s `USB_ACCESSORY_ATTACHED` intent-filter auto-launch the
app the moment the host sends `ACCESSORY_START`; `openAccessory()`
succeeds and the heartbeat runs continuously. Confirmed live, repeatedly,
across many replug/restart cycles.

**Windows side: the read never succeeded, and the reason turned out to be
driver binding, not code.** The investigation ruled out several
hypotheses in order, each with real evidence, before landing on the
actual cause:

1. **Not a timing/race issue.** Converting the one-shot checkpoint write
   to a repeating heartbeat didn't help -- reads failed identically for
   60+ continuous seconds while logcat confirmed the heartbeat was
   running the whole time.
2. **Not a wrong-interface-number bug in `aoa_probe` itself.** Iterating
   every interface's class/subclass/protocol triple (rather than
   assuming interface 0) confirmed interface 0 genuinely is the AOA
   accessory interface (`0xFF/0xFF/0x00`) on this device -- this
   hypothesis was ruled out, not the bug.
3. **A real, separate `libusb`-on-Windows quirk exists but turned out not
   to be the root cause here:** `set_composite_interface` (libusb's
   Windows backend) logs a warning when it can't parse an interface
   number out of a composite child device's hardware-ID suffix -- and
   both of this phone's accessory-mode sub-interfaces use non-standard
   suffixes (`&ADB` and `&MS_COMP_MTP&Redmi_Note_8` instead of the
   expected `&MI_XX` pattern), triggering that warning for both. Real,
   but a red herring for the actual failure -- see below.
4. **A real, but ultimately irrelevant, sub-interface driver mismatch.**
   `pnputil /enum-devices` during a live failure showed *neither*
   accessory-mode sub-interface bound to WinUSB: the accessory interface
   (`...&MS_COMP_MTP&Redmi_Note_8`) was bound to `wpdmtp.inf` (Windows'
   MTP/Portable-Devices driver stack), and the ADB interface
   (`...&ADB`) was bound to `oem51.inf` (`AndroidUsbDeviceClass`,
   Windows' inbox generic Android driver). This looked like the
   smoking gun (and matches a public report,
   [libwdi#332](https://github.com/pbatard/libwdi/issues/332), showing
   the identical `MS_COMP_MTP`/`WUDFWpdMtp` pattern on a different
   Android phone) -- but turned out not to be what actually gated the
   read. See the real cause below.
5. **The actual cause: the *parent* composite device, not either
   sub-interface, is what needed a libusb-usable driver.** A `Redmi
   Note 8` entry at the top level (`USB\VID_18D1&PID_2D01\<serial>`,
   above both sub-interfaces) had a libusbK binding from an earlier
   session's Zadig action, confirmed live in Zadig's device list
   (`Driver: libusbK (v3.1.0.0)`, USB ID `18D1:2D01`). Once that
   existed, a fresh `aoa_probe --handshake` run succeeded immediately --
   `libusb`'s Windows backend opened and read through *that* binding,
   with the sub-interfaces' own (mismatched) driver assignments never
   entering into it. In hindsight this also explains item 3 above: the
   `set_composite_interface` interface-numbering confusion was real but
   moot, since the actual open/read path never went through per-interface
   WinUSB handles at all in the working case.

**Resolved, not just diagnosed:** confirmed live -- `aoa_probe
--handshake` received the phone's heartbeat cleanly
(`Received 21 bytes: PHONECAM-AOA-PIPE-OK.`) with no Zadig changes in
that session; the existing libusbK-on-parent-device binding was
sufficient. **Do not "fix" the sub-interface driver mismatch (item 4)
going forward** -- it looks wrong in Zadig/`pnputil` but is not on the
path that matters, and changing it risks disturbing the binding that
actually works.

**A secondary, real finding: USB "No data transfer" mode does not
reliably support the AOA mode switch on this phone.** Tested at the
user's request, to see whether AOA could avoid MTP entirely. Findings,
via `pnputil` (PID changes to `0x4EE7`, non-composite, no `&MI_XX`
sub-interfaces at all) and `adb logcat`:

- The device *did* switch into accessory mode (`PID_2D01`) once while in
  this mode, but only after several seconds -- well past `aoa_probe`'s
  original 3s wait, which is why the first attempt looked like a
  no-op and had to be re-diagnosed.
- Every later `ACCESSORY_START` sent while in this mode was acknowledged
  by `libusb` at the USB-transfer level, but `adb logcat`'s
  `UsbDeviceManager` never logged the corresponding `Setting USB config
  to accessory,adb` line that a real switch produces (contrast: File
  Transfer mode reliably logs exactly that line). The USB stack still
  visibly cycles (disconnect/reconnect `uevent`s) but always settles
  back on plain `adb`.
- The phone crashed once during this testing (required a manual restart)
  -- correlated with, but not proven caused by, an `ACCESSORY_START`
  sent in this mode. Not investigated further; noted as a real, if
  unconfirmed, data point against relying on this mode.
- **Conclusion: File Transfer mode is the reliable trigger for the AOA
  switch on this device** and is what the checkpoint testing uses. Worth
  retesting "No data transfer" on other devices/Android versions before
  assuming this generalizes, but not worth more investigation on this
  one now that a working path exists.

**Alternative considered and rejected: USB tethering (RNDIS/NCM) instead of
libusb/WinUSB.** Driver-free (Windows binds RNDIS with its own inbox driver)
and not gated behind Developer Options, but tethering is carrier/SIM-gated on
many real devices, and an existing project solving nearly this exact
phone-PC-over-USB problem ([Genymobile/gnirehtet](https://github.com/Genymobile/gnirehtet))
deliberately routes over adb instead for that reason. Would also mean
building a second full transport, not a small substitution. Not pursued.

**Product-shipping note, decided but not yet implemented:** end users
must never be asked to run Zadig themselves. The real fix belongs in the
Windows installer -- silently installing a `libwdi`-driven driver binding
during setup, the same one-UAC-prompt shape the installer already has
for `phonecam-svc.exe --install`/vcam `regsvr32`. **Correction (see item
5 above): the binding that actually matters is libusbK on the *parent*
composite device (`USB\VID_18D1&PID_2D01\<serial>`), not WinUSB on
either sub-interface** -- an earlier version of this note said WinUSB,
which was written before the real root cause (item 5) was found and
would have had the installer target the wrong device node. The installer
must call `libwdi` against the parent device's hardware ID, not either
child interface. Deferred to its own follow-up phase, out of scope for
finishing the real `AoaTransport` -- it also needs code signing and an
install-time verification story (does it work on a machine that has
never seen this phone?) that hasn't been designed yet.

## Status: Phase 7 driver-auto-install investigation, stopped at a real blocker, 2026-08-19

Attempted the "does it work on a machine that's never seen this phone"
verification called for above, using a second, unrelated device -- a
Redmi Note 7 (Android 9/10, no root, USB debugging never turned on,
never connected to this PC before). Before testing, rolled back this
PC's own earlier Zadig actions (see "Resolved, not just diagnosed" above)
as best as non-elevated access allowed: `pnputil /delete-driver
oem129.inf|oem107.inf /uninstall` succeeded at un-assigning both from
their device instances, but the actual driver-store packages couldn't be
purged (`Access is denied` without elevation). Also found, while checking
this: Windows' own driver ranking for the Note 8's accessory-mode PID
already prefers a pre-existing **Samsung** driver (`ssudbus.inf`) over
either of ours, leftover from unrelated history on this machine -- this
PC was never a clean reference machine to begin with, which is exactly
why testing against a second, never-touched device mattered more than
trying to scrub this one.

**Confirmed the core hypothesis first:** `aoa_probe --handshake` against
the clean Note 7 failed exactly as predicted --
`libusb_open()` succeeds (opening the raw USB device needs no driver),
but `winusbx_claim_interface` fails to auto-claim *any* interface
(`[1] Incorrect function`, tried across all 32 interface numbers), ending
in `GetProtocol failed: LIBUSB_ERROR_NOT_FOUND`. This is the real,
reproducible "what a normal user hits" case: no Zadig history at all,
AOA cannot start.

**Researched alternatives before committing to the libwdi-installer
fix** (web research + advisor, this session):
- **WCID/MS OS Descriptors** (the theoretically clean fix -- a device
  that advertises WinUSB-compatibility via its own descriptors gets
  Windows' inbox `winusb.sys` auto-bound, zero custom driver needed at
  all) would have to be implemented in Android's own USB accessory
  gadget driver (kernel-level, `f_accessory.c`). Not reachable from a
  non-rooted stock phone -- confirmed no evidence AOSP's accessory
  gadget implements this, and the exact issue this project already found
  ([libwdi#332](https://github.com/pbatard/libwdi/issues/332)) still
  describes Zadig as required. Dead end, not our decision to make.
- **USB tethering (RNDIS/NCM)** as a fully driver-free alternative
  transport: real (Windows binds its own inbox RNDIS driver, and
  tethering isn't gated behind Developer Options), but carrier/SIM-gated
  on many real devices, and
  [Genymobile/gnirehtet](https://github.com/Genymobile/gnirehtet) -- an
  existing project solving nearly this exact phone-PC-over-USB problem --
  deliberately routes over adb instead of raw tethering for that reason.
  Rejected: would also mean building a second full transport, not a
  small substitution.

**Then hit a real, unresolved blocker trying the libwdi/Zadig fix
itself on the clean Note 7.** Unlike the Note 8 (which exposes several
separate USB interfaces -- MTP, ADB, etc. -- when debugging is on), this
Note 7 in File Transfer mode with debugging **off** enumerates as a
single, non-composite MTP interface: `pnputil` shows only one device
node (`USB\VID_2717&PID_FF40`, driver `wpdmtp.inf`), no child
interfaces to attach a second driver to alongside the existing one.

Zadig detected this device already implements **WCID** itself, declaring
`MS_COMP_MTP` compatibility (this is *how* Windows already auto-binds
its MTP driver with no driver disk needed -- consistent with the WCID
research above, just for MTP, not for the AOA accessory interface).
Zadig's "Install WCID Driver" button (installing against that declared
compatible ID rather than the hardware ID) reported success, but had no
real effect: after a replug, `pnputil /enum-devices ... /drivers` showed
only one matching driver at all -- `wpdmtp.inf` via `USB\MS_COMP_MTP`,
rank `00FF2000` -- the libusbK package never became a candidate. This
makes sense in hindsight: an install keyed to `MS_COMP_MTP` would claim
*every* MTP device on the machine, not just this phone, which is exactly
the kind of over-broad rule this project already reasoned was unsafe
elsewhere (see the parallel finding about `oem107.inf`'s ADB-class-triple
match, earlier in this document) -- Windows plausibly declined to
register something that broad, or registered it somewhere that doesn't
match this specific device instance.

The advisor's suggested next check -- retry with Zadig's plain
**"Install Driver"** (hardware-ID-targeted, the same shape as `oem129.inf`
on the Note 8, which demonstrably did register) instead of "Install WCID
Driver" -- was not completed; the user stopped the investigation here.

**Important: this is an untried next step, not a confirmed dead end.**
Only one specific install mode was tested and ruled out -- Zadig's
**"Install WCID Driver"** button, which targets the device's *declared
compatible ID* (`MS_COMP_MTP` here) rather than its hardware ID. That's a
meaningfully different operation from plain **"Install Driver"**
(hardware-ID-targeted: `USB\VID_2717&PID_FF40`), which is the exact
method that *did* successfully register a driver on the Note 8's
accessory-mode node (`oem129.inf`, confirmed via `pnputil` earlier in
this document). **The hardware-ID-targeted install was never attempted
on the Note 7** -- the investigation stopped one step short of the test
that was actually most likely to work.

**The concrete next step, cheap to run:** in Zadig, check the "Edit" box
next to the device name, use the dropdown arrow on the install button to
choose plain "Install Driver" (not the WCID variant), select libusbK or
WinUSB, install, replug, then check
`pnputil /enum-devices /instanceid "USB\VID_2717&PID_FF40\<serial>" /drivers`
-- if a libusbK/WinUSB entry now appears in the "Matching Drivers" list
alongside `wpdmtp.inf`, retry `aoa_probe --handshake`. If that succeeds,
the driver-binding mechanism itself is confirmed to work the same way on
this device as it did on the Note 8, and the remaining design question
becomes purely "how does an installer trigger this automatically" (a
maintained per-OEM VID/PID list, or an on-attach elevated install) --
not "does this even work at all."

**Not pursued further that session, for lack of time -- parked as an open
problem with a known next step**, not a dead end.

## Status: Phase 2 driver auto-install, built and verified live end-to-end, no Zadig

Picked back up and solved. Built `phonecam-usbdriver.exe`
(`windows/usbdriver/`), a separate elevated helper on top of a
newly-vendored **libwdi** (`third_party/libwdi/`), replacing the manual
Zadig step entirely -- confirmed live on the Redmi Note 8, USB debugging
off, with **zero Zadig involvement and exactly one Windows admin prompt**.

**Vendoring libwdi turned out to be its own real sub-problem.** libwdi's
own repo doesn't include the actual driver binaries its `embedder` build
step needs to bake in (`third_party/libwdi/NOTICE.txt` has the full story)
-- the WDK redistributable path was skipped entirely (WinUSB isn't used
here), and the libusbK binaries were sourced from this machine's own
Windows Driver Store (`C:\Windows\System32\DriverStore\FileRepository\
libusbk_generic_device.inf_amd64_*\`), left there by this project's own
earlier Zadig testing -- not downloaded fresh, not built from scratch.
Vendored as `third_party/libusbk-redist/`. Building libwdi itself needed
several real fixes to its upstream MSVC project (documented in
`third_party/libwdi/NOTICE.txt`): dropping the ARM64 helper (no toolset
installed, and upstream's project reference was missing
`ReferenceOutputAssembly=false`, so its build failure blocked the whole
static-lib target even though nothing needs its output), a broken
pre-build step (`embedder embedded.h` invoked as a bare command name
instead of a path, failing with exit 9009), and keeping `WDF_VER` defined
(needed at runtime for the generated INF's KMDF-version fields) even
though the WdfCoInstaller redistributable itself isn't embedded -- KMDF
1.11 has been inbox on Windows 10 1607+, and this project already
requires Windows 11-era Media Foundation APIs elsewhere.

**The helper (`windows/usbdriver/main.cpp`) does two things in one
elevated pass, matching the two-device-timing problem this document
already identified:**

1. **Installs for the phone's currently-present, normal-mode device**
   (`wdi_create_list` to find it by VID/PID, refuse if `is_composite`
   [the guard against ever touching a debugging-on/composite device --
   see below], `wdi_prepare_driver` + `wdi_install_driver`, libusbK).
2. **Pre-stages all six standardized AOA accessory-mode PIDs**
   (`0x2D00`-`0x2D05`, always under Google's VID `0x18D1` regardless of
   the phone's own normal-mode VID) via `wdi_prepare_driver` (one call per
   PID -- hand-editing one INF's device list would invalidate its
   per-package catalog signature) + `SetupCopyOEMInfW` to stage into the
   driver store without a device present. Confirmed this second half is
   not optional: the first live test (install-only, no pre-staging) sent
   a real, successful AOA handshake -- `AOA protocol version: 2`, the
   phone genuinely switched into accessory mode (`PID_2D00`) -- but
   Windows then showed the re-enumerated device in `CM_PROB_FAILED_INSTALL`
   (Problem Code 28), since no driver existed for a hardware ID that
   didn't exist a moment earlier. After adding the pre-stage step, the
   same test showed `Status: Started`, `Driver Name: oem135.inf`, no
   problem state, on the very first re-enumeration.

**Composite-device guard, the one thing this must never get wrong:** a
device with active ADB/MTP sub-interfaces (`is_composite == TRUE`, i.e.
USB debugging is on) is refused outright, exit code 11. Installing
libusbK against a composite parent replaces the whole `usbccgp.sys`
composite driver stack, which is exactly the "Zadig broke my adb" failure
class this project specifically set out to avoid causing for users.
Verified live: this Note 8 with debugging on enumerates with a real ADB
sub-interface node (`is_composite` would be true); with debugging off
(the only state this helper is meant to run in) it's a single
non-composite MTP device, matching the same topology already documented
for the Redmi Note 7 above.

**Verified live, full chain, one command, one UAC prompt:** Note 8,
debugging off, File Transfer mode -> `phonecam-usbdriver.exe --install
--vid 18D1 --pid <normal-mode PID>` (elevated) -> `pnputil` confirms
`Class Name: libusbk devices`, `Driver Name: oem129.inf` on the
normal-mode device -> `aoa_probe --handshake` succeeds, phone
re-enumerates as `18D1:2D00` with `Status: Started` (not a Problem state)
-> phone auto-launches PhoneCam via `accessory_filter.xml`'s intent match
and shows **"ON AIR"** with zero manual taps -> `phonecam-host.exe --aoa`
logs `AoaVideoTransport: connected` and `MFH264Decoder: output negotiated
1920x1080` -> `ffmpeg -f dshow -i "video=PhoneCam (Windows Virtual
Camera)"` captures real frames (dark-scene content, matching the phone's
physical surroundings at test time -- confirmed genuine via per-frame
`signalstats` luma variance, not a flat/corrupted image) all the way
through the vcam. This is the same proof shape every other transport
milestone in this document uses, now satisfied for AOA with no Zadig step
anywhere in the chain.

## Status: `--revert-all`, built and verified -- not optional, and needed a real fix beyond the obvious one

Asked directly: does unplug/replug undo the driver binding, making the
undo path unnecessary? No -- confirmed live. A Windows driver binding is
a persistent PnP association keyed to the device's hardware ID, not tied
to the current plug session; replugging only resets the AOA mode switch
itself (back to the normal-mode PID), not the driver bound to that PID.
Without an explicit revert, a phone that went through AOA setup once
would have file-transfer/MTP permanently broken on that PC, every future
plug-in, with no obvious way back. `--revert-all` is required, not a
nice-to-have.

**First implementation was incomplete, caught by live testing.**
`SetupUninstallOEMInfW` (removing the package from the driver store) does
**not** make an already-bound, currently-present device drop its current
driver -- confirmed live: after removing the package, a physical
replug left the device still referencing the now-deleted `oem*.inf`
(shown as a garbled/truncated driver name in `pnputil`'s output, since
the file it points to no longer exists). Fixed with a second, explicit
per-device step: `DiUninstallDevice` (newdev.h) on any currently-present
device matching a recorded package's VID/PID, which un-assigns its
current driver and asks PnP to re-select one. This meant changing what
gets recorded at install time (`RecordInstalledPackage`) from just the
`oem*.inf` name to `name + VID + PID`, so revert can re-locate the
device later.

**`DiUninstallDevice` alone still wasn't enough, caught by a second
round of live testing.** It doesn't just unbind the driver -- it can
remove the devnode entirely. The still-physically-attached phone
briefly vanished from `pnputil /enum-devices /connected` altogether (not
merely "driverless"), and Windows did not automatically rediscover it.
Fixed with a third step: an explicit full PnP rescan
(`CM_Reenumerate_DevNode` on the root devnode, the API equivalent of
`pnputil /scan-devices`), run automatically at the end of `--revert-all`.
Confirmed live, no manual `pnputil` command and no physical replug
needed: `ForceUninstallDeviceDriver` -> `TriggerFullPnpRescan` ->
`pnputil` immediately shows `Driver Name: wpdmtp.inf`, `Status: Started`
again. Verified twice more independently by the user directly (once
choosing "File Transfer" when the phone's own USB-mode prompt reappeared,
once leaving it on "No data transfer") -- both times the Windows-side
driver came back correctly regardless of which mode was chosen on the
phone, confirming the fix is about the PC-side binding specifically, not
contingent on any particular phone-side USB mode.

`RemoveSelfSignedCertsFromStore` (matching on libwdi's distinctive
`"(libwdi autogenerated)"` certificate-subject suffix, safe because that
string is specific enough not to plausibly collide with anything already
on a real machine) also cleaned up two leftover certificates from this
session's much earlier manual Zadig/WCID experiments as a side effect --
confirms the cert cleanup is thorough, not scoped too narrowly to just
this tool's own installs.

**Deliberately not built yet:** the host-side UX around this helper
(tray balloon offering setup, the consent dialog, `ShellExecuteExW`
elevation wiring, decline/remember state, and a tray "Remove USB driver"
item wired to `--revert-all`) -- the plan's Phase 2 design for these
still stands, this pass proved the mechanism (`phonecam-usbdriver.exe`
itself, both install and revert) works and is sound to build the UX
around. Also not yet done: wiring this into
`windows/installer/PhoneCam.iss` and testing on a phone this specific
dev machine has never touched (the Note 7 remains the right device for
that, once the composite-guard and pre-staging above are exercised
against it too).

**`adb` conflicts with `libusb` over USB, but not over Wi-Fi.** `adb`
holding the phone's ADB interface open (as soon as its server sees the
device) causes libusb's control-transfer handshake to fail with
`LIBUSB_ERROR_ACCESS` -- `adb kill-server` before every `aoa_probe`
handshake attempt is required, not optional. **Wireless debugging (`adb
connect <ip>:<port>`, port discovered via `adb mdns services`) has no
such conflict** -- it's a plain TCP connection, unrelated to the USB
interface libusb needs -- and turned out to be essential for this
project's own verification workflow (see below), not just a convenience.

## Status: Phase 7 checkpoint extended (bidirectional + multi-packet, proven), 2026-08-19

Per an advisor review before writing any real transport code: the
original checkpoint (`AoaAccessoryTransport`'s heartbeat, read
successfully by `aoa_probe`) proves only a one-directional, 21-byte,
single-USB-packet pipe. The real transport needs three things that
checkpoint says nothing about: writing host->phone, payloads larger
than one bulk transfer, and sustained throughput. Extended `aoa_probe`
(still throwaway, still not shipping product) with two OUT-direction
tests -- a small text marker and a 128KB length-prefixed payload -- sent
over the same accessory pipe right after the existing read checkpoint,
using a 1-byte tag + self-delimiting body framing that rehearses (but
is not) the real `wire-protocol.md` Channel-tag design.

**Verifying the phone-side result required working around a dead end:
`adb logcat` is unusable while this app holds the accessory open.**
Confirmed thoroughly, not assumed: zero app-generated log lines appear
for any tag, or for the app's entire PID, while it sits backgrounded
holding the AOA pipe open -- MIUI appears to silently drop this app's
own `Log.*` output in that state, even though the process is
demonstrably alive (visible via other apps' system-generated log lines
referencing it, e.g. `RenderInspector`). Fix: instead of relying on
`adb logcat`, the phone-side reader now writes a short `ACK:...` line
back over the same pipe after verifying each test, and `aoa_probe`
reads for it after sending -- closing the verification loop over USB
alone, with no dependency on logcat. Wireless `adb` (see above) remained
useful for the orthogonal problem of installing/relaunching the app
between code iterations without needing a physical replug each time.

**A real, load-bearing bug found and fixed via this testing: the
accessory pipe requires reads sized to the incoming transfer, not
small/single-byte reads.** The first version of the phone-side reader
did 1-byte-at-a-time `stream.read()` calls to find framing boundaries
(a tag byte, then a byte-by-byte scan for `'\n'`). Confirmed live this
stalls the whole pipe, not just that one read: the host received an
immediate ACK for the tag byte of a 24-byte marker frame, then nothing
further, and the host's *next*, entirely separate write (the 128KB
frame) then timed out completely (`LIBUSB_ERROR_TIMEOUT`, 0 bytes
written in 10s) trying to write to the OUT endpoint at all. Explanation:
the accessory pipe is backed by a raw USB transfer queue, not a
buffered stream -- a `read()` smaller than a queued transfer only
consumes part of it, and the remainder is not retained for the next
`read()` to pick up. The 1-byte read draining a 24-byte transfer
permanently lost the other 23 bytes, desyncing the parser (stuck
waiting for a `'\n'` that had already gone by) and leaving the
endpoint's queue undrained, which is what stalled the next write. Fixed
by rewriting the reader to always read into a generously-sized buffer
(16KB) and parse complete frames out of an in-memory accumulator,
carrying over only genuinely incomplete trailing bytes -- a resumable
state-machine parser over accumulated bytes, per the advisor's original
recommendation, not "read tag, then blocking-read N bytes."

**A second, secondary finding, not yet root-caused: repeatedly
reopening the same accessory session (many `adb install -r` + relaunch
cycles without an intervening physical replug) eventually left the
128KB write timing out even with the reader fix in place**, while a
fresh session (physical unplug/replug) immediately worked cleanly. Not
investigated further -- noted as "don't loop many app-reinstall cycles
against one long-lived accessory session during development; replug
between rounds if things start timing out for no visible reason."

**Verified live, clean run, both tests passing correctly (`aoa_probe
--handshake` on a freshly replugged device):**
```
Received 21 bytes: PHONECAM-AOA-PIPE-OK.
OUT text-marker test: wrote 24/24 bytes, status=LIBUSB_SUCCESS
OUT length-prefixed test: wrote 131077/131077 bytes, status=LIBUSB_SUCCESS
  <- ACK:text:len=22.
  <- ACK:bin:len=131072:ok=1.
```
`ok=1` confirms the phone reassembled and byte-verified the full 128KB
payload against its expected deterministic pattern -- not just "some
bytes arrived." This closes out all three gaps the advisor flagged:
bulk OUT works, multi-packet payloads far larger than
`wMaxPacketSize=512` reassemble correctly, and (implicitly, from timing
across these runs -- 34-141ms for 128KB) throughput is well above what
6Mbps 1080p video needs. **The real `AoaTransport` (host) /
`AoaAccessoryTransport` integration into the actual capture pipeline is
next**, per the advisor's explicit scope guardrail: build it *alongside*
the existing adb transport, selected by a CLI flag matching the
`--test-pattern`/`--test-transport` precedent, not as a replacement --
the adb path is still the only end-to-end-proven production pipeline,
and this AOA path's session/reopen fragility (previous finding) isn't
understood well enough yet to make it the sole transport.

## Status: Phase 7 real `AoaTransport` (Android side + host test harness), in progress

Per a second advisor consult before writing the Windows-side class: build
Android's real multiplexed transport and prove it with the existing
`aoa_probe` as the receiver *first*, since the Windows reader has nothing
genuine to decode until Android actually emits Channel-tagged video --
building both sides blind and debugging them together was exactly the
pattern that cost the most time earlier in this phase.

**Landed:**

- `WireFraming.writePacket` now makes exactly one `OutputStream.write()`
  call for header+payload combined (was two). Required for
  channel-tagging: a wrapper that tags each `write()` call would
  otherwise insert a stray second tag mid-packet. Behavior-preserving
  for the existing adb-socket path (one syscall instead of two, if
  anything cheaper).
- `transport/AoaTransport.kt` (Android) -- the real thing, distinct from
  the Phase-7-checkpoint-only `AoaAccessoryTransport`. `videoOutput` is
  an `OutputStream` that tags every `write()` call as `Channel.Video`;
  `writeControlMessage()` tags as `Channel.Control`. The reader parses a
  1-byte-tag + self-delimiting-body accumulator (same resumable-
  state-machine shape proven in the checkpoint's OUT-direction tests),
  grown/compacted via `System.arraycopy` rather than reallocating a
  fresh array per `read()` call -- an advisor-flagged concern: sustained
  video (~30 packets/sec) would otherwise put real GC pressure on the
  hot path that a one-shot 128KB test never exercised.
- `CaptureController.start()` gained an optional `videoSink: OutputStream?`
  parameter. Non-null skips `VideoSocketServer.open()`/`accept()`
  entirely and hands that stream straight to `H264Encoder`; null (the
  default) is byte-for-byte the existing Phase 3 behavior. This is the
  entire integration seam -- confirmed, not assumed, that `H264Encoder
  .start(out: OutputStream, ...)` doesn't care what kind of stream it
  gets.
- `MainActivity` has **TEMPORARY** test wiring (clearly marked, matching
  this project's established practice for scaffolding that isn't the
  final product path -- see Phase 3C's front-camera-default precedent):
  on accessory attach, it opens the real `AoaTransport` and immediately
  starts a hardcoded 720p30 `CaptureController` session over it,
  bypassing the normal Start-button/`CaptureService`/adb-socket flow
  entirely. The real integration (AOA as a user-selectable alternative
  behind that normal flow) is later work, once this proves the framing.
- `aoa_probe --test-video <output.h264>` -- the Phase-3B-equivalent
  proof, over AOA instead of adb: reads the accessory pipe, keys on the
  Channel tag first and only then trusts `WireFraming`'s header fields
  (an advisor-flagged correctness point -- never resync by scanning for
  the header's own magic byte, which could false-lock onto payload
  data), strips tag+header, and appends the raw Annex-B payload to a
  file. Same discriminating check as Phase 3B: if the file decodes
  cleanly with zero `ffmpeg` errors and the frame count matches, the
  framing is byte-correct end to end.

**Verified live, twice, real camera content, zero corruption:**
```
Done. Total: 1 config, 924 frames (31 keyframes), 1824143 payload bytes.
Done. Total: 1 config, 925 frames (31 keyframes), 10384207 payload bytes.
```
Both 30s runs at the 720p30 test-wiring default: `ffmpeg -i out.h264 -f
null -` reports zero decode errors on both, frame count matches exactly
(924/925), correct `1280x720` resolution recovered from the stream's own
SPS. Extracted frames show real (if dark-scene) sensor content with
visible compression noise, not flat/corrupted data. ~30.8fps sustained
matches the 30fps target. This is the full Phase-3B-equivalent proof,
now over AOA instead of adb.

**A real bug found and fixed getting here, not a design flaw:** the
first version of `AoaTransport.writeControlMessage`/the private tagging
helper it shared with `videoOutput` prepended a `[tag][4-byte length]`
header to *every* channel uniformly -- but video bodies
([WireFraming]'s packets) already carry their own embedded
`payload_len` and don't want an external one. The extra 4 bytes
silently shifted every field after the first packet, and the very first
live run showed exactly that signature: 1 CONFIG parsed correctly (its
fields happened to still land right), 0 FRAMEs ever recognized. Fixed
by only tagging (no length prefix) on the video path, keeping the
length prefix on the control path where it's actually needed (raw
FlatBuffers bytes aren't self-delimiting on their own).

**A second fix, advisor-flagged before it caused a real symptom:**
`AoaTransport`'s internal write helper silently swallowed every
`IOException`, meaning a broken accessory pipe would never surface to
`H264Encoder`'s existing `onWriteError` callback -- the encoder would
keep "streaming" into a dead connection indefinitely instead of tearing
down through the same path the adb transport already relies on. Fixed
by letting the exception propagate; confirmed `H264Encoder.drainLoop`
already catches `IOException` around its write and invokes
`onWriteError`, so this reuses an existing, proven path rather than
adding a new one. Both fixes landed before the two clean runs above, in
the same session -- not yet isolated which one (if either) actually
explains the earlier "many reinstall cycles -> writes start timing out"
finding, since it hasn't recurred since, but per the advisor's explicit
guidance this is treated as resolved-by-elimination rather than
requiring new recovery machinery (explicitly advised against: USB
reset, watchdogs, re-handshake-on-stall) unless it resurfaces.

**Dev-workflow finding: once a phone is in AOA accessory mode, `adb`
cannot reach it over USB at all, and there is no clean software-only way
back to normal mode.** This matters a lot for iteration speed, since
testing any Android code change requires reinstalling the APK. Confirmed
this session:

- Normal USB `adb` genuinely cannot connect while the device is
  enumerated in accessory mode (`adb devices` sits at "- waiting for
  device -" indefinitely) -- not a timing issue, retried well past any
  reasonable enumeration delay.
- **Wireless debugging (`adb connect <ip>:<port>`, discovered via `adb
  mdns services`) is the only channel that reaches the phone in this
  state**, since it's a plain Wi-Fi TCP connection with no dependency on
  the USB interface at all -- confirmed it doesn't conflict with
  `libusb` either (unlike USB `adb`, which requires `adb kill-server`
  before every `aoa_probe` handshake). This made several rounds of
  install-and-relaunch possible without physically replugging.
- **But `adb mdns services` discovery is not reliable** -- it found the
  phone's service once, then repeatedly found nothing on later attempts
  in the same session, with Wi-Fi and the Wireless debugging toggle both
  confirmed on by the user each time. No root cause identified (possibly
  a Windows Firewall/mDNS-multicast quirk, possibly router-side --not
  investigated further). When it fails, the phone's Wireless debugging
  settings screen shows the IP:port directly and can be read off that
  screen instead of relying on discovery.
- There is genuinely no host-triggered way to force the phone out of
  accessory mode short of a physical unplug/replug -- consistent with
  the [libwdi/UsbDk issue](https://github.com/daynix/UsbDk/issues/34)
  found earlier this phase. If wireless adb isn't reachable, a replug is
  the only path back to a state where new code can be installed and
  run.

## Status: Phase 7 real `AoaTransport` (Windows host side), verified live end-to-end, 2026-08-19

Built the host-side counterpart to `AoaTransport.kt`: `windows/host/transport/AoaTransport.h`/`.cpp`
(class `AoaVideoTransport`), a direct port of `aoa_probe`'s already-debugged
`OpenAccessoryInterface()`/`RunHandshake()`/`RunTestVideo()` logic into
production shape. Video channel only, per an advisor review before writing
any code -- control still runs over adb (`control/ControlTransport.h`); AOA's
control-tagged frames are recognized (skipped, not dispatched) so the demuxer
doesn't mis-resync on them, but nothing consumes them yet.

**Design:**

- **Common interface, not a subclass relationship:** extracted `VideoPacket`/
  `PacketType`/`VideoPacketCallback` out of `AdbTransport.h` into a new
  `transport/VideoTransport.h`, and both `AdbVideoTransport` and the new
  `AoaVideoTransport` implement its `Connect()`/`Disconnect()`/
  `RunReceiveLoop()` interface. `AdbVideoTransport::Connect()` lost its
  `port` parameter (nothing called it with a non-default value) so its
  signature matches the interface exactly. `LiveVideoBridge` now holds a
  `std::unique_ptr<VideoTransport>` (default-constructed to
  `AdbVideoTransport` for every existing call site) instead of owning a
  concrete `AdbVideoTransport` -- its `Run()` supervise/reconnect loop
  (connect, stream until disconnect, back off, retry) needed **no other
  change** to work with AOA: it already treats "connect failed" and
  "receive loop ended" identically and retries after a backoff, which is
  exactly the right behavior for a transport with no clean reconnect --
  repeated failed `Connect()` calls (logged, backed off) until the user
  physically replugs is correct, not a bug to route around.
- **`AoaVideoTransport::Connect()`** first tries to open an
  already-re-enumerated accessory device (`OpenAccessoryInterface`, ported
  verbatim from `aoa_probe`); if that fails, it looks for the phone in
  normal mode and runs the AOA handshake (`RunHandshake`, also ported
  verbatim) to switch it into accessory mode, then retries the open. This
  means `phonecam-host --aoa` triggers the mode switch itself -- the user
  doesn't need `aoa_probe --handshake` as a separate step for normal
  operation (that tool remains useful for isolated diagnosis).
- **Reader demuxer** is the same buffer-accumulator parser as
  `RunTestVideo`, ported as the class's `RunReceiveLoop`: never a read
  sized to one message, keys on the Channel tag before trusting header
  fields, drops-and-resyncs on an unrecognized tag. Dispatches
  `Channel.Video` frames as `VideoPacket`s (matching `AdbVideoTransport`'s
  existing shape, so `LiveVideoBridge` needed no changes downstream of the
  transport interface); skips `Channel.Control` frames without parsing
  their contents.
- **Shutdown-safety note, not present in the adb transport:** libusb
  documents closing a device handle while a synchronous transfer is
  in-flight on another thread as unsafe (unlike a Winsock socket, where
  `shutdown()` safely unblocks a concurrent `recv()`). `RunReceiveLoop`
  holds its transport's mutex for the duration of each individual
  `libusb_bulk_transfer` call (bounded to a 200ms timeout), not the whole
  loop -- so `Disconnect()`, possibly called from another thread, blocks
  for at most ~200ms acquiring that lock before it's safe to close, rather
  than racing a close against an in-flight transfer.
- **CLI wiring:** `phonecam-host --aoa` selects `AoaVideoTransport` in
  place of the default `AdbVideoTransport`, added alongside the existing
  flag set (`--test-decode`, `--test-transport`, `--test-pattern`) in
  `main.cpp`, not replacing the default. `windows/host/CMakeLists.txt`
  gained the same `third_party/libusb` include/lib/DLL-copy wiring
  `aoa_probe`'s `CMakeLists.txt` already had.

**Verified live**, same session, phone in File Transfer mode (the
established reliable AOA trigger): `adb kill-server` (required, same
adb-vs-libusb conflict as always), then `phonecam-host.exe --aoa` --
logged `AoaVideoTransport: connected` followed by
`MFH264Decoder: output negotiated 1280x720`, i.e. the handshake, the
re-enumeration wait, the interface claim, and real decoded frames all
worked with zero manual intervention beyond the one adb-kill step. (The
concurrent `ControlTransport: adb forward exited with code 1` errors are
expected and harmless -- the control channel still needs `adb`, which was
deliberately killed for the video test; this is the documented
non-fatal-to-video behavior.) Then, the actual discriminating check
(Phase 3C's own standard, not just a dumped file): `ffmpeg -f dshow -i
video="PhoneCam (Windows Virtual Camera)"` captured 90 real frames
through the *entire* pipeline -- `AoaVideoTransport` -> `MFH264Decoder` ->
`SharedFrameRing` -> `MFCreateVirtualCamera` -> DirectShow -- and a
decoded frame showed real, correctly-exposed live camera content (a dim
indoor scene), not corrupted or blank data. This is the same proof shape
Phase 3C used for the adb transport, now repeated for AOA.

**Deliberately not done in this pass, per the advisor's scope guardrail**
(don't restructure more than necessary to prove the video path): control
channel over AOA, removing `MainActivity`'s TEMPORARY test wiring in favor
of real Start-button/user-selectable-transport integration, and installer
driver auto-install. See the top-level plan for these as separate,
explicitly deferred items -- the Start-button integration in particular
needs a product decision (auto-detect vs. explicit toggle vs.
AOA-preferred-with-adb-fallback) that hasn't been made yet, not just more
implementation.

## Status: Phase 7 real Android-side integration (auto-detect), verified live end-to-end, 2026-08-19

Decision (asked of the user directly, not inferred): **auto-detect** --
an accessory attach starts capture on its own, no Start-button tap
needed, closest to what `MainActivity`'s TEMPORARY wiring already did.
This session replaced that TEMPORARY wiring with the real thing, routed
through the same `CaptureService` the normal adb/Start-button path
already uses, rather than a separate ad-hoc path:

- **`CaptureService` gained `ACTION_START_AOA`** (extra: a `UsbAccessory`
  Parcelable), alongside the existing `ACTION_START`/`ACTION_STOP`. It
  opens an `AoaTransport` itself (previously `MainActivity` owned this)
  and calls `CaptureController.start(videoSink = transport.videoOutput)`
  -- meaning an AOA session now gets the exact same foreground-service
  type, wake lock, and `onCaptureError`/`lastError` handling the adb path
  already has, instead of none of that. Resolution changed from the
  TEMPORARY wiring's hardcoded 1280x720 to 1920x1080/30fps/6Mbps, matching
  `MainScreenViewModel`'s own Start-button defaults and, more importantly,
  the vcam's declared 1080p stream size (`windows/vcam/MediaStream.cpp`)
  -- the TEMPORARY wiring's 720p would have silently stretched on the PC
  side, the same bug class Phase 3C already fixed once before.
- **Both start actions now guard against a redundant dispatch while
  already streaming** (`if (controller.state != CaptureState.IDLE)
  return`) -- not just AOA's own re-launch-while-attached case (the old
  TEMPORARY wiring's `aoaTransport.isOpen` check, moved here), but also
  the adb path's own auto-start effect (below), which can now race an
  AOA-triggered session it has no other way of knowing about.
- **`MainActivity`** no longer owns any transport or controller -- 
  `handleAccessoryIntent` now just resolves the `UsbAccessory` (unchanged
  logic) and forwards an `ACTION_START_AOA` intent to `CaptureService`.
- **`MainScreenViewModel`'s poll loop now runs continuously from `init{}`**
  instead of only being started inside `startCapture()`. This was a real
  gap, not a style preference: `MainScreen`'s own auto-start effect calls
  `startCapture()` once whenever it sees `Idle`, and previously that was
  the *only* thing that ever started polling -- so a session that
  `MainActivity` triggered via `ACTION_START_AOA` (never touching
  `startCapture()`) would stream successfully while the UI kept showing
  stale "STANDBY / tap start to begin transmitting" forever. Polling now
  reflects `CaptureService`'s real state regardless of which path started
  it.

**Verified live, this session, with zero manual taps on the phone**:
`phonecam-host.exe --aoa` (adb killed first, as always) sent the
handshake; the phone auto-launched the app via `accessory_filter.xml`'s
intent match (the "always allow" AOA dialog checkbox from earlier this
phase meant no dialog reappeared either); `MainActivity` forwarded
straight to `CaptureService`; the log showed `AoaVideoTransport:
connected` followed by `MFH264Decoder: output negotiated 1920x1080`
(confirming the new resolution, not the old TEMPORARY 720p); and the
phone's own screen, checked directly, showed the real "ON AIR" status
with a live FPS/RES/TIME readout -- confirming the `MainScreenViewModel`
polling fix, not just the capture pipeline. A second DirectShow capture
(same method as the transport-only proof above) through the *real*
non-TEMPORARY path showed the same live scene at full native 1920x1080,
sharper than the earlier 720p-then-upscaled-by-nothing capture since
there's no resolution mismatch anymore.

**Still deliberately deferred**, unchanged from above: control channel
over AOA, and installer driver auto-install (`libwdi` against the parent
composite device's hardware ID, per the corrected shipping note earlier
in this document).

## Status: transport auto-detect (adb preferred, AOA fallback), verified live

`phonecam-host.exe` with no CLI flag now runs `adb devices` once at
startup (`DetectAdbDevicePresent()`, `windows/host/main.cpp`) and picks
`AdbVideoTransport` if a real device is listed (state `device`, not
`unauthorized`/`offline`/absent), `AoaVideoTransport` otherwise -- so a
debugging-enabled phone always gets the proven, zero-setup adb path, and
AOA is only ever attempted when adb genuinely can't reach anything.
`--aoa`/`--adb` remain as explicit dev overrides, unchanged.

Runs once at startup, not continuously -- a given phone/PC setup is
consistently debugging-on or debugging-off in practice; picking up a
change means relaunching the host (already easy via the tray's Exit
item), not restructuring `LiveVideoBridge` to swap transports mid-run,
which the reconnect loop doesn't support and isn't worth building for an
edge case.

Verified live with debugging off: logs `No debugging-enabled phone
detected -- using AOA`, then (since the driver from
`windows/usbdriver/` wasn't installed at the time of this specific test)
`AoaVideoTransport::Connect()` fails and retries with backoff exactly
like the adb path already does when no phone is streaming -- confirms
the fallback is graceful, not a crash or hang, even in the
worst case (no driver, no phone streaming, nothing to connect to at
all). The debugging-on -> adb-preferred direction is the same
`DetectAdbDevicePresent()` call with different `adb devices` output and
was not separately re-verified live this pass, since the two branches
share identical code and the adb path itself is already proven
extensively elsewhere in this document.

## Status: driver-setup helper wired into `phonecam-host.exe` itself, verified live -- Phase 2 is now a real product feature, not a CLI tool

Previously `phonecam-usbdriver.exe` was only invokable by hand (a PowerShell
script running it elevated). Now `phonecam-host.exe`'s tray menu has two
real items -- **"Set up USB driver for AOA..."** (shown only when the host
picked AOA, i.e. no debugging-enabled phone was found) and **"Remove USB
driver (restore file transfer)"** (always available) -- each launching the
helper via `ShellExecuteExW`'s `runas` verb on a dedicated worker thread
(`windows/host/main.cpp`'s `RunUsbDriverHelperElevated`), so the one UAC
prompt and the install itself never block the tray's own message loop.
`phonecam-host.exe` itself stays `asInvoker` throughout -- only the small
helper ever elevates.

This needed two small preparatory additions:
- **`TrayIcon` gained real extensibility** (`SetMenuItems`, `MenuItem`):
  a callback invoked fresh every time the context menu opens, appending
  items (with click callbacks, checkable state) between the status line
  and Exit. Minimal on purpose -- no submenus yet, dynamic IDs starting
  at `WM_APP`-adjacent 200, looked up in a per-open-rebuilt vector rather
  than a permanent table. This is the same mechanism Phase 4's future
  camera/rotation menus will use.
- **`phonecam-usbdriver.exe --install` gained auto-detect** (no
  `--vid`/`--pid` needed): scans present devices for the first
  non-composite match against a short, project-specific known-Android-VID
  list (Xiaomi + Google), rather than requiring the host to already know
  the phone's exact hardware ID. Keeps `--vid`/`--pid` as an explicit
  manual override for testing.

**Verified live, the complete real user flow, start to finish:** phone
attached, debugging off, driver not yet installed -> `phonecam-host.exe`
launched with no flags, tray shows "waiting for phone..." plus both new
menu items -> clicked "Set up USB driver for AOA..." -> one UAC prompt,
approved -> `pnputil` confirms the binding (`Class Name: libusbk
devices`) -> and, with no further action at all, the *same already-running*
`phonecam-host.exe`'s own background `LiveVideoBridge` retry loop picked
up the newly-available driver on its next attempt and logged
`AoaVideoTransport: connected` -- the phone had already re-enumerated
into accessory mode (`PID_2D00`) by the time the install finished. No
restart, no second click, no separate tool run by hand. This is the
complete product experience the whole Phase 2 investigation was building
toward.

**Not yet done:** the automatic *proactive* offer (a balloon/prompt shown
once, on its own, if AOA has been failing to connect for a while) --
today the tray items are always there to click manually, which is the
core requirement satisfied, but nothing nudges a first-time user toward
them yet. Also still open: wiring `phonecam-usbdriver.exe` into
`windows/installer/PhoneCam.iss` so it actually ships next to
`phonecam-host.exe`, and testing this whole flow on a phone this dev
machine has never touched (the Note 7, once the composite-guard and
pre-staging are exercised against it too).

## Phase 3: vcam real 720p/1080p + rotation/mirror, built and verified --
## with one confirmed Windows platform bug scoped and documented

Two structural pieces landed together, since the second depends on the first:

**Resource-lifetime split (`FrameGenerator`).** Previously `SetD3DManager`
did double duty -- opening the D3D device handle *and* allocating every
GPU/D2D resource at a hardcoded 1920x1080, with `HasD3DManager()`
conflating "GPU mode" with "resources are sized correctly". Split into
`SetD3DManager(IUnknown*)` (device handle only) and `EnsureRenderTarget(w,
h)` (idempotent -- no-ops if already sized correctly, else tears down and
rebuilds everything including a fresh `CLSID_VideoProcessorMFT`, never
re-typed on a live MFT). `MediaStream::Start(IMFMediaType*)` now actually
reads the negotiated `MF_MT_FRAME_SIZE` and calls `EnsureRenderTarget`
with it, instead of ignoring it and always using 1920x1080. Also fixed in
passing: `SetStreamState`'s `if (_state = value)` -- assignment, not
comparison, silently skipping the Start()/Stop() calls on every
PAUSED/RUNNING transition.

**8 media types, real app-negotiated resolution.** `MediaStream::
Initialize` now declares RGB32 + NV12 at four sizes (1920x1080, 1280x720,
1080x1920, 720x1280) instead of two hardcoded-1080p types -- confirmed via
`ffmpeg -f dshow -list_options` showing all four sizes across `bgr0`/
`yuyv422`/`nv12`. This is genuinely how a UVC webcam exposes resolution:
**the consuming app picks the size**, there's no PC-side "push
resolution" mechanism (`MFVirtualCamera::Stop()`/`Start()` would make the
device disappear from a running consumer). Also fixed a real latent bug
found while rewriting this: NV12's `MF_MT_DEFAULT_STRIDE` was declared as
`width * 1.5` (the whole-frame byte multiplier) instead of the Y-plane
stride (`width`) -- evidence this type had never been exercised by a real
consumer before now.

**Rotation/mirror: `SharedFrameRing` header, `_v1` -> `_v2`.** A packed
`std::atomic<uint32_t> transform` field (2 bits rotation quarter-turns +
1 bit mirror) plus `magic`/`version` fields were added to `RingHeader`.
This is a *standing* setting, not per-frame -- it belongs in the ring
because the vcam DLL runs inside the Frame Server's Session-0
service-account process, invisible to `HKCU`, and because it must survive
across periods with no frames being written (phone disconnected). The
mapping/event names were bumped `PhoneCam_FrameRing_v1` ->
`..._v2` in the same change, on purpose: a name bump makes an old/new
binary mismatch structurally impossible to alias onto the same section,
rather than relying on both sides remembering to check a version field.
`WriteFrame`'s bound also relaxed from separate `width > kMaxWidth ||
height > kMaxHeight` checks to a single byte-count bound -- not required
today (rotation stays PC-side, portrait frames never enter the ring) but
removes a latent 1080x1920-shaped landmine for any future phone-side
work. `FrameGenerator::DrawLatestRingFrameOrWaitingMessage` applies the
transform as a D2D `SetTransform` composed as rotate -> mirror -> scale
-> translate (mirror always composed *after* rotation, so "mirror" keeps
meaning "flip what the viewer sees" regardless of the chosen rotation),
scaling the rotated source to exactly fill the negotiated canvas
(`scale = 1.0` exactly for the headline case: a 1920x1080 sensor frame
rotated 90 into a 1080x1920 canvas). `LiveVideoBridge` gained thin
`SetTransform`/`GetNegotiatedSize` forwarders (the latter fed by
`FrameGenerator` publishing `_width`/`_height` into the ring's header on
every draw) so Phase 4's tray can reach both without touching the ring
directly. Rotation itself has not yet been tested against a live rotated
frame (needs the phone attached, next session) -- the matrix math was
independently checked by the advisor and the identity case (0, no
mirror) is provably unchanged from the pre-Phase-3 behavior, but this is
still open.

### A real, confirmed Windows platform bug: portrait sizes corrupt through DirectShow, not through native Media Foundation

Pulling the new portrait sizes (1080x1920, 720x1280) through `ffmpeg -f
dshow` (any pixel format -- `nv12`, `bgr0`, `yuyv422` all equally
affected) produced a reproducible diagonal shear/green-noise corruption
across the *entire* frame, including flat background regions with no
rendered content. All four landscape-equivalent widths that had ever been
exercised before (1920, 1280) were clean; only the two new portrait
widths (1080, 720) broke. Isolated with three targeted tests rather than
guessed at:

1. **Added a throwaway 1024x1920 size** (1024 x 4 bytes/px = 4096, a
   multiple of 256; still portrait). Clean. This separates the two
   hypotheses that were otherwise perfectly confounded on 4 data points
   (1080/720 fail, 1920/1280 don't) -- "portrait orientation" and "row
   byte-width not 256-byte-aligned" -- and confirms alignment, not
   orientation, correlates with the bug.
2. **Forced `IsGpuMode()` to `false`**, redeployed, re-pulled 1080x1920.
   Identical corruption. This rules out our own GPU-vs-CPU rendering
   choice as the cause: the CPU/WIC path builds its NV12 output using the
   *actual* destination sample buffer's pitch (from `Lock2DSize`), which
   is provably self-consistent regardless of width -- if even that path
   shows the bug, the corruption isn't in anything `FrameGenerator`
   produces.
3. **Wrote a throwaway native Media Foundation probe**
   (`IMFSourceReader`, not DirectShow) to pull the same 1080x1920 and
   720x1280 NV12 samples directly. Both perfectly clean, buffer size
   exactly matching the tightly-packed expectation
   (`width*height*1.5`, no padding).

Conclusion: the corruption is 100% confined to the Windows Frame
Server's DirectShow bridge (`VCAMDS`, visible in its own log lines when a
dshow app opens the camera) -- the closed-source compatibility shim that
lets legacy DirectShow apps see an MF-based virtual camera at all, part
of the OS (`MFCreateVirtualCamera`'s own plumbing, not anything in this
repo's vendored/third-party code), not something this project can patch.
Native MF consumption (what Chrome, the Windows Camera app, and modern
capture backends use) is unaffected and was independently verified clean
for both portrait sizes with the real (GPU) render path restored.

**Practical impact, stated plainly:** portrait resolutions work correctly
in native-MF apps. They are expected to render corrupted in DirectShow-
based apps -- this specifically includes Zoom's classic Windows desktop
client and older/classic OBS capture, both DirectShow-based, both named
targets of this project. This was not tested against real Zoom/OBS
directly (no license/build available in this session) -- confirmed via
`ffmpeg -f dshow`, which uses the same DirectShow capture APIs those
apps do, so the same VCAMDS bridge is exercised. **No workaround was
attempted**: there is no alternate portrait size that is both
256-byte-aligned *and* a real 9:16 resolution apps would recognize, and
patching or bypassing `VCAMDS` is outside this project's reach. Landscape
sizes (1920x1080, 1280x720) are unaffected in every consumer tested,
including DirectShow.

## Phase 4: tray controls -- all 7, verified live over AOA (rotation/mirror), with a real
## AOA reconnect bug found and worked around along the way

Built in one pass: lens switch, zoom, exposure (EV), torch, focus mode, rotation, and mirror,
all as real tray menu items -- not stubs. Three pieces of plumbing had to land first:

- **`TrayIcon` gained real submenus** (`MenuItem::submenu`), replacing the flat list Phase 2
  left it with. `AppendItems()` recurses, building a child `HMENU` via `MF_POPUP` for any item
  with a non-empty `submenu` and otherwise falling through to the existing flat-item logic --
  dynamic-ID allocation and the `dynamicCallbacks` vector are unchanged, since Windows destroys a
  submenu's `HMENU` together with its parent, so nothing new needs explicit cleanup.
- **`ControlChannel` gained a capability/settings cache** (`GetLastCapabilities()`/
  `HasCapabilities()`/`GetLastSettings()`/`HasSettings()`), populated inside the existing
  `RunReceiveLoop` before the message reaches the caller's handler -- this is the single decode
  chokepoint every consumer already reads from, so the tray reads from here directly rather than
  duplicating `ConsoleControlUi`'s own private cache (that duplication stays, intentionally, for
  the console UI itself -- not worth touching working code for this).
- **`ControlTransport::Send` gained a dedicated send mutex**, separate from the existing
  connect/disconnect mutex so a send failing fast during teardown doesn't block on an unrelated
  lock. Was genuinely unsynchronized before this -- fine while only `ConsoleControlUi`'s console
  thread ever called it, a real bug once the tray's message-pump thread became a second
  concurrent sender (lens/zoom/ev/torch/focus clicks).

**What's live vs. not, honestly:** rotation, mirror, and the resolution display are fully
PC-side (no phone involvement at all) and were verified live end-to-end, including the default
(below). Lens/zoom/exposure/torch/focus are fully wired -- `ControlChannel::Send*` calls fire
correctly -- but only *reach* the phone over the adb control channel, which is still adb-forward
-only (see `AoaVideoTransport`'s class doc). Over AOA their submenus simply don't appear at all
(no capabilities have ever arrived to build them from), matching the existing "never show a
control the phone doesn't support" rule rather than showing dead menu items. **Decided not to
build a control channel over AOA** to close this gap -- discussed directly: these five controls
are realistically set once per session (camera/zoom/torch/focus at setup, rarely mid-call), so
setting them by hand at the phone once is an acceptable tradeoff against the real cross-stack
work (Android accessory-pipe write path + a host-side transport abstraction change) that closing
it properly would need. This is a deliberate scope boundary, not a forgotten TODO.

**Default rotation is 90 clockwise, not 0** -- the reference mounting for this whole project is
the phone held vertically, and a landscape sensor frame needs exactly this rotation to look
normal; 0 would come out sideways for the common case. Applied automatically at startup
(`bridge.SetTransform` right after `vcam.Start` succeeds), not just offered as a tray option.

**Verified live:** pulled a frame from the running vcam with the default rotation in effect --
confirmed the pillarboxed-into-a-landscape-canvas geometry is exactly right (a vertical strip
centered with black bars either side, matching the fallback case documented in Phase 3 above)
without ever touching the tray. Rotation/mirror submenus themselves show the right checkmarks
against `currentRotation`/`currentMirror` (tray-thread-only state, see `main.cpp`'s comment on
why that's safe without a mutex -- `itemsProvider` and every `onClick` run on the same thread).

### A real AOA reconnect bug, found while testing this live, worked around not yet fixed

While testing over AOA tonight (phone: Redmi Note 8, MIUI, debugging off), the connection got
stuck **stuck-but-silent** more than once: `AoaVideoTransport::Connect()` succeeds (libusb
claims the accessory interface fine), logged as `AoaVideoTransport: connected`, but no bytes
ever arrive -- `RunReceiveLoop`'s `libusb_bulk_transfer` just times out forever
(`LIBUSB_ERROR_TIMEOUT`, silently `continue`d, by design -- see its own comment), never hitting
the `LIBUSB_ERROR_NO_DEVICE` branch that would let the reconnect loop retry. Confirmed this
happens even when the phone is physically replugged while the host process keeps running: a real
`Get-PnpDevice` check showed the phone back in normal mode (not accessory) while the *host's*
connection state hadn't budged -- the stale libusb handle from the previous accessory session
just never learns the device is gone.

**What actually unwedges it**, confirmed by repetition, not guessed: restarting
`phonecam-host.exe` itself while the phone is *not* currently in accessory mode. A graceful
`taskkill` (not `/F`) alone did **not** fix it once (the phone was still in accessory mode from
the stale session at that moment, so the new process just re-claimed the same lingering
connection without ever re-running `RunHandshake()`) -- what worked was the sequence: phone-side
Cancel -> physical unplug/replug (forces the phone genuinely back to normal-mode enumeration,
confirmed via `Get-PnpDevice`) -> *then* restart the host process, so its fresh `Connect()` call
actually goes through `RunHandshake()` again instead of finding an already-accessory-mode device
and skipping straight to `OpenAccessoryInterface()`.

**Not fixed, flagged for later:** `RunReceiveLoop` should detect a dead connection itself (e.g. a
consecutive-timeout counter that gives up and returns after some threshold, rather than looping
`LIBUSB_ERROR_TIMEOUT` forever) so the existing reconnect loop can recover without a manual
process restart. Real, reproducible, but out of scope for tonight -- noted here rather than
silently worked around with no trace.
