# Architecture

Current design: USB tethering and USB debugging (ADB), control protocol **v8**.
This document distills the former development journal; its full history is
preserved at [commit 253bd2d](https://github.com/Abdo-Eid/phonecam/blob/253bd2d/docs/architecture.md)
and on the local `archive` branch. Historical AOA/Wi-Fi/pairing implementations
are not current features.

## Pipeline and components

```text
Android: Camera2 → MediaCodec H.264
                     │
              USB-tethered TCP or ADB-forwarded local sockets
                     │
Windows host: MF H.264 decoder → NV12 → shared-memory frame ring
                                             │
Windows Frame Server: phonecam-vcam.dll → Media Foundation / DirectShow apps
```

| Component | Responsibility | Build |
|---|---|---|
| `android/` | Camera capture, encoder, USB listeners, control, UI and foreground service | Gradle/Kotlin |
| `windows/host/` | Transport selection, control, decoding, tray, virtual-camera registration | CMake/C++20 |
| `windows/common/` | Wire helpers, logging and shared frame-ring contract | CMake |
| `windows/svc/` | Creates and holds the cross-session frame ring | CMake |
| `windows/vcam/` | COM media source loaded by Windows Frame Server | MSBuild/NuGet |
| `windows/installer/` | Packages Release binaries, virtual camera and bundled ADB | Inno Setup |
| `proto/` | FlatBuffers schemas compiled by both endpoints | `flatc` |

The host runs as an ordinary user. The ring service and one-time installation
handle elevation; camera capture and decoding do not run in that service.

## USB connections

- **Tethering:** Windows finds the phone from the RNDIS/NCM adapter's gateway.
  Video is TCP 27183; control is TCP 27184. Android binds only to an IPv4 address
  on `rndis*`, `usb*`, or `ncm*`, never a wildcard, Wi-Fi or cellular address.
  The listener refreshes the address before accepting clients, allowing
  tethering to appear after the app opens.
- **ADB:** the phone exposes abstract sockets `phonecam_video` and
  `phonecam_control`. The host forwards local ports to those sockets. Android's
  debugging authorization remains required.

Automatic startup prefers tethering and otherwise chooses ADB. The tray can
explicitly switch paths without rebuilding the shared ring or re-registering
the virtual camera. Do not assume automatic mode continuously changes transport
or follows changed tether addresses: those recovery cases still need validation.

There is **no QR pairing or application authentication**. Tethered control and
video are plaintext; any peer able to reach the USB-tethered ports can use them.
Use a trusted host/link and do not bridge it to untrusted networks. See
[security.md](security.md) and [wire-protocol.md](wire-protocol.md).

## Android service lifecycle

`CaptureService` owns the durable control channel and a `CaptureController`.
The visible UI binds/polls the service; the controller owns one video capture
cycle. Control can start capture while the app is visible without depending on
an already-running camera.

**Only connected video streaming may outlive the UI.** Background eligibility
requires streaming state, delivered video frames, a live control connection,
no capture error and no deliberate Stop. On unbind, idle/waiting/retry/error
work loses its started-service state and notification; Android then destroys
the service and closes capture, listeners and wake lock. Eligibility is latched
at departure so queued Start/keep-alive commands cannot revive a closed retry.

A connected stream remains foreground when backgrounded and can be rebound by
the returning UI. If its control connection ends, capture fails, or capture is
stopped while hidden, the service exits rather than waiting for another PC.
Reopen PhoneCam to reconnect. Lifecycle exit does not latch a deliberate Stop
or erase settings. Notification updates/promotion/removal are serialized so a
late callback cannot leave an ordinary notification after teardown.

User Start/Stop work runs off the main thread. Video accepts use bounded polling
and per-capture cancellation tokens: closing `LocalServerSocket` from another
thread alone was not a reliable way to unblock an accept. Never let an old
capture generation consume a later generation's cancellation state.

## Capture pipeline

Camera2 renders directly into MediaCodec's input `Surface`; the phone does not
copy raw pixels through the application. The normal request is 1920×1080 at
30 fps, H.264 at 6 Mbps, with roughly one-second IDRs. Encoded packets use the
20-byte framing in [wire-protocol.md](wire-protocol.md); control uses separate
length-prefixed FlatBuffers envelopes.

The reference phone's Qualcomm encoder rejected some profile/low-latency keys.
Acceptance of a codec parameter is not proof it is honored or improves latency.
Optional benchmark knobs stay debug/telemetry-gated; Release retains the tested
defaults. Encoder experiments and uncertainty bounds are in
[bench/results.md](../bench/results.md).

Stats report measured encode rate, requested/adapted bitrate, estimated drops
and thermal status. The adaptive policy reduces bitrate under thermal pressure,
ramps toward the last requested ceiling when clear, and has a 1 Mbps floor.
Estimated drops are not hardware loss counters; thermal-pressure behavior was
not established by tests that only observed nominal temperature.

## Control channel

The phone sends `StreamIntent` before `CapabilityDescriptor`. The host waits for
compatible capabilities before requesting video and builds camera controls from
the descriptor, never assumed device features. Commands use nonzero `cmd_id` and
correlated acknowledgements; settings snapshots reconcile actual state.

Stop intent is stored separately from capture/error state. Automatic StartVideo
and reconnect must not clear it; only an explicit Start may do so. Stop gates
Windows output to black, including late frames, without removing the virtual
camera. A lightweight GetStreamIntent heartbeat lets the host detect stale
control connections even while stopped.

Implemented controls include lens, zoom, EV, torch, focus mode, tap-to-focus,
bitrate, keyframe requests and video Start/Stop. Manual exposure/WB,
stabilization and phone capture-size/FPS changes are not implemented. The
virtual camera's output-size negotiation is separate from phone capture size.
See [control-protocol.md](control-protocol.md).

FlatBuffers union members are ordinal: schema changes can renumber payloads.
Both endpoints must use v8; this version removes the old authentication payloads.
Rebuild/update both ends together rather than debugging mismatched wire types
as camera failures.

## Elevation and shared memory

Windows Frame Server loads the virtual-camera DLL in a service-account process
in Session 0. The host is in the interactive user's session, so the frame ring
uses the `Global\` namespace and a DACL allowing the relevant service accounts.
Creating the global mapping needs `SeCreateGlobalPrivilege`; opening an existing
mapping only needs the appropriate access rights.

`phonecam-svc.exe` creates/holds the ring as LocalSystem. The unprivileged host
and virtual camera open it. Moving capture/decoding into that elevated service
or running the tray app as administrator is unnecessary. Shared-memory layout
changes require version/name changes and coordinated binaries.

## Vcam resolution

The consuming app negotiates 1920×1080, 1280×720, 1080×1920 or 720×1280 in the
advertised media types. This is not a command to change the phone encoder.
Tray resolution is read-only. Rotation/mirror are shared-ring settings applied
PC-side; mirroring follows rotation. The reference mounting defaults to 90°
clockwise rotation.

Virtual-camera requests are asynchronous, paced by a rational 30 fps clock,
with increasing sample times and no catch-up burst. Restart invalidates queued
requests. Regular timestamps do not establish unique images or optical latency.

### Portrait through DirectShow

Portrait sizes were clean in a native Media Foundation reader but corrupted
through Windows' DirectShow compatibility bridge (`VCAMDS`). Three historical
checks isolated the issue: an aligned 1024-wide portrait test was clean;
forcing the CPU render path did not repair 1080-wide output; a native MF probe
read both actual portrait sizes cleanly. Landscape output remained usable.

No workaround is implemented. Do not infer native-MF correctness proves
DirectShow correctness, or report actual Zoom/OBS versions as tested merely
because an FFmpeg DirectShow probe passed.

## Quality/perf

HD CPU NV12/RGB conversions use BT.709 rather than the earlier BT.601 constants;
the GPU/CPU paths and destination pitch must remain consistent. GPU zero-copy
decode and slice-based transport redesigns are deferred, not measured wins.
Reference-relative host delay, network RTT, sample timestamps and file-write
age are not absolute glass-to-glass latency. Keep measurements within matched
binary/scene cohorts; [bench/results.md](../bench/results.md) preserves the data.

## Build, deployment and evidence

Building/testing a checkout host does **not** update the installed Windows
executable. The registered virtual-camera DLL is also a separate deployed copy;
its MSBuild post-build copy is best-effort and may fail if locked. Use
[build.md](build.md) for exact build/deployment steps, not historical commands.

Current reference-device evidence and deployment status:
[USB and lifecycle validation](usb-validation-2026-11-08.md). Outstanding work is
in [plan.md](../plan.md). The closed wireless investigation is distilled in
[networking.md](../networking.md); it is not an active feature roadmap.
