param(
  [ValidateRange(1,5)][int]$Repeats = 3,
  [ValidateRange(15,120)][int]$DurationSeconds = 30,
  [Parameter(Mandatory=$true)][string]$Inventory
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$report = Get-Content $Inventory -Raw | ConvertFrom-Json
$accepted = @($report.probes | Where-Object accepted | ForEach-Object { $_.case })
$requested = @('default','latency0','priority0','max_b_frames0','intra_refresh30','idr2','idr5')
$cases = @($requested | Where-Object { $_ -in $accepted })
if ('default' -notin $cases) { throw 'No accepted default in inventory' }
if (Get-Process phonecam-host -ErrorAction SilentlyContinue) { throw 'Another host is running' }
$cohort = 'sweep-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$output = Join-Path $PSScriptRoot "runs\$cohort"
New-Item -ItemType Directory -Path $output | Out-Null
Copy-Item $Inventory (Join-Path $output 'inventory.json')
$rows = [System.Collections.Generic.List[object]]::new()
$failed = @{}
$random = [System.Random]::new(20261022)
for ($round = 1; $round -le $Repeats; $round++) {
  # Reproducible interleaving: don't put all default runs before all candidates.
  $order = @($cases | Sort-Object { $random.Next() })
  if ($round -eq 1) { $order = @('default') + @($order | Where-Object { $_ -ne 'default' }) }
  foreach ($case in $order) {
    if ($failed.ContainsKey($case)) { continue }
    $label = "$cohort-r$round-$case"
    $status = 'passed'
    $errorText = ''
    try {
      & (Join-Path $PSScriptRoot 'run-b0.ps1') -DurationSeconds $DurationSeconds `
        -Label $label -EncoderCase $case -SkipInstall -ResumeStopped
    } catch {
      $status = 'failed'
      $errorText = $_.ToString()
      $failed[$case] = $true
    }
    $directory = Get-ChildItem (Join-Path $PSScriptRoot 'runs') -Directory -Filter "*-$label" |
      Select-Object -Last 1
    $rows.Add([pscustomobject]@{
      round = $round; encoder_case = $case; status = $status
      run = $directory.FullName; error = $errorText
    })
    $rows | Export-Csv (Join-Path $output 'run-order.csv') -NoTypeInformation
    if ($case -eq 'default' -and $status -eq 'failed') { throw "Default failed; abort cohort. $errorText" }
  }
}
python (Join-Path $PSScriptRoot 'compare_encoder.py') $output |
  Tee-Object -FilePath (Join-Path $output 'comparison.json')
if ($LASTEXITCODE -ne 0) { throw 'Cohort analysis failed' }
Write-Output "cohort=$output"
