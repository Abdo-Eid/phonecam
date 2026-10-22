# PhoneCam media pipeline plan

> Archived from the old worktree on 2026-10-22. Historical text below is
> preserved, not current guidance. `../plan.md` supersedes its status,
> worktree instructions and end-to-end latency attribution.
> In particular, the historical **652.5 ms optical claim and ~550 ms consumer
> attribution below are withdrawn**, not validated current results.

Status: clean B0 baseline and first end-to-end reading recorded; stopped before E1<br>
Worktree: `D:\work\phonecam-media-pipeline`<br>
Branch: `media-pipeline`<br>
Base: `master` at `2d9ee9e` (this repository has no `main` branch)<br>
Date: 2026-10-20<br>
Last updated: 2026-10-20 15:45 local

## 0. Where we are now

### Done

- Created the clean worktree and branch from `master` at `2d9ee9e`.
- Built the instrumented `phonecam-host` Debug binary and installed the matching Android debug APK.
- Added development-only timing instrumentation:
  - `BenchTelemetry.kt`, Camera2 sensor/AE logs, MediaCodec timing logs;
  - host receive/decode/ring timing logs.
- Added the repeatable harness:
  - `bench/analyze.py` — parses phone and host logs into distributions;
  - `bench/run-b0.ps1` — installs/starts the app, runs the host for 30 s, captures logs, and analyzes;
  - `bench/results.md` — recorded baseline results;
  - `bench/timer-window.ps1` and `bench/end-to-end.ps1` — live timer plus virtual-camera preview and screenshot capture.
- Captured three clean 30 s B0 runs:
  - `20261020-152812-B0-clean`
  - `20261020-153046-B0-r2`
  - `20261020-153131-B0-r3`
- Confirmed the virtual camera and ffplay preview path; the timer window layout has been tested.

### Measured B0 baseline (segment timings, not glass-to-glass)

- ~30.33 fps in all three runs; sensor, encoder, and host frames matched 1:1; no pairing misses.
- Sensor → encoder output: p50 ~96.0 ms, p95 ~100–102 ms, p99 ~104–107 ms.
- Host decode: p50 ~6.1–6.3 ms, p95 ~8.2–8.9 ms.
- Ring write: p50 ~0.18–0.21 ms; host receive → ring: p50 ~6.3–6.5 ms.
- Keyframes ~0.99 s apart; encoded frames p50 ~23.8 KB, keyframes ~75 KB.

Full numbers and caveats: `bench/results.md`.

### End-to-end result (recorded)

- Method: `bench/end-to-end-frames.ps1`, run `bench/e2e/frames-20261020-160050`.
- The phone's rear camera pointed at the live millisecond clock on the left half
  of the primary monitor; the virtual camera was captured with ffmpeg dshow, and
  each captured frame was compared with the clock visible inside that frame.
- **Clock to captured frame: p50 652.5 ms, min 635 ms, max 683 ms, stdev 15 ms**
  over 10 legible samples.
- Only ~102 ms of that is the phone + host pipeline. The other ~550 ms is in the
  virtual camera -> DirectShow -> consumer path.
- Caveat: ffmpeg reported the DirectShow buffer full 2056 times during the
  capture, so this is an upper bound for that consumer path.
- Details: `bench/results.md`.

### Remaining gap

The end-to-end number cannot be split further from outside the product, because
the virtual camera stamps samples with `MFGetSystemTime()` at delivery
(`windows/vcam/MediaStream.cpp:261`) and does not carry the original capture
time into the consumer.

### Next step (E1 not started)

E1 (encoder capability inventory) has **not** been started, by instruction. The
next action is attribution of the ~550 ms that lives after the host ring:
compare the host ring write timestamp with the virtual camera's delivery
timestamp. That is a host/vcam instrumentation task, not a pipeline change, and
it decides whether latency work belongs in the phone pipeline at all.

### Current caveats

- The ffplay preview path was abandoned: it drifted by 1.5-1.9 s and later lost
  the host. The end-to-end number now comes from timestamped ffmpeg frame grabs.
- A single fixed consumer must be chosen before any comparison, per section 3.
  ffmpeg dshow capture is that consumer for now; its own buffering cost is
  still an open caveat.
- The worktree intentionally contains uncommitted instrumentation and harness
  files. Do not measure from `D:\work\phonecam`.
- No encoder, keyframe, queue, decoder, or camera tuning has started yet.

### Immediate next step

Stopped here by instruction. The end-to-end baseline is recorded; the next
action is the attribution task described above, and E1/E2 remain gated on it.

## 1. Scope

This plan covers PhoneCam's **media pipeline only**:

- camera capture and frame-rate behaviour;
- H.264 encoding and bitstream structure;
- packet/frame handoff and queueing inside the pipeline;
- host-side decoding and handoff to the virtual camera;
- measurement of latency, frame continuity, CPU cost, and recovery behaviour.

It deliberately does **not** cover connection selection or connection work: no hotspot, Wi-Fi, Wi-Fi Direct, USB transport changes, discovery, or network-topology experiments. Those ideas are recorded at the end only so they are not lost.

The goal is not to make the pipeline look better in a preview. The goal is to make the path that feeds the Windows virtual camera faster and more predictable, and to prove each change with measurements.

## 2. Current baseline

The current product path is:

```text
Camera2 sensor
  -> Camera2 output Surface (zero-copy)
  -> Android MediaCodec H.264 encoder
  -> framed video packets
  -> host transport
  -> Windows Media Foundation H.264 decoder (CPU, NV12 output)
  -> SharedFrameRing (3 slots, latest-frame read)
  -> virtual camera frame generation
  -> consumer application
```

Current defaults and relevant implementation details:

- Capture target: 1920x1080, 30 fps, 6 Mbps by default.
- `CameraCapture.kt` sets `CONTROL_AE_TARGET_FPS_RANGE` to `[fps, fps]`, so the intended behaviour is already a fixed AE target range rather than an open-ended range. This still needs to be verified on the device in low light.
- `H264Encoder.kt` configures MediaCodec with a 1-second I-frame interval and requests CBR. Optional encoder settings are intentionally avoided because this phone/vendor encoder has rejected some optional keys in the past.
- The Windows decoder is the Media Foundation software H.264 decoder. It attempts to enable low-latency mode with `CODECAPI_AVLowLatencyMode`, but this is best effort.
- `SharedFrameRing` has three slots and exposes the newest complete frame. The virtual camera reads that latest frame rather than replaying a backlog.
- Development-only timing instrumentation and a repeatable harness now exist on this branch. The clean B0 baseline has been recorded; see section 2.1 and `bench/results.md`.

Important baseline caveat: the original checkout at `D:\work\phonecam` contains uncommitted transport/RTP-related work that is not present on this branch. Measurements for this plan must use this clean worktree and must record the exact commit and working-tree state. Do not combine results from the two trees.

### 2.1 Measured B0 baseline

Three consecutive 30 s runs on the product path, with instrumentation enabled:

| Metric | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| Sensor FPS | 30.329 | 30.326 | 30.326 |
| Encoder FPS | 30.342 | 30.330 | 30.329 |
| Host FPS | 30.340 | 30.330 | 30.326 |
| Pairing misses | 0 | 0 | 0 |
| Sensor → encoder output p50 | 96.02 ms | 96.04 ms | 96.05 ms |
| Sensor → encoder output p95 | 101.58 ms | 100.95 ms | 100.01 ms |
| Host decode p50 | 6.09 ms | 6.29 ms | 6.21 ms |
| Host decode p95 | 8.20 ms | 8.82 ms | 8.90 ms |
| Ring write p50 | 0.18 ms | 0.21 ms | 0.18 ms |
| Receive → ring p50 | 6.26 ms | 6.50 ms | 6.39 ms |

The dominant measured segment is phone-side sensor → encoder output at ~96 ms
p50. Host decode adds ~6 ms and the ring write is ~0.2 ms. These are segmented
numbers, not a glass-to-glass latency. The external timer measurement is still
pending.

The camera was pointed at a dark/covered scene for these runs. Frame pacing and
bitrate are valid for that scene, but scene-dependent encoder behaviour must be
confirmed with the end-to-end reference scene.

## 3. Measurement principles

1. **Measure the real product output.** Use the registered Windows virtual camera and a real consumer, not the phone preview or a developer-only decode window.
2. **Separate the signal path from the transport.** The existing USB delivery path is the stable control path for isolating media-pipeline changes. Connection experiments are out of scope.
3. **Report distributions, not just averages.** Record p50, p95, p99, maximum, and frame-to-frame jitter.
4. **One variable at a time.** Change one encoder, camera, queue, or decoder parameter per experimental build.
5. **Repeat runs.** Use at least three runs per configuration and estimate run-to-run noise before declaring a win.
6. **Record the environment.** Phone model/build, Android version, app commit, encoder name/profile/level, host OS, GPU/driver, virtual-camera build, consumer application/version, resolution, frame rate, bitrate, lighting, battery/thermal state, and background load.
7. **Do not mix consumers.** ffmpeg, OBS, Chrome, and a native Media Foundation consumer can add different buffering. Pick one primary consumer for comparisons and keep it fixed.
8. **No published latency claims until the harness is proven.** Do not assume that a low-latency codec key, a keyframe interval, or a decoder flag produces a measurable improvement.

### 3.1 End-to-end latency measurement

Use two complementary measurements:

**A. External reference timer (authoritative end-to-end check)**

- Display a high-contrast timer with millisecond resolution on a monitor.
- Point the phone at that timer.
- Capture the virtual-camera output with the fixed consumer.
- For each captured output frame, compare the timer value visible in the frame with the wall-clock time at which that output frame was captured.
- Record the distribution over at least 60 seconds.
- Keep monitor refresh rate, camera distance, focus, exposure, and consumer buffering fixed.

Status: done. `bench/timer-window.ps1` plus `bench/end-to-end-frames.ps1`
capture timestamped frames from the virtual camera while the phone watches the
live clock. Result: 652.5 ms p50 (±15 ms) clock-to-captured-frame, of which
~102 ms is the phone + host pipeline. The ffplay preview variant was rejected
because it drifted by 1.5-1.9 s. See `bench/results.md`.

This is a practical glass-to-glass proxy. It includes camera exposure, encoding, host decode, virtual-camera delivery, monitor scanout, and the capture method. It is not a pure internal pipeline number, so report it as an end-to-end visual measurement with its method, not as a universal product latency.

**B. Segmented internal timing (for attribution)**

Use development-only instrumentation to measure each segment independently:

- sensor timestamp -> encoder input timestamp;
- encoder input -> encoded output packet;
- encoder output -> host receipt;
- host receipt -> decoded frame;
- decoded frame -> ring write;
- ring write -> virtual-camera delivery/sample request.

Per-side durations are safe to compare directly. Cross-device absolute clocks (phone monotonic time vs. Windows `QPC`) must not be subtracted without a documented offset calibration. If a cross-device offset is needed, estimate it through the control channel and record the uncertainty.

## 4. Instrumentation to add before changing the pipeline

All instrumentation should be development-only, disabled by default in release builds, and cheap enough not to distort the result.

### 4.1 Android side

- Add a `CameraCaptureSession.CaptureCallback` in `CameraCapture.kt` to record:
  - `SENSOR_TIMESTAMP`;
  - requested and actual AE target FPS range from `CaptureResult`;
  - `SENSOR_FRAME_DURATION`/frame interval where available;
  - dropped-frame indicators where available.
- In `H264Encoder.kt`, record per-output-buffer:
  - input/output presentation timestamps;
  - keyframe flag and encoded size;
  - time spent waiting for a free output buffer and time spent draining/writing;
  - queue depth or an equivalent backpressure signal.
- Dump the selected codec and output format once per session:
  - codec name, profile, level, color format;
  - actual frame rate and bitrate settings;
  - whether optional keys were accepted or rejected.
- Emit structured logs (JSONL or CSV) with a run/session id. Keep logging optional so the baseline and instrumented builds can be compared.

### 4.2 Windows side

- Timestamp packet/frame receipt and decoder input submission.
- Timestamp decoded output and the corresponding ring write.
- Measure decoder call duration and track CPU usage.
- Record whether a delivered frame is new or a repeat/latest-frame reuse.
- Where practical, timestamp virtual-camera frame requests and delivery. If instrumenting the Frame Server is too invasive, use the external timer and host-side ring timestamps instead.

### 4.3 Benchmark harness

- A fixed capture command or small native consumer that records frames with reliable timestamps.
- A run manifest containing the commit, build type, device, host, settings, and artifact hashes.
- A results table or JSONL output that can be summarized into p50/p95/p99/max.
- A manual or scripted procedure for the external-timer test.
- A clean procedure for returning the phone and host to the same state between runs.

## 5. Baseline test matrix

Run the unmodified current pipeline first. Do not tune anything until this baseline exists.

| ID | Configuration | Purpose |
|---|---|---|
| B0 | 1080p30, 6 Mbps, default encoder settings | Product default baseline |
| B1 | 720p30, default settings | Lower-cost baseline |
| B2 | 1080p30, 4/6/8/12 Mbps | Bitrate sensitivity, keyframe burst behaviour |
| B3 | 1080p30, low-light scene | Verify fixed AE FPS and measure exposure-related frame loss |
| B4 | 1080p30, normal-light scene | Reference for B3 |
| B5 | 10-minute steady run | Thermal drift, frame-interval drift, encoder stability |
| B6 | Forced keyframe / decoder restart | Recovery time to a clean picture |

For each row, capture:

- end-to-end visual latency;
- segmented internal timings;
- delivered FPS and frame-interval jitter;
- dropped/duplicated frames;
- keyframe sizes and intervals;
- bitrate over time;
- encoder queueing/backpressure;
- host decode time and CPU;
- thermal state and any phone throttling;
- visual artifacts, colour/rotation/orientation behaviour, and camera compatibility.

If the virtual camera does not advertise a requested combination (for example 60 fps), record that as a capability limit rather than forcing it. Do not add a 60 fps experiment to the main matrix until the camera, encoder, ring, and virtual-camera media types are confirmed to support it.

## 6. Experiments in execution order

### E1. Encoder capability inventory

Before changing encoder behaviour, enumerate what the device actually supports:

- encoder names and supported profiles/levels;
- supported sizes, frame rates, bitrate range, and colour formats;
- intra-refresh support;
- slice-related support, if exposed by the vendor encoder;
- acceptance of optional keys such as `KEY_LOW_LATENCY`, `KEY_LATENCY`, `KEY_PRIORITY`, `KEY_MAX_B_FRAMES`, and an intra-refresh period.

Test optional keys **one at a time**, because this encoder has rejected bundles of optional settings before. A failed `configure()` or `start()` is a result, not something to work around silently. Record the exact exception/code.

### E2. Verify fixed AE frame rate in low light

`CameraCapture.kt` already requests `[fps, fps]`, so the first task is verification, not a code change:

- Confirm the repeating request actually contains the fixed range.
- Record `CaptureResult` values in bright and dim scenes.
- Compare requested sensor frame duration with observed encoded frames.
- Check for long exposures and dropped frames that make the stream look slower even when the encoder is configured for 30 fps.

Only change camera behaviour if the measurements show the range is not being applied or is being overridden.

### E3. Keyframe interval and intra-refresh

Current baseline: one IDR approximately every second.

Test:

- 1-second I-frame interval (baseline);
- 2-second interval;
- 5-second interval;
- intra-refresh at a comparable refresh period, if supported;
- intra-refresh with an initial IDR and a documented recovery/keyframe-request path.

Measure:

- average bitrate and keyframe burst size;
- end-to-end latency during and after refresh;
- recovery time after a forced decoder reset or lost access unit;
- visual smearing/artifacts during the refresh wave;
- behaviour when a consumer starts mid-stream.

Intra-refresh is only acceptable if recovery and join behaviour remain predictable. It must not be adopted solely because it lowers the average bitrate.

### E4. Low-latency encoder and decoder options

Test supported optional settings individually:

- encoder low-latency/latency hints;
- encoder priority;
- decoder low-latency mode (already attempted on the MF decoder);
- any vendor-supported latency modes exposed through the codec.

For each, record whether configuration succeeds, whether the bitstream remains compatible, and whether measured latency or frame pacing changes. Do not make any of these a required architectural assumption; availability is device- and API-dependent.

### E5. Bitrate, resolution, and frame-rate sweep

Use the baseline matrix to answer:

- Does 720p materially reduce end-to-end latency compared with 1080p?
- At what bitrate does 1080p30 remain stable on the fixed host/consumer setup?
- Does lowering bitrate reduce latency, or only reduce quality?
- Does the encoder produce fewer/smaller keyframe spikes at a different bitrate?
- Does anything change in low light when the AE frame rate is fixed?

Keep the virtual camera's advertised modes in mind: a source resolution that does not match a supported virtual-camera mode may add conversion or scaling outside the encoder.

### E6. Pipeline queueing and backpressure

Measure before changing policy:

- how long the encoder waits for an output buffer;
- how long the encoder's drain/write path blocks;
- whether host decode or the shared ring falls behind;
- whether the ring correctly serves the latest frame under slow consumption;
- how latency behaves when the consumer is deliberately slowed.

Then, if needed, test a bounded **pipeline-side** queue policy that drops stale frames instead of building latency. This experiment is limited to encoder/decoder/ring handoff. Socket and transport queue design is outside this plan.

### E7. Decoder cost and GPU zero-copy investigation

Measure the existing software MF decoder first:

- decode time per frame;
- CPU usage at 720p30 and 1080p30;
- memory copies between decode output, ring, and virtual-camera rendering;
- effect of decode cost on frame pacing.

Only if decode/copy is shown to be a meaningful part of end-to-end latency should GPU/D3D11 zero-copy decoding be investigated. That would change the current CPU-NV12 shared-memory contract and the virtual-camera frame path, so it is a separate project, not a quick tuning change.

### E8. Slices and packetization

Check whether the vendor encoder exposes slice output and whether it can be used without changing the decoder contract.

Slice encoding is only useful if the output can be sent before a complete frame/access unit is assembled. It also complicates packetization, marker bits, and recovery semantics. Do not add it to the default path unless the capability inventory and end-to-end measurements justify it.

### E9. Keyframe recovery and join behaviour

Using the existing control channel's keyframe-request/reconnect path:

- force a decoder reset and measure time to the first clean frame;
- force a missing access unit and measure recovery;
- start a consumer mid-stream and measure time to a usable image;
- compare periodic IDR, longer IDR intervals, and intra-refresh recovery.

This is a media-recovery experiment, not a connection-selection experiment.

### E10. SPS/PPS repetition policy

Evaluate whether repeated SPS/PPS is needed only after measuring mid-stream join and recovery failures. The control channel already provides a session handshake and keyframe-request mechanism, so unconditional per-second repetition is not a baseline requirement.

## 7. Decision rules

A pipeline change is kept only when all of the following hold:

- It improves p95 end-to-end latency beyond the measured run-to-run noise, or fixes a demonstrated reliability problem.
- It does not make p99 latency or frame continuity materially worse.
- It does not regress virtual-camera compatibility, orientation/rotation, resolution negotiation, or the existing control path.
- It survives a 10-minute steady run without thermal or queueing degradation.
- Its configuration is supported by the actual device encoder, not merely documented by Android.
- It can be reverted cleanly and the result is reproducible from the recorded manifest.

If a result is within the noise floor, record it as inconclusive rather than adopting the change.

## 8. Ready-to-measure checklist

Before the first baseline run:

- [x] Use `D:\work\phonecam-media-pipeline` only; recorded commit `2d9ee9e` plus the uncommitted instrumentation/harness state.
- [x] Build and deploy the instrumented host from this worktree; confirm the installed virtual-camera DLL is the shared copy in `C:\ProgramData\PhoneCam`.
- [x] Install the matching Android build on the test phone.
- [x] Confirm the virtual camera is selectable in DirectShow/ffplay.
- [x] Fix the measurement consumer: ffmpeg dshow frame capture (the ffplay preview was rejected, it drifted 1.5-1.9 s). The consumer's own buffering cost is still an open caveat.
- [x] Confirm the test scene and timer alignment (phone pointed at the clock on the primary monitor). Monitor refresh rate is still unrecorded.
- [ ] Set the phone to a repeatable power/thermal state.
- [x] Verify the external-timer measurement procedure on one frame with a readable delayed timer (`bench/e2e/frames-20261020-160050`, 10 legible samples).
- [x] Add the development-only timing logs.
- [x] Run B0 at least three times and establish the noise floor.
- [x] Archive logs and the run manifest for the three B0 runs.

Current position: B0 and the end-to-end reading are both recorded. The open
question is where the ~550 ms after the host ring lives. Do not start E1/E2
until that is attributed.

The first deliverable after this plan is a recorded baseline, not a new encoder configuration.

## 9. Not worth adopting now

These items were considered and are deliberately deferred or rejected. They are kept here so the reasoning is not lost.

### 9.1 Raw video or MJPEG

Raw frames avoid inter-frame dependency but require far more bandwidth than the existing H.264 path. MJPEG has poor compression efficiency and no advantage for the current product path. Neither is a sensible default for a live camera stream.

### 9.2 GPU zero-copy decoding as an immediate change

The current decoder produces CPU NV12 frames that fit the existing shared-memory and virtual-camera contract. Replacing that with D3D11/GPU zero-copy would be a larger architectural change and must wait for evidence that decode/copy cost is significant. Measure the software decoder first.

### 9.3 Relying on `KEY_LOW_LATENCY` as an architectural foundation

Availability and behaviour are device- and API-dependent. It may be worth testing if the encoder accepts it, but the pipeline must not depend on it. A missing key must not break capture or increase latency unexpectedly.

### 9.4 Resending SPS/PPS every second by default

The control channel already has a handshake and keyframe-request path. Repeated parameter sets add bytes and do not fix the underlying recovery policy. Evaluate this only if measurements show a real mid-stream join or recovery problem.

### 9.5 HEVC as a first-step improvement

HEVC adds compatibility, decoder, and interop questions without a demonstrated need in the current 1080p30 path. Do not change codecs before the H.264 baseline is understood.

### 9.6 Slice encoding now

The vendor encoder's slice support is unverified, and slice output only matters if the transport can act on it before complete-frame assembly. It is not a default-path change until both sides are proven.

### 9.7 WebRTC, SRT, QUIC, or TCP as media transports

These are transport/connection projects, not media-pipeline experiments. They are outside this worktree's scope. Reliable/retransmitting transports can also conflict with a low-latency live-video policy unless queue and drop behaviour are designed carefully.

### 9.8 Wi-Fi, hotspot, and Wi-Fi Direct comparisons

Connection behaviour is explicitly out of scope. This plan does not compare network topologies, radio settings, or discovery mechanisms.

### 9.9 Immediate vsync or "remove the jitter buffer" changes

The virtual camera runs through Windows Media Foundation and the consuming application, so frame scheduling is not fully controlled by PhoneCam. Measure the actual delivery timeline before adding scheduling assumptions or changing the capture path.

### 9.10 Audio

Audio is not part of the current virtual-camera video path and is not included in this measurement plan. It should be treated as a separate feature with its own latency and synchronization requirements.

### 9.11 60 fps as an assumed baseline

Do not assume the camera, encoder, shared ring, and virtual-camera media types support a useful 60 fps mode. Treat 60 fps as a capability-gated follow-up experiment only after 30 fps is measured and the supported modes are known.

## 10. Immediate next step

The measurement harness and the clean B0 baseline are done. The immediate next
step is the external reference-timer capture:

1. Point the phone's rear camera at the left half of the primary monitor (the
   full-height live timer).
2. Run `.\bench\end-to-end.ps1 -WarmupSeconds 8 -Shots 5 -ShotIntervalMs 700`.
3. Read the live timer and the delayed timer in the preview from the same
   screenshot; record the delta and the consumer caveats.
4. Add the result to `bench/results.md` and the run record.

No encoder, keyframe, queue, or decoder change should be made before that
end-to-end reading exists.
