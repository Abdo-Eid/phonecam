# Media measurements

This directory contains opt-in measurement tools, offline analyzers and
[historical results](results.md). No encoder/media default was changed by the
recorded experiments. Main is now USB-only; old RTP/hotspot runs are not evidence
for the current transport or lifecycle behavior.

## Safety and compatibility

- Live runners use the real phone/camera, may install an APK, restart the app,
  launch a host or open a timer. Do not run them implicitly or during another
  camera session. Preserve user Stop intent and existing network/security settings.
- Match host/APK/vcam hashes and keep scenes/workloads controlled. Do not combine
  historical cohorts with newly rebuilt binaries.
- Some legacy orchestrators still assume the old pairing/tray UI. In particular,
  `run-b0.ps1 -ResumeStopped` delegates to `scripts/test_stopped_gui.ps1`, whose
  full Stop/reconnect test still expects a QR dialog. Review/adapt automation
  before using it on main; these scripts were not revalidated by the USB tests.
- RTP receiver/reorder targets and the decoder replay's `--rtp` option were
  removed. Wireless collectors and authentication/startup scripts are historical
  tools, not supported main-branch validation instructions. Original code and
  the removed experiment plan remain at
  [253bd2d](https://github.com/Abdo-Eid/phonecam/tree/253bd2d/bench).
- Raw `runs/`, `e2e/`, `logs/`, images and private identifiers stay local/ignored.
  Distilling the docs does not delete those artifacts.

## Offline analysis

`analyze.py` summarizes phone/host segments; `compare_encoder.py` compares
matched experiment cohorts; `analyze_receipt.py` interprets native-consumer
receipt records and readable timer intervals. These do not access a device.

```powershell
python -m unittest discover -s bench -p 'test_*.py'
```

Do not treat first-frame-relative delay, RTT, file-write time or MF scheduled
sample age as absolute optical latency. Sensor/encoder “pairing misses” in
results mean telemetry timestamp matching, not the removed PC pairing feature.

## Native receipt capture

The fixed native MF consumer timestamps every `ReadSample` return before
inspection/copying, drains every sample and writes selected BMPs after capture.
It neither manages the PhoneCam session nor resamples at a lower frame rate.

```powershell
cmake -S windows -B windows/build
cmake --build windows/build --target phonecam-media-bench --config Debug
# Requires the intended host/session already running:
.\bench\capture-receipt.ps1 -DurationSeconds 10 -SaveEvery 30
```

`-ShowTimer` opens a visible reference timer and needs a readable controlled
scene. Transcribe only legible readings into `frame,timer_unix_ms` CSV, then:

```powershell
python bench/analyze_receipt.py bench/e2e/receipt-<run>/capture --timer-readings timer-readings.csv
```

Display scanout, exposure, buffering, image-copy overhead and clock adjustments
limit attribution. Distinct sample times do not prove distinct pictures. The
older `end-to-end*.ps1` file-write/fps-filter methods are not validated baselines.

## Opt-in instrumentation and experiments

- Phone telemetry: `adb shell setprop log.tag.PhoneCamBench DEBUG` before launch;
  reset it to INFO afterward. Host telemetry uses Debug `PHONECAM_BENCH=1`.
  Measurement overhead is unquantified; do not assume the instrumentation is free.
- `EncoderInventoryTest` enumerates/configures AVC options without camera
  frames. Configure/start acceptance is not proof a parameter is honored.
- `BenchEncoderSettings` and Debug `PHONECAM_BENCH_MEDIA` select isolated
  experiments; Release ignores these hooks. Media profiles change requested
  width/height/bitrate, not product connection behavior.
- `run-encoder-sweep.ps1`, `run-profile-sweep.ps1`, `run-consumer-stress.ps1` and
  `run_keyframe_response.py` preserve the historical experiment workflow, subject
  to the compatibility review above. Keep the phone UI visible until streaming;
  disconnected hidden sessions now exit rather than wait in the background.
- `run-decoder-recovery.ps1` builds a synthetic H.264 fixture and replays a missing
  access unit/reset through the actual MF decoder. It needs no phone, but its
  CPU load can invalidate a concurrent timing run. It establishes image-hash
  recovery for that fixture, not live corruption recovery or wireless smoothness.

For current product tests, builds and deployment use [docs/build.md](../docs/build.md).
For outstanding evidence gaps use [plan.md](../plan.md). The hotspot work is
[closed](../networking.md); no live measurement is currently authorized by these docs.
