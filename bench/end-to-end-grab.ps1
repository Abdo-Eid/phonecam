param(
  [int]$WarmupSeconds = 10,
  [int]$Shots = 5,
  [int]$ShotIntervalMs = 1000,
  [int]$BufferMb = 1
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$existingHost = Get-Process phonecam-host -ErrorAction SilentlyContinue
if ($existingHost) {
  throw "phonecam-host is already running (PID $($existingHost.Id -join ', ')). Stop it before measuring."
}

Add-Type -AssemblyName System.Windows.Forms
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$leftWidth = [int]($screen.Width / 2)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $PSScriptRoot ("e2e\grab-" + $stamp)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$timerProcess = $null
$hostProcess = $null

try {
  $timerProcess = Start-Process powershell -ArgumentList @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $PSScriptRoot 'timer-window.ps1'),
    '-Width', $leftWidth,
    '-Height', $screen.Height
  ) -PassThru -WindowStyle Minimized
  Start-Sleep -Seconds 1

  $env:PHONECAM_BENCH = '1'
  $hostProcess = Start-Process `
    -FilePath (Resolve-Path .\windows\build\host\Debug\phonecam-host.exe) `
    -ArgumentList '--adb' `
    -WorkingDirectory $root `
    -RedirectStandardOutput (Join-Path $outDir 'host.out.log') `
    -RedirectStandardError (Join-Path $outDir 'host.err.log') `
    -PassThru -WindowStyle Hidden
  Start-Sleep -Seconds $WarmupSeconds
  if ($hostProcess.HasExited) {
    throw "phonecam-host exited during startup (exit code $($hostProcess.ExitCode))."
  }

  $rows = @()
  for ($i = 1; $i -le $Shots; $i++) {
    $before = Get-Date
    $frame = Join-Path $outDir ('grab-{0:D2}.png' -f $i)
    $err = Join-Path $outDir ('grab-{0:D2}.err.txt' -f $i)
    & ffmpeg -hide_banner -loglevel error `
      -f dshow -rtbufsize "${BufferMb}M" -probesize 32 -analyzeduration 0 `
      -i 'video=PhoneCam (Windows Virtual Camera)' `
      -frames:v 1 -y $frame 2> $err
    $after = Get-Date
    $rows += [pscustomobject]@{
      shot = $i
      before = $before.ToString('o')
      after = $after.ToString('o')
      frame = $frame
    }
    Start-Sleep -Milliseconds $ShotIntervalMs
  }
  $rows | Export-Csv (Join-Path $outDir 'grabs.csv') -NoTypeInformation

  @(
    "created=$stamp",
    "commit=$(git rev-parse HEAD)",
    "branch=$(git branch --show-current)",
    "device=$(adb shell getprop ro.product.model)",
    "android=$(adb shell getprop ro.build.version.release)",
    "screen=$($screen.Width)x$($screen.Height)",
    "timer_left_width=$leftWidth",
    "warmup_s=$WarmupSeconds",
    "shots=$Shots",
    "shot_interval_ms=$ShotIntervalMs",
    "buffer_mb=$BufferMb"
  ) | Out-File (Join-Path $outDir 'metadata.txt') -Encoding utf8

  Write-Output $outDir
}
finally {
  foreach ($process in @($hostProcess, $timerProcess)) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
  }
  Remove-Item Env:PHONECAM_BENCH -ErrorAction SilentlyContinue
}
