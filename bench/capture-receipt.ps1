param(
  [ValidateRange(1,60)][int]$DurationSeconds = 10,
  [ValidateRange(0,300)][int]$SaveEvery = 30,
  [ValidateRange(0,250)][int]$ReadDelayMs = 0,
  [switch]$ShowTimer
)

# The caller owns the host and the phone session. Never start, stop or change them here.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$binary = Join-Path $root 'windows\build\tools\media_bench\Debug\phonecam-media-bench.exe'
if (-not (Test-Path $binary)) { throw 'Build the phonecam-media-bench target first' }
if (-not (Get-Process phonecam-host -ErrorAction SilentlyContinue)) { throw 'Start the intended host/session first' }
$run = Join-Path $PSScriptRoot ('e2e\receipt-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$capture = Join-Path $run 'capture'
$timer = $null
$consumer = $null
try {
  if ($ShowTimer) {
    Add-Type -AssemblyName System.Windows.Forms
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $timer = Start-Process powershell -ArgumentList @(
      '-NoProfile', '-File', "`"$(Join-Path $PSScriptRoot 'timer-window.ps1')`"",
      '-Width', [int]($screen.Width / 2), '-Height', $screen.Height,
      '-UnixMilliseconds'
    ) -PassThru -WindowStyle Minimized
    Start-Sleep -Seconds 2
  }
  $consumer = Start-Process $binary -ArgumentList @("`"$capture`"", $DurationSeconds, $SaveEvery, $ReadDelayMs) `
    -PassThru -RedirectStandardOutput (Join-Path $run 'consumer.out.log') `
    -RedirectStandardError (Join-Path $run 'consumer.err.log')
  if (-not $consumer.WaitForExit(($DurationSeconds + 20) * 1000)) { throw 'Consumer timed out; capture is invalid' }
  if ($consumer.ExitCode -ne 0) {
    Get-Content (Join-Path $run 'consumer.err.log') | Write-Output
    throw "Consumer failed: $($consumer.ExitCode); logs at $run"
  }
  python (Join-Path $PSScriptRoot 'analyze_receipt.py') $capture |
    Tee-Object -FilePath (Join-Path $run 'summary.json')
  if ($LASTEXITCODE -ne 0) { throw "Invalid receipt capture: $run" }
  Get-FileHash $binary, 'C:\ProgramData\PhoneCam\phonecam-vcam.dll' |
    Export-Csv (Join-Path $run 'artifact-hashes.csv') -NoTypeInformation
  @(
    "commit=$(git -C $root rev-parse HEAD)",
    'consumer=native MF SourceReader (not DirectShow/ffplay)',
    'transport=caller-owned; record with the host run',
    "show_timer=$ShowTimer"
  ) | Out-File (Join-Path $run 'metadata.txt') -Encoding utf8
  Write-Output "run=$run"
} finally {
  foreach ($process in @($consumer, $timer)) {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
  }
}
