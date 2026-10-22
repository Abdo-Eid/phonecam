param(
  [int]$WarmupSeconds = 8,
  [int]$Shots = 5,
  [int]$ShotIntervalMs = 700,
  [int]$PreviewWidth = 0,
  [int]$PreviewBufferMb = 8
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$existingHost = Get-Process phonecam-host -ErrorAction SilentlyContinue
if ($existingHost) {
  throw "phonecam-host is already running (PID $($existingHost.Id -join ', ')). Stop it before measuring."
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$leftWidth = [int]($screen.Width / 2)
if ($PreviewWidth -le 0) {
  $previewWidth = $screen.Width - $leftWidth
} else {
  $previewWidth = $PreviewWidth
}
$previewHeight = [int]($previewWidth * 9 / 16)
$previewLeft = $screen.Width - $previewWidth

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $PSScriptRoot ("e2e\" + $stamp)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$timerProcess = $null
$hostProcess = $null
$previewProcess = $null

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
  Start-Sleep -Seconds 6
  if ($hostProcess.HasExited) {
    throw "phonecam-host exited during startup (exit code $($hostProcess.ExitCode))."
  }

  $previewError = Join-Path $outDir 'ffplay.err.log'
  $previewOutput = Join-Path $outDir 'ffplay.out.log'
  $previewDevice = '"video=PhoneCam (Windows Virtual Camera)"'
  $previewProcess = Start-Process ffplay -ArgumentList @(
    '-hide_banner',
    '-loglevel', 'warning',
    '-f', 'dshow',
    '-rtbufsize', "${PreviewBufferMb}M",
    '-probesize', '32',
    '-analyzeduration', '0',
    '-i', $previewDevice,
    '-noborder',
    '-x', $previewWidth,
    '-y', $previewHeight,
    '-left', $previewLeft,
    '-top', 0,
    '-window_title', '"PhoneCam preview"',
    '-vf', "setpts=N/30/TB,scale=${previewWidth}:${previewHeight}",
    '-fflags', 'nobuffer',
    '-flags', 'low_delay',
    '-sync', 'video',
    '-framedrop'
  ) -PassThru -RedirectStandardError $previewError -RedirectStandardOutput $previewOutput
  Start-Sleep -Seconds $WarmupSeconds
  if ($previewProcess.HasExited) {
    $tail = if (Test-Path $previewError) { Get-Content $previewError -Tail 20 | Out-String } else { '' }
    throw "ffplay exited during startup (exit code $($previewProcess.ExitCode)).`n$tail"
  }

  $timestamps = @()
  for ($i = 1; $i -le $Shots; $i++) {
    $now = Get-Date
    $bmp = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bmp)
    try {
      $graphics.CopyFromScreen($screen.X, $screen.Y, 0, 0, $bmp.Size)
      $path = Join-Path $outDir ('shot-{0:D2}.png' -f $i)
      $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
      $graphics.Dispose()
      $bmp.Dispose()
    }
    $timestamps += ('shot-{0:D2} {1}' -f $i, $now.ToString('o'))
    Start-Sleep -Milliseconds $ShotIntervalMs
  }
  $timestamps | Out-File (Join-Path $outDir 'shots.txt') -Encoding utf8

  @(
    "created=$stamp",
    "commit=$(git rev-parse HEAD)",
    "branch=$(git branch --show-current)",
    "device=$(adb shell getprop ro.product.model)",
    "android=$(adb shell getprop ro.build.version.release)",
    "screen=$($screen.Width)x$($screen.Height)",
    "timer_left_width=$leftWidth",
    "preview=${previewWidth}x${previewHeight}+${previewLeft}+0",
    "warmup_s=$WarmupSeconds",
    "shots=$Shots",
    "shot_interval_ms=$ShotIntervalMs"
  ) | Out-File (Join-Path $outDir 'metadata.txt') -Encoding utf8

  Write-Output $outDir
}
finally {
  foreach ($process in @($previewProcess, $hostProcess, $timerProcess)) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
  }
  Remove-Item Env:PHONECAM_BENCH -ErrorAction SilentlyContinue
}
