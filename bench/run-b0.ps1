param(
  [ValidateRange(1,900)][int]$DurationSeconds = 30,
  [string]$Label = "B0",
  [switch]$SkipInstall,
  [switch]$ResumeStopped,
  [ValidateSet('default','low_latency','latency0','priority0','max_b_frames0','intra_refresh30','idr2','idr5','profile_baseline','profile_constrained_baseline')]
  [string]$EncoderCase = 'default',
  [ValidateSet('default','720p6m','1080p4m','1080p8m','1080p12m')]
  [string]$MediaProfile = 'default',
  [switch]$CaptureConsumer
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

# Debug-only tags recognized by BenchEncoderSettings. Product/release defaults never change.
$caseTags = @{
  low_latency = 'PCBenchLowLatency'; latency0 = 'PCBenchLatency0'; priority0 = 'PCBenchPriority0'
  max_b_frames0 = 'PCBenchNoBFrames'; intra_refresh30 = 'PCBenchIntraRefresh'
  idr2 = 'PCBenchIdr2'; idr5 = 'PCBenchIdr5'
  profile_baseline = 'PCBenchBaseline'; profile_constrained_baseline = 'PCBenchConstrained'
}
$expectedSettings = @{
  default = '1920x1080@30 bitrate=6000000'; '720p6m' = '1280x720@30 bitrate=6000000'
  '1080p4m' = '1920x1080@30 bitrate=4000000'; '1080p8m' = '1920x1080@30 bitrate=8000000'
  '1080p12m' = '1920x1080@30 bitrate=12000000'
}

# Only one host may hold the virtual camera. Abort instead of interfering with another session.
$existing = Get-Process phonecam-host -ErrorAction SilentlyContinue
if ($existing) {
  throw "phonecam-host is already running (PID $($existing.Id -join ', ')). Stop it before measuring."
}

$run = Join-Path $PSScriptRoot ("runs\" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-$Label")
New-Item -ItemType Directory -Force -Path $run | Out-Null

if (-not $SkipInstall) {
  adb install -r .\android\app\build\outputs\apk\debug\app-debug.apk |
    Out-File (Join-Path $run "install.txt")
  if ($LASTEXITCODE -ne 0) { throw "APK installation failed; run not started: $run" }
}
$packageLines = & adb shell pm path io.github.abdoeid.phonecam
$pmExit = $LASTEXITCODE
$package = (($packageLines | Select-Object -First 1) -replace '^package:', '').Trim()
if ($pmExit -ne 0 -or -not $package) { throw "Cannot identify installed APK (adb exit $pmExit)" }
$hashLines = & adb shell sha256sum $package
$hashExit = $LASTEXITCODE
$installedHash = ($hashLines -split '\s+')[0]
$localHash = (Get-FileHash .\android\app\build\outputs\apk\debug\app-debug.apk).Hash
if ($hashExit -ne 0 -or $installedHash -ne $localHash) { throw 'Installed APK does not match the benchmark build' }
"installed_apk_sha256=$installedHash" | Out-File (Join-Path $run 'installed-apk.txt') -Encoding utf8
adb logcat -G 8M | Out-Null
adb shell setprop log.tag.PhoneCamBench DEBUG
foreach ($tag in $caseTags.Values) { adb shell setprop "log.tag.$tag" INFO }
if ($EncoderCase -ne 'default') { adb shell setprop "log.tag.$($caseTags[$EncoderCase])" DEBUG }
adb shell am force-stop io.github.abdoeid.phonecam
adb logcat -c
$logcatProcess = $null
$hostProcess = $null
$consumerProcess = $null
$previousProfile = $env:PHONECAM_BENCH_MEDIA
$previousBench = $env:PHONECAM_BENCH
try {
if ($DurationSeconds -gt 120) {
  # Stream long runs instead of trusting a finite device log buffer.
  $logcatProcess = Start-Process adb -ArgumentList @('logcat','-v','monotonic','-s','PhoneCamBench:D','*:S') `
    -PassThru -RedirectStandardOutput (Join-Path $run 'android.log') `
    -RedirectStandardError (Join-Path $run 'logcat.err.log')
}
adb shell am start -n io.github.abdoeid.phonecam/.MainActivity |
  Out-File (Join-Path $run "launch.txt")
if ($LASTEXITCODE -ne 0) { throw 'Could not launch the phone app' }

Start-Sleep -Seconds 3
$env:PHONECAM_BENCH = "1"
$env:PHONECAM_BENCH_MEDIA = $MediaProfile
$hostOut = Join-Path $run "host.out.log"
$hostErr = Join-Path $run "host.err.log"
$hostProcess = Start-Process `
  -FilePath (Resolve-Path .\windows\build\host\Debug\phonecam-host.exe) `
  -ArgumentList '--adb' `
  -WorkingDirectory $root `
  -RedirectStandardOutput $hostOut `
  -RedirectStandardError $hostErr `
  -PassThru

  Start-Sleep -Seconds 8
  if ($hostProcess.HasExited) {
    throw "phonecam-host exited during startup (exit code $($hostProcess.ExitCode))."
  }
  if ($ResumeStopped -and (Select-String -Path $hostErr -Pattern 'Connection: Stopped' -Quiet)) {
    # Explicit opt-in: use the product's tray Start action, never edit saved Stop state.
    & (Join-Path $root 'scripts\test_stopped_gui.ps1') -ProcessId $hostProcess.Id `
      -Action Resume -LogPath $hostErr
    Start-Sleep -Seconds 8
  }
  if ($CaptureConsumer) {
    $consumerProcess = Start-Process `
      -FilePath (Join-Path $root 'windows\build\tools\media_bench\Debug\phonecam-media-bench.exe') `
      -ArgumentList @("`"$run\consumer-capture`"", $DurationSeconds, '0') -PassThru `
      -RedirectStandardOutput (Join-Path $run 'consumer.out.log') `
      -RedirectStandardError (Join-Path $run 'consumer.err.log')
  }
  Start-Sleep -Seconds $DurationSeconds
  if ($consumerProcess) {
    if (-not $consumerProcess.WaitForExit(20000)) { throw 'Native consumer stalled; run is invalid' }
    if ($consumerProcess.ExitCode -ne 0) { throw "Native consumer failed; see $run\consumer.err.log" }
  }
  if ($logcatProcess) {
    if ($logcatProcess.HasExited) { throw 'Logcat streaming exited; long-run capture is invalid' }
  } else {
    adb logcat -d -v monotonic -s "PhoneCamBench:D" |
      Out-File (Join-Path $run "android.log") -Encoding utf8
  }
}
finally {
  if ($consumerProcess -and -not $consumerProcess.HasExited) { Stop-Process -Id $consumerProcess.Id -Force }
  if ($hostProcess -and -not $hostProcess.HasExited) {
    Stop-Process -Id $hostProcess.Id -Force
  }
  $env:PHONECAM_BENCH = $previousBench
  $env:PHONECAM_BENCH_MEDIA = $previousProfile
  if ($logcatProcess -and -not $logcatProcess.HasExited) { Stop-Process -Id $logcatProcess.Id -Force }
  foreach ($tag in $caseTags.Values) { adb shell setprop "log.tag.$tag" INFO }
}

@(
  "created=$(Get-Date -Format o)",
  "commit=$(git rev-parse HEAD)",
  "branch=$(git branch --show-current)",
  "device=$(adb shell getprop ro.product.model)",
  "android=$(adb shell getprop ro.build.version.release)",
  "app=$(adb shell dumpsys package io.github.abdoeid.phonecam | Select-String -Pattern 'versionName' | Select-Object -First 1)",
  "duration_s=$DurationSeconds",
  "run=$run",
  "transport=adb",
  "working_directory=$root"
  "encoder_case=$EncoderCase"
  "analysis_warmup_seconds=8"
  "media_profile=$MediaProfile"
  "capture_native_consumer=$CaptureConsumer"
) | Out-File (Join-Path $run "metadata.txt") -Encoding utf8

git diff --binary --output="$(Join-Path $run 'source.patch')"
git status --porcelain | Out-File (Join-Path $run 'source-status.txt') -Encoding utf8
Get-FileHash @(
  '.\windows\build\host\Debug\phonecam-host.exe',
  '.\android\app\build\outputs\apk\debug\app-debug.apk',
  'C:\ProgramData\PhoneCam\phonecam-vcam.dll'
) | Export-Csv (Join-Path $run 'artifact-hashes.csv') -NoTypeInformation

python .\bench\analyze.py (Join-Path $run "android.log") (Join-Path $run "host.err.log") `
  --require-frames ([Math]::Max(1, $DurationSeconds * 20)) --warmup-seconds 8 |
  Tee-Object -FilePath (Join-Path $run "summary.txt")
if ($LASTEXITCODE -ne 0) { throw "Invalid baseline; retained diagnostics at $run" }
if (-not (Select-String -Path $hostOut -SimpleMatch $expectedSettings[$MediaProfile] -Quiet)) {
  throw "Requested media profile not confirmed by CurrentSettings: $MediaProfile; run is invalid"
}
if (-not (Select-String -Path (Join-Path $run 'android.log') -SimpleMatch "encoder_case name=$EncoderCase" -Quiet)) {
  throw "Encoder case was not confirmed in telemetry: $EncoderCase; run is invalid"
}
python .\bench\analyze.py (Join-Path $run 'android.log') (Join-Path $run 'host.err.log') `
  --require-frames ([Math]::Max(1, $DurationSeconds * 20)) --warmup-seconds 8 --json |
  Out-File (Join-Path $run 'summary.json') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "JSON analysis failed: $run" }
if ($CaptureConsumer) {
  python .\bench\analyze_receipt.py (Join-Path $run 'consumer-capture') |
    Out-File (Join-Path $run 'consumer-summary.json') -Encoding utf8
  if ($LASTEXITCODE -ne 0) { throw "Native receipt analysis failed: $run" }
}
