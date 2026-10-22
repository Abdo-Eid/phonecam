param([ValidateRange(1,5)][int]$Repeats = 3, [ValidateRange(15,120)][int]$DurationSeconds = 30)
$ErrorActionPreference = 'Stop'
$cohort = 'profiles-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$output = Join-Path $PSScriptRoot "runs\$cohort"
New-Item -ItemType Directory -Path $output | Out-Null
$profiles = @('default','720p6m','1080p4m','1080p8m','1080p12m')
$rows = [System.Collections.Generic.List[object]]::new()
$failed = @{}
$random = [System.Random]::new(20261023)
for ($round = 1; $round -le $Repeats; $round++) {
  $order = @($profiles | Sort-Object { $random.Next() })
  if ($round -eq 1) { $order = @('default') + @($order | Where-Object { $_ -ne 'default' }) }
  foreach ($profile in $order) {
    if ($failed.ContainsKey($profile)) { continue }
    $label = "$cohort-r$round-$profile"
    $status = 'passed'; $errorText = ''
    try {
      & (Join-Path $PSScriptRoot 'run-b0.ps1') -Label $label -DurationSeconds $DurationSeconds `
        -SkipInstall -ResumeStopped -EncoderCase default -MediaProfile $profile
    } catch {
      $status = 'failed'; $errorText = $_.ToString(); $failed[$profile] = $true
    }
    $directory = Get-ChildItem (Join-Path $PSScriptRoot 'runs') -Directory -Filter "*-$label" | Select-Object -Last 1
    $rows.Add([pscustomobject]@{ round = $round; configuration = $profile; encoder_case = 'default'
      status = $status; run = $directory.FullName; error = $errorText })
    $rows | Export-Csv (Join-Path $output 'run-order.csv') -NoTypeInformation
    if ($profile -eq 'default' -and $status -eq 'failed') { throw "Default failed; abort cohort. $errorText" }
  }
}
python (Join-Path $PSScriptRoot 'compare_encoder.py') $output |
  Tee-Object -FilePath (Join-Path $output 'comparison.json')
if ($LASTEXITCODE -ne 0) { throw 'Profile analysis failed' }
Write-Output "cohort=$output"
