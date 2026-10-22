# Media measurements

Active checkout: `D:\work\phonecam` on `main`. Status and scope: `../plan.md`.
The user authorized E1 and subsequent agreed media experiments on 2026-10-22.
No current-base performance improvement has been established yet.
Hotspot investigation is closed at the user's request: corruption containment
helped, but remaining lag was unacceptable; USB is the dependable path. See
`../networking.md` section 22. Do not automatically resume any live runner.

- `analyze.py`: offline segmented telemetry analysis (no device access).
- `run-b0.ps1`: opt-in phone/host baseline runner, forced USB/ADB path.
  Install the matching debug build first (unless letting the runner install it)
  and explicitly resume capture if the app retains a user-stopped state. The
  runner must not bypass the current product's Stop policy.
  `-ResumeStopped` explicitly uses the existing tray Start helper when needed.
  Empty/short telemetry runs now fail instead of being accepted as baselines.
  Installed APK hash is checked even with `-SkipInstall`; a refused installation
  aborts rather than silently measuring an old build.
- `results.md`: current-base observations plus historical results and caveats.
- `legacy-plan.md`: archived plan; its status is superseded by `../plan.md`.
- `timer-window.ps1` and `end-to-end*.ps1`: experimental timer/capture tools.
  They open visible windows and use the real camera. **Not validated latency
  benchmarks**: file-write timestamps and fps resampling cannot establish
  frame-receipt latency. Do not run them as part of the migration.
- `capture-receipt.ps1` / `analyze_receipt.py`: the replacement fixed native MF
  consumer. It timestamps every `ReadSample` return with QPC and precise Windows
  UTC time, keeps selected samples in memory and writes BMPs only after capture
  stops. The stream is not resampled. It never manages the host/session itself.

## Native receipt capture

Build (does not deploy the vcam DLL):

```powershell
cmake -S windows -B windows/build
cmake --build windows/build --target phonecam-media-bench --config Debug
```

With the intended PhoneCam host/session already running:

```powershell
.\bench\capture-receipt.ps1 -DurationSeconds 10 -SaveEvery 30
```

Use `-ShowTimer` only when the phone is physically aimed at the left half of
the primary monitor. That option opens a Unix-millisecond reference timer.
Read only fully legible digits from saved BMPs and create `timer-readings.csv`
with columns `frame,timer_unix_ms`. Then analyze the capture directory:

```powershell
python bench/analyze_receipt.py bench/e2e/receipt-<run>/capture --timer-readings timer-readings.csv
```

This measures receipt-side visual age, not universal product latency. Timer
refresh/scanout, exposure, consumer buffering and clock adjustments matter.
Raw-image copying can affect later samples. Sample timestamps are not image
identities: zero repeated sample times does NOT prove zero repeated pictures.
Keep this MF consumer fixed for comparisons; do not mix it with old DirectShow
or ffplay results. The wrapper bounds stalls and closes only its own processes.

Enable phone telemetry before restarting the app:
`adb shell setprop log.tag.PhoneCamBench DEBUG`. Host telemetry requires
`PHONECAM_BENCH=1`. Both are opt-in; measurement overhead remains unquantified.
`encoder_write_us` now measures the complete `VideoPacketSink.writePacket`
call, including RTP packetization on an RTP session.

The host fields named `decode_us` and `ring_us` describe elapsed time from
packet callback to decoded callback, then decoded callback to publication.
They can include scheduling, locking and bookkeeping, not just pure codec/copy
cost. A decoder buffering across packets needs additional attribution before
these are treated as exact per-frame decode latency.

Local `runs/`, `e2e/`, `logs/` and generated images are ignored, not deleted.
`logs/migration/` preserves the old worktree's original tracked-file diff.
No builds, APK installation, camera runs or vcam deployment are implicit in
an offline analyzer invocation.

## Encoder experiments (debug only)

`EncoderInventoryTest` writes `files/bench/encoder-inventory.json` in the debug
app. It enumerates AVC capabilities and probes configure/start with one optional
setting at a time, without camera frames or transport. Success does not establish
that a key is honored. Real-device instrumentation runs should select this test
class only, not unrelated data-reset/lifecycle tests.

`BenchEncoderSettings` is gated by debug-build telemetry. `-EncoderCase` on
`run-b0.ps1` selects exactly one dedicated log tag; all experiment tags reset
to INFO afterward. Normal/release behavior keeps the existing format defaults.
The active case is logged before configure. `--warmup-seconds 8` excludes initial
per-side startup from each experiment summary; earlier historical runs were not
trimmed this way and are not the comparison controls.

```powershell
.\bench\run-encoder-sweep.ps1 -Repeats 3 -DurationSeconds 30 -Inventory bench/runs/<E1-run>/encoder-inventory.json
```

The sweep tests only settings accepted by the device probe, interleaves controls
with candidates, archives the run order, and produces `comparison.json` using
`compare_encoder.py`. It does not adopt a setting or declare an end-to-end win.
Keep framing/lighting stable and avoid builds/other load while the sweep runs.

## Resolution, bitrate, steady runs and recovery

`run-profile-sweep.ps1` interleaves three runs of 1080p at 4/6/8/12 Mbps and
720p at 6 Mbps. The debug host's `PHONECAM_BENCH_MEDIA` hook changes only media
request fields; it does not change connection behavior. Release ignores it.
Each run must confirm the actual dimensions/bitrate in CurrentSettings. The
profile cohort has fresh default controls, not the older encoder-sweep binary.

`run-b0.ps1 -DurationSeconds 600 -SkipInstall -ResumeStopped -CaptureConsumer`
executes the default steady test with a real native virtual-camera consumer.
Long runs stream logcat; the consumer keeps every receipt timestamp but saves
no images. USB/ADB needs neither hotspot nor USB tethering. The runner does
not alter lock-screen, security, Wi-Fi or tethering settings.

`run_keyframe_response.py` (run only when the host is otherwise stopped) uses
the existing console keyframe command with 5-second periodic IDRs. It measures
same-PC command issue to the next decoded keyframe at the host ring, joining
phone/host records by PTS identity only. It is not clean-picture recovery or
glass-to-glass latency. Its command/sample matching has offline regression tests.

`run-decoder-recovery.ps1` is a separate synthetic, offline replay through the
actual `MFH264Decoder` implementation: one missing access unit and decoder reset,
with initial-only, cached and repeated parameter sets. Decoded NV12 hashes are
compared with a complete-stream reference. This is NOT live-phone packet loss
or evidence that the current USB/RTP startup contracts are broken. Do not run
its generation/build/replay while live timing cohorts are active.

### RTP corruption recovery (PC-only)

`phonecam-rtp-recovery-tests` exercises receiver assembly/recovery on ephemeral
loopback ports, never the installed host's live port. Build/run it alongside
`phonecam-video-tests` in Debug and Release. It requires no phone or camera.
`phonecam-rtp-reorder-tests` separately checks the gap-only 12 ms / 64-packet
reorder policy with an injected clock. The receiver integration suite also
checks actual socket deadline expiry without a subsequent packet arrival.

`phonecam-decoder-recovery <fixture.h264> <new-output-directory> --rtp` also
packetizes a synthetic fixture, injects loss/late joining and routes it through
the actual RTP receiver and MF decoder. `rtp-summary.csv` reports picture counts,
hash mismatches and recovery frame indices. Healthy replay must exactly match
the direct decode; every delivered picture after loss must match the reference,
with dependent pictures withheld until a self-contained IDR.
The additional reordered-fragment scenario must deliver every reference picture
without triggering unnecessary loss recovery. Passing it does not establish that
any particular live sequence gap was caused by reordering rather than real loss.

Network phone/offline collectors default to the single USB ADB device (`adb -d`),
not a hard-coded private device serial. With multiple USB devices, set
`ANDROID_SERIAL` to the intended USB device locally. Raw diagnostics and images
belong under the ignored artifact directories, never in a public commit.

Generate the fixture with at least 120 frames at 30 fps, GOP 30, no B frames,
AUD delimiters and repeated SPS/PPS; multi-slice output exercises recovery of
partially lost IDRs, e.g.:

```powershell
ffmpeg -hide_banner -loglevel error -nostdin -f lavfi -i 'testsrc2=size=1920x1080:rate=30' -frames:v 120 -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -keyint_min 30 -sc_threshold 0 -bf 0 -b:v 6M -x264-params 'aud=1:repeat-headers=1:slices=4' -pix_fmt yuv420p -f h264 synthetic.h264
```

Use a new ignored artifact directory. The replay is paced and uses generated
images, not the user's camera. Its frame indices are not live/wall-clock recovery
latency, and passing it does not establish wireless bandwidth or smooth video.

`run-consumer-stress.ps1 -ResumeStopped` keeps one default USB/ADB camera session
alive while alternating three 15-second captures each at 0/100 ms read delay.
No images are saved. Phone must be unlocked for camera startup; the opt-in tray
Start helper is used only if capture is stopped. `mf_sample_to_receipt_us` uses
MF receipt time minus scheduled sample time; this is delivery/queueing age,
**not sensor/image age**. Compare first/last thirds and timestamp skips, and do
not mistake a fresh reader's recovery for continuous-reader drain recovery.
