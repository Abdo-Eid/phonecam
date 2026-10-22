# Media-pipeline work — current status

Updated: 2026-10-22. Active checkout: `D:\work\phonecam`, branch `main`.
Migration base: `387f8b9` (stopped-app cleanup, vcam pacing and RTP startup fixes).

## Where we are

- **Closed at the user's request.** Final hotspot verdict after the reordering
  follow-up: **"better but still lagging; not worth the effort anymore"**.
  Use USB as the dependable path; no further hotspot/network/phone experiments
  are active or requested. Retain the implemented corruption/reordering safeguards,
  but do not mark wireless smoothness accepted or its root cause solved.
  Full closeout: `networking.md` section 22 and `bench/results.md`.
  Final offline checks: Debug/Release reordering, 17 receiver cases, video timing
  and media-setting tests; 28 Python tests; benchmark PowerShell syntax; whitespace
  checks. Final Android rebuild/validation remains unconfirmed, as documented;
  no phone access was needed for closeout. User authorized committing/pushing the
  accumulated source, tools and sanitized documentation, not raw local artifacts.
- Prior authorized software scope (now stopped): **USB was the dependable fallback,
  not abandonment of hotspot recovery work**. No more router/driver comparisons or phone connection
  for development; implement a concrete improvement and the user will try hotspot
  once more. RTP recovery is now hardened and installed in the PC host only:
  discard the entire damaged picture, withhold dependent pictures until an intact
  IDR with SPS/PPS/first slice, and signal decoder discontinuity on recovery.
  User's live verdict: **no pixelation, but freezing/lagging**. Not accepted as
  smooth hotspot operation. Saved log records 90 sequence-gap events; loss versus
  reordering was not distinguished. Follow-up adds gap-only bounded reordering
  (12 ms target / 64 packets), keeping clean-IDR recovery on genuine/unresolved loss.
  17 receiver cases plus bounded-reorder unit checks pass in Debug/Release;
  Release receiver suite passed 10 repetitions. Six synthetic 1080p RTP-to-MF
  image-hash scenarios pass, including reordered delivery of all 120 images.
  No Android/USB/encoder/bitrate/resolution or network-setting change. New installed
  host SHA256: `010117C0C6FD63BB8F0083DCACDF8F74D4C3D77DA2D16A8B6D708EC8FD9E367C`.
  Prior candidate and original host remain backed up under their update directories.
  Latest backup/hash/unchanged registry snapshots/local logs:
  `windows/build/updates/rtp-reorder-20261022/`. The user subsequently reported the
  follow-up **better but still lagging**, then stopped further work. This is a
  subjective live improvement, **not accepted smoothness** or controlled A/B.
  USB remains the dependable path. See `networking.md` 20–22.
- Separate network investigation: `networking.md` records the user's upstream
  A/B story, unchanged phone-to-PC hotspot topology and Network B baseline.
  PC/phone ICMP and capped local TCP checks are recorded for B and A; return-B
  recovered on initial PC checks but degraded during the hotspot stage, then
  recovered in a later short idle check. User reports DNS moved from `1.1.1.3` to router/default; PC
  DHCP DNS is verified. The first repeat had phone off hotspot; after restoring
  it, gateway/internet delay and missed replies returned, so DNS is not a proven
  fix. Router 2.4 GHz b/g/n update is now tested: PC link report rose from 54 to
  300 Mbps, but poor gateway/internet responsiveness remains.
  Phone hotspot retry-counter deltas are preserved with interpretation caveats.
  User requested an upstream-disconnected/hotspot-on test; standalone
  `bench/hotspot_offline.py` is prepared, tested and documented. Manual offline
  collection is now recorded in `networking.md` section 15; no automatic network
  changes or active job. Local ICMP impairment persists without upstream Wi-Fi,
  while both small 8 Mbps-capped TCP payloads completed.
  First offline attempt was blocked by an overly strict retained-route check;
  user had disconnected correctly. Guard fixed and replayed against the saved
  state; 26 regression tests passed. User's corrected offline run completed and
  its phase-boundary topology/counters were checked; no single root cause claimed.
  No networking defaults
  changed by the assistant during those diagnostic runs; no video retest was involved.
  User clarified offline PC-hosted hotspot performance is the goal and reports
  phone-hosted offline hotspot was close to cable. Online research and supported
  local-only Wi-Fi Direct AP alternative are recorded in `networking.md` section
  16; neither a driver fix nor AP prototype is implemented/validated. The old
  8 Mbps tests were capped deliberately, not measurements of maximum capacity.
  Fast-path read-only inspection: ASUS FA507RC already has its listed OEM
  3.0.1.1338 driver, Wi-Fi Maximum Performance AC/DC and U-APSD disabled.
  User's own PhoneCam trial is acceptance, not synthetic network scores. A
  reversible ax-to-ac compatibility trial was subsequently explicitly approved
  and applied via UAC; only CurrPhyMode changed, original state saved. PC hotspot
  recovered Up at 192.168.137.1; phone association is unverified because ADB queries
  failed. User's own PhoneCam verdict was **same**, so the original ax setting
  was restored and verified; hotspot recovered Up. This trial is not a fix.
  Older ASUS-listed 3.0.1.1325 is locally available as oem114.inf and its INF
  matches the hardware; a separately approved driver comparison is the next
  candidate, not yet applied (`networking.md` 17). No driver/application defaults changed.
  User declined the driver comparison. A subsequent user-reported return to B
  received a read-only check (`networking.md` 18): upstream Connected, hotspot On
  with one client at .83, ax retained. PhoneCam host was open, ADB absent; no
  active probes/video tests or performance-recovery claim.
- Optical measurement execution closed at the user's request after the short
  capture; no further handheld setup is required. The installed host was later
  updated/relaunched for the separately authorized RTP recovery trial above.
  Retain all performance defaults. Unfinished acceptance checks below are not
  represented as completed experiments.
- Measurement sources, scripts and historical artifacts have been transferred
  from the old `media-pipeline` worktree. The original source diff is archived
  under ignored `bench/logs/migration/` before removing that worktree.
- Sensor/AE telemetry and encoder timing are adapted to the current code,
  including `VideoPacketSink` and the camera stop/cancel guards.
- Host receive/decode/ring timing is adapted without replacing the current
  decoder reset, discontinuity, output gating or connection management.
- Current-base B0, encoder inventory, 21 encoder-setting runs, 15 media-profile
  runs, the ten-minute steady test, and synthetic/live keyframe checks are recorded.
  No performance default has changed; details and limitations are below.
- Validation passed: Windows Debug host build, Android debug APK build and JVM
  tests, offline analyzer regression test, PowerShell syntax checks and
  `git diff --check`. After migration, the matching APK was installed and the
  three B0 runs below were executed over USB/ADB.
- `main` now tracks `origin/main`; the 11 existing commits were published with
  permission. The migrated benchmark work is still uncommitted. The old
  worktree was removed after verifying the transfer; submodules remain intact.
- User authorized continuing the agreed media work on 2026-10-22. No optional
  key becomes a product default merely because configure/start succeeds.

## What remains valid from the old work

The three runs on `2d9ee9e` are historical segmented observations:
approximately 96 ms sensor-to-encoder output, 6 ms host decode and 0.2 ms ring
handoff. They do not establish current-product latency or an improvement.
Raw logs and captures are preserved locally under ignored `bench/runs/`,
`bench/logs/` and `bench/e2e/`. Results and caveats: `bench/results.md`.

The 652.5 ms file-write-based reading is **not a validated end-to-end baseline**.
The 2 fps filter, conversion, PNG writing, buffering and sample selection all
confound it. Do not subtract the historical segment medians and attribute the
remainder to the virtual camera. Likewise, first-frame-relative delay does not
measure absolute latency differences between restarted sessions.

## Current progress — 2026-10-22

- First attempted run (`20261022-120707-B0-main-r1`) was invalid: the phone was
  Stopped and produced no frames. Diagnostics retained; never count it as B0.
- The runner/analyzer now reject empty or short runs. MIUI denied shell touch
  injection; used the existing, explicit tray Start helper instead, without
  changing phone security settings or editing its persisted Stop state.
- Three successful runs: `20261022-121541-B0-main-r1-resumed`,
  `20261022-121641-B0-main-r2`, `20261022-121727-B0-main-r3`.
  Observed host rate ~30.3 fps; phone sensor-to-encoder p50 98.3–99.4 ms,
  p95 108.2–109.2 ms; host decode p50 5.9–6.2 ms. Pairing misses: zero.
  Scene/lighting were not standardized, and session start/end counts differ
  by a frame: do not claim loss-free delivery or a win/regression versus old B0.
- Phone is on the laptop hotspot, but these runs deliberately forced USB/ADB.
  A hotspot run must be recorded separately rather than changing transport
  halfway through a baseline.
- Added `phonecam-media-bench`, a fixed native MF consumer. It timestamps each
  `ReadSample` return before inspecting/copying the sample, drains every frame,
  and writes selected images only after capture. No fps filter or PNG writing
  in the acquisition loop. Capture watchdog is in `bench/capture-receipt.ps1`.
- Test-pattern smoke run: 150 samples / 5 s, zero repeated/backward sample times.
  Live USB camera smoke run: 145 samples, 29.991 receipt fps, zero repeated or
  backward sample times (`bench/e2e/receipt-20261022-121945-130`). Neither test
  proves image continuity or optical end-to-end latency.
- **Next blocker:** a readable physical reference timer in the native consumer's
  saved frames. Do not use file write times. This gates absolute latency claims,
  not the newly authorized independent encoder inventory.

## Upstream changes already present

`387f8b9` implements asynchronous 30 fps vcam pacing and timestamp handling,
decoder reset on session/discontinuity, and RTP startup/timestamp fixes.
RTP now repeats SPS/PPS with every keyframe. Do not redo these as new changes.
Their performance benefit still needs a controlled measurement; recorded
startup/timestamp correctness tests are not glass-to-glass measurements.

## Execution tracker

| Item | Status | Evidence / remaining work |
|---|---|---|
| Matching builds and segmented B0 | Recorded | Three provisional runs above; new experiment controls use the same debug APK as their candidates |
| Receipt-time capture method | Short optical observation recorded | 10-second handheld capture, 20 saved images; overlapping timer digits limit precision; not a controlled 60-second baseline |
| E1 encoder inventory | Done for default device encoder | `bench/runs/20261022-122914-E1-inventory/encoder-inventory.json` |
| E2 fixed AE in bright/dim light | Partial | All 21 encoder-sweep runs report [30,30] and 33.333 ms frame duration; controlled lighting test remains |
| E3 IDR / intra-refresh | Three-run sweep done | Longer intervals honored; no demonstrated latency/bandwidth win; intra-refresh activity unproven; defaults retained |
| E4 encoder latency options | Three-run sweep done | No convincing benefit; low-latency=1 rejected at configure; no hint adopted |
| E5 bitrate / resolution | Three-run sweep done; quality/optical checks pending | 15 passing runs; 720p phone p95 ~4.5 ms lower, not a proven E2E win; bitrate-only changes not adopted |
| B5 ten-minute stability | Recorded | 17,999 native samples at 30.002 fps; no timestamp repeats/reversal or accumulating phone delay; unique pictures not established |
| E6 backpressure | Slow-consumer check done | Six captures, one session; 100 ms read delay increases bounded MF sample age (~166 ms), not an accumulating FIFO; optical image age unknown |
| E7 decoder cost / GPU | Software path measured; GPU deferred | B5 decode p50/p95 11.11/16.91 ms; profile-dependent cost; no controlled GPU A/B or CPU-utilization claim |
| E8 slices | Deferred | No verified standard slice-output support; no transport redesign here |
| E9 recovery | Synthetic replay and live command checks done; live clean-image recovery pending | Synthetic missing AU/reset hashes; 10/10 live requests, 64.34 ms median to decoded keyframe |
| E10 SPS/PPS | Upstream RTP policy retained | Already repeats with each RTP IDR; USB policy and recovery require separate evidence |

E1 findings: `OMX.qcom.video.encoder.avc`, hardware encoder, advertises 1080p30
and 720p30. `low-latency=1` fails configure with `IllegalArgumentException`.
Baseline and Constrained Baseline profile-only requests fail with codec error
`0x80001001`. Latency=0, priority=0, max-B-frames=0, intra-refresh-period=30 and
2/5-second IDR configure/start successfully. **Acceptance is not proof of effect.**
The codec does not advertise CBR or intra-refresh support despite accepting those
requested settings; do not trust format acceptance as evidence they are honored.

Completed batch: `bench/runs/sweep-20261022-131644`, all 21 runs successful.
Candidate median run p95 deltas were only -0.288 to +0.268 ms versus default;
the three defaults themselves spanned ~0.391 ms in p95. No hint is adopted.
See `bench/results.md` for the decision table and caveats.

Completed profile batch used `bench/run-profile-sweep.ps1` and a debug-only media-setting hook
that changes only width/height/bitrate fields of the existing StartVideo command.
Transport, pairing, discovery and retry behavior are unchanged. Release builds
ignore the hook; every run verifies actual CurrentSettings. The following default
ten-minute steady test used the native virtual-camera consumer.
Long runs stream logcat instead of trusting a finite log buffer.

E5: `profiles-20261022-144900`, three repeats of each of five profiles, identical
artifacts and no failed runs. Median phone p95: default 99.025 ms, 720p6m
94.518 ms, 1080p4m 99.046 ms, 1080p8m 99.208 ms, 1080p12m 98.523 ms.
720p trades detail for a lower measured segment; retain 1080p/6 Mbps defaults
until controlled image-quality/optical evidence supports a change.

B5: `20261022-150039-B5-default-native-consumer`, phone/encoder/host ~30.303 fps,
zero sensor/encoder pairing misses, phone p50/p95 89.20/98.11 ms. Native receipt
30.002 fps; no duplicate/backward sample timestamps. Receipt interval p95/p99/max
47.28/49.53/82.08 ms, so rational sample timestamps do not mean uniform arrivals.
Minute bins show no accumulating phone-side delay; reported thermal stayed 0 and
bitrate 6000 kbps. This is not proof of unique images or an optical E2E result.

E9: synthetic `20261022-151849-E9-synthetic-recovery` recovers a missing AU at
the next IDR (500 ms media time). Reset without SPS/PPS never recovers in the
six-second corpus; cached/repeated headers recover in 500/533.333 ms. This does
not demonstrate a broken live startup contract. Live `20261022-153322-E9-keyframe-response`
has 10/10 acknowledged requests followed by decoded keyframes, p50 64.337 ms,
max 78.873 ms from PC command issue to host ring. This is not corrupted-picture
recovery or optical latency. Locked-phone failed attempt is retained/excluded.

The rejected stay-awake attempt left/restored the original setting 0. No batch
changes power/security/network settings. Live clean-picture, optical latency,
and controlled bright/dim quality checks still need a controlled readable scene.

E6: `20261022-154733-E6-consumer-stress`, three alternating repeats of 0/100 ms
read delay, 15 s each, no image saves. Normal receipt 29.977–29.995 fps and MF
scheduled-sample age p50 18.589–18.851 ms. Slow receipt 9.167–9.182 fps and age
p50 165.909–167.501 ms; first/last thirds remain similar, maximum 183.526 ms.
Slow sample timestamps jump forward by 100/133.333 ms, consistent with skipped
paced samples rather than an ever-growing queue. Host rate remains ~30.301 fps.
This does not prove unique pictures, low optical latency, or continuous-reader
recovery: each consumer reopened, while the camera session stayed alive.

Final validation follows live measurements. Windows Debug/Release media targets
built; media-setting isolation, video pacing/RTP startup, and pairing regression
tests passed in both configurations. Release host binary contains neither the
benchmark environment names nor the telemetry format string. Nine offline Python
tests, PowerShell syntax checks and `git diff --check` passed. The all-target Debug
build was blocked by the already-running service executable; it was not stopped
or replaced. Android Debug/Release APK + Debug unit/instrumentation-APK validation
did not finish: dependency download stalled, then the build attempt was canceled
to end the handheld session. The offline retry was interrupted too. Earlier
Debug build/JVM tests remain valid for their recorded source, not the final
callback change. This project exposes no `testReleaseUnitTest` task.

Two instrumentation-only
isolation fixes: Android no longer registers a per-frame callback when disabled;
Windows Release now ignores the telemetry environment flag as well as media
profiles. Original measured APK/host are archived under ignored
`bench/logs/pre-final-validation-20261022-155113/`; new builds form a new cohort.

## Next actions

The following are remaining acceptance gaps, **not active jobs**. Do not require
further phone handling in this session.

1. Validate Release/debug isolation and final builds/tests (live timings finished).
2. Preserve benchmark artifacts and keep post-validation builds separate from measured cohorts.
3. With a readable scene, complete optical receipt-timer validation and paired
   bright/dim/quality checks. Do not claim the full plan completed before these.
4. Retain media defaults unless end-to-end, quality and continuity evidence supports
   a change. Original media acceptance work remains separate from the subsequently
   authorized RTP recovery work; the first live trial stopped pixelation but
   froze/lagged, and the user found the reordering follow-up better but still
   unacceptably laggy. Further work is stopped, not represented as completed.

Still unmeasured (not silently marked done): host process CPU usage at 720p/1080p,
full decode→ring→vcam copy attribution with image identity, slow-consumer picture
freshness, live clean-frame recovery/mid-stream join, and decoder-low-latency
on/off A/B. GPU and slice redesigns remain deferred, not failed experiments.
The existing data supports retaining defaults, not declaring every original
experiment or its end-to-end acceptance criteria satisfied.

## Closing optical observation and export fix

User requested a quick final capture rather than more handheld waiting.
`bench/e2e/optical-final-20261022-161636` links the ten-second capture
`bench/e2e/receipt-20261022-161642-621`: 300 native samples, 20 saved images,
30.029 receipt fps. Timer and camera were stopped immediately afterward.

The benchmark BMP exporter had inverted positive-stride top-down RGB rows by
writing a positive BMP height. Fixed the header sign in `main.cpp`; new capture
is upright, confirmed visually and by its -1080 BMP height. Portrait pillarboxing
inside the fixed 1920x1080 consumer is expected; no phone rotation fix was needed.
Earlier raw BMPs remain unchanged. Timing CSVs are unaffected by the export fix.

Only stable leading timer digits were transcribed; the final two digits overlap
display refreshes. `timer-reading-bounds.csv` and `visual-age-bounds.json` record
100-ms-wide timestamp intervals for all 20 selected images. Across these bounds,
visual timer-to-receipt age lies within roughly 97–212 ms. This is a short,
handheld, uncertainty-bounded observation, not a precise p50/p95 latency claim,
the planned 60-second controlled baseline, or evidence of a tuning improvement.
Display/exposure/copying and incompletely verified build cancellation are caveats.
Nine offline tests and `git diff --check` passed after the exporter change; its
Debug consumer build/capture passed. The interrupted final Release consumer build
and final Android build must not be reported as passed.

## Installed desktop update — 2026-10-22

At the user's explicit request, the installed desktop files were updated in place
from the current Release checkout builds. Host/service/USB helper and dependencies
were hash-verified. The shared vcam DLL already matched the current Release output
(`A88838...D2816`), so it was preserved rather than replacing a loaded identical DLL.
The running PhoneCamRingService now uses the quoted installed path
`"C:\Program Files\PhoneCam\phonecam-svc.exe"`, automatic start, not the Debug
checkout. Connection registry settings are byte-for-byte unchanged. No Android
installation, camera handling, reboot, or performance-default change was needed.

Release builds and media-settings/video/pairing tests passed. Installed host's
PC-only pattern/registered-camera smoke test received 91 samples over three
seconds (~29.93 fps); the owned test host was stopped afterward. Backup files,
deployment diagnostics and verified hashes: `windows/build/updates/20261022-181944/`.
Installer attempt first rolled back on a locked bundled ADB; service restart
attempt also failed before installation. Final hash-aware in-place update passed.
The displayed package version remains 0.8.0 because no release-version bump was
made; installed binaries now match today's Release builds. This deployment does
not complete the remaining media-experiment acceptance gaps.

## Full experiment inventory

`bench/legacy-plan.md` preserves the original detailed plan and deferred ideas:
encoder capabilities (E1), fixed AE verification (E2), keyframe/intra-refresh
(E3), codec latency options (E4), bitrate/resolution sweeps (E5), backpressure
(E6), decoder/copy costs (E7), slices (E8), recovery (E9), SPS/PPS (E10).
Its old status/checklists and latency attribution are superseded by this file.
`docs/plan.md` is the existing connection/security plan and is left unchanged.
