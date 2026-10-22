param([ValidateRange(1,3)][int]$Repeats = 3, [ValidateRange(5,30)][int]$DurationSeconds = 15, [switch]$ResumeStopped)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (Get-Process phonecam-host -ErrorAction SilentlyContinue) { throw 'Another host is running' }
$output = Join-Path $PSScriptRoot ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-E6-consumer-stress')
New-Item -ItemType Directory -Path $output | Out-Null
$hostProcess = $null
$previousBench = $env:PHONECAM_BENCH
$previousProfile = $env:PHONECAM_BENCH_MEDIA
$rows = [System.Collections.Generic.List[object]]::new()
try {
  # Keep one camera session alive throughout; no forced restart between consumers.
  adb shell am start -n io.github.abdoeid.phonecam/.MainActivity | Out-File (Join-Path $output 'launch.txt')
  if ($LASTEXITCODE -ne 0) { throw 'Could not launch the phone app' }
  $env:PHONECAM_BENCH = '1'; $env:PHONECAM_BENCH_MEDIA = 'default'
  $hostProcess = Start-Process (Join-Path $root 'windows/build/host/Debug/phonecam-host.exe') `
    -ArgumentList '--adb' -PassThru -RedirectStandardOutput (Join-Path $output 'host.out.log') `
    -RedirectStandardError (Join-Path $output 'host.err.log')
  Start-Sleep -Seconds 8
  if ($hostProcess.HasExited) { throw 'Host exited' }
  if ($ResumeStopped -and (Select-String -Path (Join-Path $output 'host.err.log') -Pattern 'Connection: Stopped' -Quiet)) {
    & (Join-Path $root 'scripts/test_stopped_gui.ps1') -ProcessId $hostProcess.Id -Action Resume `
      -LogPath (Join-Path $output 'host.err.log')
    Start-Sleep -Seconds 8
  }
  if (-not (Select-String -Path (Join-Path $output 'host.err.log') -Pattern 'Connection: Streaming via' -Quiet)) {
    throw 'Phone did not start streaming; unlock/open the app before retrying'
  }
  for ($round = 1; $round -le $Repeats; $round++) {
    $delays = if ($round % 2) { @(0,100) } else { @(100,0) }
    foreach ($delay in $delays) {
      $status = 'passed'; $errorText = ''; $capture = ''
      try {
        $messages = & (Join-Path $PSScriptRoot 'capture-receipt.ps1') `
          -DurationSeconds $DurationSeconds -SaveEvery 0 -ReadDelayMs $delay
        $messages | Write-Output
        $capture = ($messages | Where-Object { $_ -is [string] -and $_.StartsWith('run=') } | Select-Object -Last 1).Substring(4)
      } catch { $status = 'failed'; $errorText = $_.ToString() }
      $rows.Add([pscustomobject]@{ round=$round; read_delay_ms=$delay; status=$status; run=$capture; error=$errorText })
      $rows | Export-Csv (Join-Path $output 'run-order.csv') -NoTypeInformation
      if ($status -ne 'passed') { throw "Consumer stress failed; diagnostics retained. $errorText" }
    }
  }
  Get-FileHash (Join-Path $root 'windows/build/host/Debug/phonecam-host.exe'), `
    (Join-Path $root 'windows/build/tools/media_bench/Debug/phonecam-media-bench.exe'), `
    'C:\ProgramData\PhoneCam\phonecam-vcam.dll' |
    Export-Csv (Join-Path $output 'artifact-hashes.csv') -NoTypeInformation
  if (-not (Select-String -Path (Join-Path $output 'host.out.log') -SimpleMatch '1920x1080@30 bitrate=6000000' -Quiet)) {
    throw 'Default media settings not confirmed'
  }
  Write-Output "cohort=$output"
} finally {
  if ($hostProcess -and -not $hostProcess.HasExited) { Stop-Process -Id $hostProcess.Id -Force }
  $env:PHONECAM_BENCH = $previousBench; $env:PHONECAM_BENCH_MEDIA = $previousProfile
}
