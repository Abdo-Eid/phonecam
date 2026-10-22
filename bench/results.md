# PhoneCam media-pipeline measurements

## Final status — hotspot work stopped, USB retained (2026-10-22)

- The user reported that clean-IDR recovery removed pixelation, but froze/lagged.
- Bounded reordering was then reported **better, but still lagging**. The user
  stopped further effort and chose USB as the dependable path. Wireless
  smoothness is **not accepted**; no exact radio/driver root cause was established.
- The PC retains clean-IDR recovery and a gap-only 12 ms / 64-packet reorder
  policy. No encoder, resolution, bitrate, USB-path or phone-APK change was made
  for these Windows-only fixes. Synthetic picture-hash success is not a live
  smoothness or end-to-end latency claim.
- Final offline regression checks passed: Debug/Release bounded reordering,
  17 receiver cases, existing video timing/media settings, 28 Python tests and
  benchmark PowerShell parsing. The previously interrupted final Android
  validation is **not** claimed passed.
- Full story, online sources, A/B/offline evidence, unsuccessful adapter trial,
  deployment hashes/backups, final logs and limits: `../networking.md`, especially
  sections 20–22. Original unfinished media acceptance gaps remain in `../plan.md`.
- Raw logs, generated media, private device/network identifiers and room/user
  images remain local in ignored artifact directories. No further live test is active.

## E1 encoder capability inventory — 2026-10-22

Real-device instrumentation: `EncoderInventoryTest.inventoryAndProbeIndividualKeys`,
one passing test, Redmi Note 8 / Android 11 (API 30). No camera frames are fed in
this test. Report: `bench/runs/20261022-122914-E1-inventory/encoder-inventory.json`.
Default selected encoder: hardware `OMX.qcom.video.encoder.avc`.

| Isolated request (1080p30, requested 6 Mbps CBR) | Configure/start result |
|---|---|
| Unchanged default | Accepted |
| `low-latency=1` | Rejected at configure: `IllegalArgumentException` |
| `latency=0` | Accepted |
| `priority=0` | Accepted |
| `max-bframes=0` | Accepted |
| `intra-refresh-period=30` | Accepted, but feature NOT advertised |
| 2-second IDR interval | Accepted |
| 5-second IDR interval | Accepted |
| Baseline profile alone | Rejected at configure: codec error `0x80001001` |
| Constrained Baseline profile alone | Rejected at configure: codec error `0x80001001` |

The codec advertises 1080p30/720p30, widths 96–4096, heights 96–2160 and bitrate
range 1–100,000,000 bps. Broad independent ranges are not a guarantee that every
size/rate combination works. It reports **CBR unsupported** despite accepting
the existing CBR request; intra-refresh similarly is not advertised. The output
format omits those effective settings, so no honoring/quality/latency claim is
made. Software encoders are enumerated but not substituted into the product.

## E3/E4 real-stream sweep — completed

Cohort `bench/runs/sweep-20261022-131644`: 21 successful runs, three interleaved
rounds, 30 seconds requested per case, first eight seconds of per-side telemetry
excluded. Identical host/APK/vcam hashes across all runs; comparison rejects
mixed artifacts. All runs used USB/ADB, 1080p30 and requested 6 Mbps.

| Case | Median run p95 sensor-to-encoded | Delta vs default | Outcome |
|---|---:|---:|---|
| Default | 108.895 ms | — | Retained |
| Latency=0 | 109.061 ms | +0.166 ms | No demonstrated latency benefit |
| Priority=0 | 108.707 ms | -0.188 ms | No demonstrated latency benefit |
| Max B-frames=0 | 109.156 ms | +0.261 ms | No demonstrated latency benefit |
| Intra-refresh period=30 | 108.607 ms | -0.288 ms | Activity not established; no demonstrated benefit |
| IDR interval=2 s | 108.889 ms | -0.006 ms | Interval honored; not a latency win |
| IDR interval=5 s | 109.163 ms | +0.268 ms | Interval honored; not a latency win |

Default p95 varied from 108.507 to 108.898 ms (~0.391 ms range), and its p50
varied by ~2.69 ms. All candidate median p95 differences above are smaller than
that observed default p95 range. Three repeats do not establish a statistical
noise floor for every environment, but they give no basis to adopt these hints.
Phone clock mapping also uses millisecond-quantized uptime samples; sub-ms
differences must not be treated as precision improvements.

Default IDRs were ~0.99 s apart; requested 2/5 s became ~1.98/4.95 s. Average
encoded bitrate remained ~5.99–6.00 Mbps for every case. In this mostly static
scene, longer IDRs reduced keyframe burst sizes but not average bandwidth or
measured phone latency; recovery/quality effects remain unmeasured. The
intra-refresh request retained the same ~0.99 s IDRs and ~75 KB keyframes; the
test does not prove intra-refresh was active. It is not adopted.

Host rates stayed ~30.30 fps. No encoder sequence resets were recorded. A single
sensor/encoder pairing miss occurred in one run each of intra-refresh, priority
and latency; boundary trimming can contribute, and this does not prove a dropped
video frame. Neither counts nor sample timestamps alone establish image continuity.

**Decision: no product defaults changed. No end-to-end latency win claimed.**
Raw run order, per-run summaries and `comparison.json` are preserved locally.

## E5 resolution/bitrate sweep — completed measurements

Cohort `bench/runs/profiles-20261022-144900`: 15 passing runs, three interleaved
repeats per profile, eight-second warmup trim and identical artifacts. Host hash
`7ACE37DC53CABB05C36BE99D247055B7BFAB2045D92F1DDCC956B56919BF2BF9`;
APK `77E5D30C63E3AA8387A9B0BC76C542296A4986500C7DDDC8699D9E42F78440D5`.
Compare within this cohort, not against the earlier encoder-sweep host/environment.
Only debug media request dimensions/bitrate changed; USB/ADB remained fixed.

| Profile | Median run phone p50 | Median run phone p95 | p95 delta vs control |
|---|---:|---:|---:|
| 1080p / 6 Mbps (default) | 89.808 ms | 99.025 ms | — |
| 720p / 6 Mbps | 83.927 ms | 94.518 ms | -4.507 ms |
| 1080p / 4 Mbps | 90.067 ms | 99.046 ms | +0.021 ms |
| 1080p / 8 Mbps | 90.301 ms | 99.208 ms | +0.183 ms |
| 1080p / 12 Mbps | 89.806 ms | 98.523 ms | -0.502 ms |

Phone metric is sensor-to-encoder output, **not end-to-end latency**. Default
p95 ranged 97.816–99.298 ms. Every run had zero pairing misses/encoder sequence
resets and host rate ~30.3 fps; these are not image-continuity proofs. Measured
bitrate followed the requested 4/6/8/12 Mbps targets. 720p reduced phone segment
and host decode cost, but lowers source detail. In this scene, bitrate alone
provided no convincing phone latency gain. Controlled quality/motion/lighting
and optical tests remain; **retain 1080p/6 Mbps defaults**.

## B5 ten-minute native-consumer stability — completed

Run `bench/runs/20261022-150039-B5-default-native-consumer`, same host/APK/vcam
as E5, USB/ADB, defaults. Phone/encoder/host ~30.303 fps, zero pairing misses.
After eight-second telemetry warmup, phone p50/p95 89.202/98.112 ms; host decode
p50/p95 11.113/16.913 ms; ring-write p50/p95 0.215/0.406 ms. These are separate
segments, not calibrated cross-device or optical latency.

Native MF consumer received 17,999 samples at 30.002 fps with zero duplicate or
backward sample times, one empty read, and no image copies/saves. Rational sample
intervals were 33.3333/33.3334 ms, but receipt interval p95/p99/max was
47.281/49.528/82.084 ms. Timestamp continuity does not prove unique pictures or
perfectly regular delivery. Do not merge these results with old DirectShow captures.

`steady-trend.json` minute bins show phone ~30.301–30.307 fps and no accumulating
phone-side PTS age (p95 96.785–98.416 ms; quantized uptime minus PTS, not a new
calibration). Host decode minute p95 ranged 13.776–24.167 ms. Reported thermal
stayed 0 and bitrate 6000 kbps; no thermal-driven adaptation was observed, not a
guarantee about other scenes/devices or unreported physical temperatures.

## E9 synthetic recovery and live command response

Offline run `bench/runs/20261022-151849-E9-synthetic-recovery` replays a six-second
1080p30 baseline H.264 corpus through the actual host `MFH264Decoder`. A complete
decode provides reference NV12 hashes; frame 45 is omitted in every fault case.
One-second IDRs and cached/repeated SPS/PPS are deliberate test inputs.

| Scenario | Sustained reference match | Media time after frame 45 |
|---|---:|---:|
| One missing AU, initial headers only, no reset | Frame 60 | 500 ms |
| Reset, initial headers only | No recovery in corpus | — |
| Reset with cached headers | Frame 60 | 500 ms |
| Reset with repeated headers | Frame 61 | 533.333 ms |

This is deterministic synthetic replay, not live loss, a wall-clock recovery
benchmark, or proof of a defect in current USB/RTP startup. Existing RTP header
repetition and decoder reset contracts remain unchanged; USB header policy is
not altered without live evidence.

Live run `bench/runs/20261022-153322-E9-keyframe-response` uses the existing console
command with five-second periodic IDRs: **10/10 requests acknowledged Ok** and
followed by a decoded keyframe at the host ring within one second. Same-PC QPC
issue-to-ring p50 64.337 ms, p95/max 78.873 ms, minimum 46.989 ms. PTS joins frame
identity only; clocks are not subtracted across devices. A subsequent keyframe
is not proof of request causality in every case or clean-image recovery after
corruption. Locked-phone attempt `20261022-152349-E9-keyframe-response` failed
startup and is excluded. No connection behavior was changed.

## E6 slow-consumer/backpressure — completed bounded-age check

Cohort `bench/runs/20261022-154733-E6-consumer-stress`: six 15-second native
MF captures, alternating order over three rounds, one continuous default USB/ADB
camera session. No image saves/copies; identical consumer/host/vcam artifacts
recorded per capture/cohort. Added `MFGetSystemTime()` immediately after receipt
to subtract the MF sample timestamp in the same clock domain, not phone time.

| Read delay | Samples/run | Receipt fps range | Sample-age p50 range | Sample-age maximum |
|---|---:|---:|---:|---:|
| 0 ms | 450–451 | 29.977–29.995 | 18.589–18.851 ms | 29.318 ms |
| 100 ms | 139 | 9.167–9.182 | 165.909–167.501 ms | 183.526 ms |

Normal first/last-third age medians were ~18–19 ms. Slow first-third medians
165.359–168.093 ms and last-third medians 165.627–166.964 ms show no accumulating
age during these short captures. Slow sample timestamp intervals had median
100 ms and p95/max 133.3334 ms (forward skips), no duplicate/backward times.
Subsequent fresh normal consumers returned to ~30 fps/~19 ms age. We did not
switch a single reader back to fast draining, so continuous-reader recovery is
not established. Retest longer stalls/other consumers before generalizing.

Across the session, host receipt/decode telemetry remained 30.301 fps, decode
p50/p95 6.047/8.166 ms. Product stats include startup ramp (17→30.2 fps), reported
drop values 13/1/0, thermal 0; do not call this a zero-drop run or interpret those
stats as consumer frame drops. Timing does not establish unique image content.

**Interpretation:** the fixed native reader adds a bounded delivery/queueing
penalty when intentionally slow; no evidence here warrants a pipeline queue
redesign. Scheduled-sample age is NOT sensor/image age or optical E2E latency.
Raw captures are linked by `run-order.csv`; `comparison.json` retains summaries.

## Build-isolation follow-up

After all above measurements, disabled Android telemetry was changed to avoid
registering its per-frame capture callback, and Windows telemetry was gated out
of Release (media profiles were already Debug-only). No product tuning defaults
changed. Measured APK/host archived at
`bench/logs/pre-final-validation-20261022-155113/`; later rebuilt artifacts are
not substituted into the measured cohorts. Final validation status: `../plan.md`.

## Closing short optical observation — precision limited

Session `bench/e2e/optical-final-20261022-161636`, capture
`bench/e2e/receipt-20261022-161642-621`: ten seconds, 300 native MF samples,
20 saved images, 30.029 receipt fps. User requested a quick finish; no further
handheld capture was requested. Camera, owned host and timer are stopped.

Fixed a benchmark-only BMP export defect: positive media stride is top-down,
requiring negative BMP height. The old header vertically inverted saved images;
this was not caused by holding the phone in portrait. New -1080-height BMP was
checked visually upright. Existing artifacts are preserved; receipt timings are
not derived from image orientation or file-write times.

Timer digits visibly overlap refreshes. Rather than guess exact milliseconds,
`timer-reading-bounds.csv` records stable leading digits with 100-ms-wide intervals
for each of 20 images. `visual-age-bounds.json` gives an overall interval envelope
of **97–212 ms** for timer-to-native-receipt age. This is NOT an exact median,
p95, controlled 60-second baseline, universal product latency, or measured win.
It includes display refresh/scanout, exposure, consumer delay and periodic image
copies. Handheld scene and incompletely verified background-build cancellation
prevent treating it as a controlled performance cohort. Defaults remain unchanged.

Final Android Debug/Release validation was interrupted after a stalled dependency
download; no successful final-build claim. Windows media builds/tests passed
before the BMP correction; the corrected Debug consumer built and captured,
but its final Release rebuild was interrupted. Nine offline tests passed after
the correction. Remaining acceptance gaps and deferred experiments: `../plan.md`.

## Current-base B0 — 2026-10-22

Base `387f8b9` plus opt-in migrated instrumentation, matching Debug phone/host,
Redmi Note 8 / Android 11, USB/ADB forced, 1080p30 at 6 Mbps. The phone was
connected to the laptop hotspot too, but this is NOT a Wi-Fi performance run.

| Metric | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| Host fps | 30.329 | 30.293 | 30.302 |
| Sensor / encoder / host counts | 1170 / 1169 / 1170 | 1078 / 1077 / 1077 | 1077 / 1076 / 1077 |
| Sensor/encoder pairing misses | 0 | 0 | 0 |
| Sensor-to-encoder output p50 | 99.44 ms | 98.29 ms | 98.48 ms |
| Sensor-to-encoder output p95 | 109.22 ms | 108.24 ms | 108.21 ms |
| Host decode p50 | 6.21 ms | 5.89 ms | 6.14 ms |
| Host decode p95 | 10.80 ms | 8.21 ms | 9.13 ms |
| Host post-decode handoff p50 | 0.193 ms | 0.162 ms | 0.205 ms |
| Host receive-to-ring p50 | 6.40 ms | 6.06 ms | 6.35 ms |

Runs in `bench/runs/`:

- `20261022-121541-B0-main-r1-resumed`
- `20261022-121641-B0-main-r2`
- `20261022-121727-B0-main-r3`

Each captured at least the requested 30 s plus startup, so counts cover unequal
session spans. Boundary counts differ by one; they do not prove frame loss or
perfect continuity. Lighting, framing and background load were not controlled.
These are provisional baseline observations, not a controlled old/new A/B.
Host thermal telemetry reported 0 and steady-state dropped counts 0; this is
not a ten-minute thermal test. No pipeline improvement has been proven.

Each run includes source state, binary/APK/vcam SHA256 hashes and logs. Installed
vcam SHA256: `A88838ABC34C9B02934038225FBBDBD85ED277FE7E24D21D1E81B6442D5D2816`.
The first attempt, `20261022-120707-B0-main-r1`, had no frames because capture was
Stopped; it is excluded. The runner now rejects such runs.

## Native-consumer validation — not an optical latency result

Fixed consumer: native MF synchronous SourceReader, native RGB32 1920x1080@30.
Every sample is drained and receipt is timestamped with QPC/precise UTC before
sample inspection/copying. Selected raw images are written after capture, not
used for their file modification times. No fps resampling filter is involved.

| Smoke test | Samples | Receipt fps | Repeated/backward sample times |
|---|---:|---:|---:|
| Host test pattern, 5 s | 150 | 29.940 | 0 / 0 |
| Live phone over USB, 5 s | 145 | 29.991 | 0 / 0 |

Artifacts: `bench/e2e/native-smoke-20261022-121523` and
`bench/e2e/receipt-20261022-121945-130` (associated host logs in
`bench/e2e/live-consumer-20261022-121936`). Live receipt-interval p95 was 46.92 ms;
sample-time increments were 33.3333/33.3334 ms. Scheduled sample timestamps
and average fps do not imply perfectly regular arrival or unique image content.
The tool still includes its own reader buffering and periodic image-copy cost.
**An optical timer reading is pending. There is no new end-to-end latency claim.**

## Archived measurements

> Historical runs on `2d9ee9e`, migrated to `D:\work\phonecam` on 2026-10-22.
> No new-base comparison or improvement has been proven. The old worktree's
> raw artifacts are retained locally; do not treat them as current-product data.
> During migration, analyzer unit errors were fixed (keyframe seconds and host
> intervals) and FPS now uses interval count `(n-1)/span`, not `n/span`. The
> table below preserves the original report; rerun the analyzer for corrected
> rates. No historical raw logs have been rewritten.

All measurements are from the clean `media-pipeline` worktree, Redmi Note 8
(Android 11) over ADB, 1080p landscape, 30 fps. Phone-side and host-side clocks
are not combined in these numbers; this is a segmented baseline, not
glass-to-glass latency.

## B0 baseline

Three consecutive 30 s runs with instrumentation enabled:

- `20261020-152812-B0-clean`
- `20261020-153046-B0-r2`
- `20261020-153131-B0-r3`

| Metric | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| Sensor FPS | 30.329 | 30.326 | 30.326 |
| Encoder FPS | 30.342 | 30.330 | 30.329 |
| Host FPS | 30.340 | 30.330 | 30.326 |
| Sensor frames | 1021 | 1138 | 1137 |
| Encoded frames | 1020 | 1137 | 1136 |
| Host frames | 1020 | 1138 | 1136 |
| Pairing misses | 0 | 0 | 0 |
| Sensor→encoder output p50 | 96.02 ms | 96.04 ms | 96.05 ms |
| Sensor→encoder output p95 | 101.58 ms | 100.95 ms | 100.01 ms |
| Sensor→encoder output p99 | 106.53 ms | 105.00 ms | 103.93 ms |
| Host decode p50 | 6.09 ms | 6.29 ms | 6.21 ms |
| Host decode p95 | 8.20 ms | 8.82 ms | 8.90 ms |
| Host decode p99 | 10.81 ms | 11.79 ms | 11.49 ms |
| Ring write p50 | 0.18 ms | 0.21 ms | 0.18 ms |
| Receive→ring p50 | 6.26 ms | 6.50 ms | 6.39 ms |
| Keyframe interval | ~0.99 s | ~0.99 s | ~0.99 s |
| Encoded frame p50 | 23.84 KB | 23.81 KB | 23.82 KB |
| Encoded keyframe p99 | ~75 KB | ~75 KB | ~75 KB |

### Observations

- The pipeline is frame-locked at ~30.33 fps: sensor, encoder, and host counts
  match 1:1 in all three runs; no pairing misses.
- Phone-side sensor→encoder output is stable at ~96 ms p50 and ~100–102 ms p95.
  This is the dominant measured segment.
- Host decode is ~6.1–6.3 ms p50; ring write is ~0.2 ms; receive→ring is
  ~6.3–6.5 ms p50.
- Encoder output intervals are jittery (p95 ~37–38 ms, p99 ~40–43 ms) while
  sensor intervals are a steady 33 ms. The average rate stays at 30 fps.
- Keyframes are ~0.99 s apart; keyframe size is ~75 KB versus ~24 KB for
  inter-frames.

### Caveats

- Instrumentation is enabled in both app and host. The first run is 30 s;
  repeats are 30 s plus startup.
- The camera was pointed at a dark/covered scene. Frame timing and bitrate are
  valid for this scene, but scene-dependent encoder behavior should be
  confirmed with the end-to-end reference timer.
- The host used the shared installed virtual-camera DLL
  (`C:\ProgramData\PhoneCam\phonecam-vcam.dll`, last modified 2026-08-19).
- These numbers stop at the host ring buffer. Display/consumer latency is not
  included yet.

## End-to-end reference timer — unvalidated historical attempt

### Method

`bench/end-to-end-frames.ps1`:

- A large millisecond clock is shown on the left half of the primary monitor
  (1920x1080) and the phone's rear camera points at it.
- The virtual camera is captured with
  `ffmpeg -f dshow -i "video=PhoneCam (Windows Virtual Camera)"`,
  8 MB DirectShow buffer, output scaled to 540 px wide at 2 fps.
- For every captured frame, the clock visible **inside** the frame is compared
  with that frame's file write timestamp. This is NOT a frame-receipt timestamp:
  filtering, conversion, encoding and file I/O also contribute to the difference.

Run: `bench/e2e/frames-20261020-160050` — 20 frames captured, 15 contained the
clock, 10 had a fully legible timestamp. Data: `e2e-latency.csv`, contact sheet
`timer-strip-wide.png`.

### Result

| Sample | n | min | p50 | max | mean | stdev |
|---|---:|---:|---:|---:|---:|---:|
| clock -> captured frame written | 10 | 635 ms | **652.5 ms** | 683 ms | 654.1 ms | 15.1 ms |

This is an observed file-write age, not a validated latency baseline. Stable
numbers alone do not validate the method. The `fps=2` filter can introduce
resampling delay; startup/waiting frames and many buffer warnings were present.
Some photographed digits also overlap, so manual readings have uncertainty.

### Interpretation

- The previous attribution of ~550 ms to the virtual camera/consumer is
  withdrawn. Separate-run segment medians cannot be subtracted from this
  confounded reading to isolate a component; transport time was not measured.
- ffmpeg reported buffer-full warnings 2056 times. This does not by itself
  locate the bottleneck or validate a bound for the product's latency.
- Revalidate a fixed consumer with frame-receipt timing on the current base
  before comparing any pipeline changes.

### Other consumer measurements on the same scene

| Consumer | Result | Usable? |
|---|---|---|
| ffplay preview, default timestamps | 1.57 s, drifting to 1.86 s | No — ffplay was not keeping up |
| ffplay preview, `setpts=N/30/TB` + `-sync video` | ~348 ms (1 sample) | Partial — drift fixed, run then lost the host |
| one-shot ffmpeg frame grab | ~240 ms | No — includes the PNG write time |
| timestamped ffmpeg frame burst | 652.5 ms median file-write age | No — fps filter and write-time confounds |

### Why the end-to-end number cannot be split further from the outside

At the old base the virtual camera stamped samples at request handling time.
`387f8b9` now implements a paced sample clock and stamps the final converted
sample too. The installed DLL changed on 2026-10-21. Neither version's sample
time alone proves the age of the image it carries. Cross-device clock domains
must not be subtracted without calibration; host ring-to-delivery attribution
needs same-host timing and frame identity.
