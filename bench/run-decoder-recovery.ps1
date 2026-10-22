# Offline synthetic recovery through the same MF decoder implementation used by
# the host. Do not run while a timed phone cohort is active (CPU load confound).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (Get-Process phonecam-host -ErrorAction SilentlyContinue) { throw 'Wait until live measurements finish' }
$run = Join-Path $PSScriptRoot ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-E9-synthetic-recovery')
New-Item -ItemType Directory -Path $run | Out-Null
cmake -S (Join-Path $root 'windows') -B (Join-Path $root 'windows/build')
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
cmake --build (Join-Path $root 'windows/build') --target phonecam-decoder-recovery --config Debug
if ($LASTEXITCODE -ne 0) { throw 'Recovery tool build failed' }
$corpus = Join-Path $run 'synthetic.h264'
& ffmpeg -hide_banner -loglevel error -nostdin -f lavfi -i 'testsrc2=size=1920x1080:rate=30' `
  -frames:v 180 -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline `
  -level:v 4.0 -b:v 6M -g 30 -keyint_min 30 -sc_threshold 0 -bf 0 `
  -x264-params 'aud=1:repeat-headers=1' -pix_fmt yuv420p -f h264 $corpus
if ($LASTEXITCODE -ne 0) { throw 'Synthetic corpus generation failed' }
$binary = Join-Path $root 'windows/build/tools/media_bench/Debug/phonecam-decoder-recovery.exe'
& $binary $corpus (Join-Path $run 'replay') 2> (Join-Path $run 'decoder.err.log') |
  Out-File (Join-Path $run 'decoder.out.log') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "Decoder recovery replay failed; diagnostics in $run" }
Get-FileHash $binary,$corpus | Export-Csv (Join-Path $run 'artifact-hashes.csv') -NoTypeInformation
Get-Content (Join-Path $run 'replay/summary.csv')
Write-Output "run=$run"
