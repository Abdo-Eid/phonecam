param(
    [Parameter(Mandatory=$true)][string]$Label,
    [int]$DurationSeconds = 60,
    [int]$WarmupSeconds = 15
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'windows\build\host\Release\phonecam-host.exe'
$directory = Join-Path $root 'windows\build\link-comparison'
New-Item -ItemType Directory -Force $directory | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$prefix = Join-Path $directory "$Label-$stamp"
$stdout = "$prefix.out.log"
$stderr = "$prefix.err.log"

function Get-PhoneAddress {
    $text = (adb shell ip -4 addr show wlan0) -join "`n"
    if ($text -notmatch 'inet (\d+\.\d+\.\d+\.\d+)/') { throw 'Phone Wi-Fi address unavailable' }
    return $Matches[1]
}

function Get-RadioInfo {
    # Do not retain SSID, BSSID, MAC, or the rest of the private Wi-Fi dump.
    $text = (adb shell dumpsys wifi | Select-String 'mWifiInfo|WifiInfo:') -join "`n"
    $result = @{}
    foreach ($field in @('RSSI', 'Frequency', 'Link speed', 'Tx Link speed', 'Rx Link speed')) {
        if ($text -match "(?i)$([regex]::Escape($field)):\s*(-?\d+)") {
            $result[$field] = [int]$Matches[1]
        }
    }
    return $result
}

function Read-Lines([string]$Path) {
    if (Test-Path $Path) { return @(Get-Content $Path) }
    return @()
}

function Quantile($Values, [double]$Fraction) {
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Max(0, [Math]::Ceiling($Fraction * $sorted.Count) - 1)
    return $sorted[$index]
}

$target = Get-PhoneAddress
$radio = Get-RadioInfo
$route = Find-NetRoute -RemoteIPAddress $target | Select-Object -Last 1
$previousSecret = $env:PHONECAM_SECRET
$process = $null
try {
    if (-not $env:PHONECAM_SECRET) {
        $preferences = (adb shell 'run-as io.github.abdoeid.phonecam cat shared_prefs/phonecam_pairing.xml') -join "`n"
        $env:PHONECAM_SECRET = ([xml]$preferences).map.string.'#text'
        if (-not $env:PHONECAM_SECRET) { throw 'Set PHONECAM_SECRET or connect the debuggable phone over USB' }
    }
    # Only replace this repository's running host, never an unrelated installed executable.
    Get-Process phonecam-host -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -like "$root\windows\build\*"
    } | Stop-Process
    Start-Sleep -Seconds 2
    $process = Start-Process $exe -ArgumentList '--rtp',$target -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru
    Start-Sleep -Seconds $WarmupSeconds
    $initial = @(Read-Lines $stderr)
    if (-not ($initial -match 'LiveVideoBridge: \d+ packets, [1-9]\d* frames')) {
        throw 'No decoded frames after warmup; do not treat this run as a valid link benchmark'
    }
    $initialRtp = @($initial -match 'RtpVideoTransport: \d+ datagrams') | Select-Object -Last 1
    $initialDatagrams = 0L
    $initialLoss = 0L
    if ($initialRtp -match 'RtpVideoTransport: (\d+) datagrams, .*?, \d+ access units, (\d+) lost') {
        $initialDatagrams = [long]$Matches[1]
        $initialLoss = [long]$Matches[2]
    }
    $lineCount = $initial.Count
    $reports = [System.Collections.Generic.List[object]]::new()
    $pings = [System.Collections.Generic.List[object]]::new()
    $ping = [System.Net.NetworkInformation.Ping]::new()
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        while ($clock.Elapsed.TotalSeconds -lt $DurationSeconds) {
            $at = $clock.Elapsed.TotalSeconds
            try {
                $reply = $ping.Send($target, 1000, [byte[]]::new(32))
                $pings.Add([pscustomobject]@{
                    seconds = [Math]::Round($at, 3)
                    status = $reply.Status.ToString()
                    rttMs = $(if ($reply.Status -eq 'Success') { $reply.RoundtripTime } else { $null })
                })
            } catch {
                $pings.Add([pscustomobject]@{ seconds = [Math]::Round($at, 3); status = 'Error'; rttMs = $null })
            }
            $lines = @(Read-Lines $stderr)
            for ($i = $lineCount; $i -lt $lines.Count; ++$i) {
                if ($lines[$i] -match 'LiveVideoBridge: (\d+) packets, (\d+) frames') {
                    $reports.Add([pscustomobject]@{
                        seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
                        packets = [long]$Matches[1]
                        frames = [long]$Matches[2]
                        diagnostic = $lines[$i]
                    })
                }
            }
            $lineCount = $lines.Count
            Start-Sleep -Milliseconds 100
        }
    } finally {
        $ping.Dispose()
    }
    $successes = @($pings | Where-Object status -eq 'Success')
    $rtts = @($successes | ForEach-Object rttMs)
    $finalRtp = @(Read-Lines $stderr | Where-Object { $_ -match 'RtpVideoTransport: \d+ datagrams' }) | Select-Object -Last 1
    $datagrams = $null
    $loss = $null
    if ($finalRtp -match 'RtpVideoTransport: (\d+) datagrams, .*?, \d+ access units, (\d+) lost') {
        $datagrams = [long]$Matches[1] - $initialDatagrams
        $loss = [long]$Matches[2] - $initialLoss
    }
    $fps = $null
    if ($reports.Count -ge 2) {
        $first = $reports[0]
        $last = $reports[$reports.Count - 1]
        $fps = [Math]::Round(($last.frames - $first.frames) / ($last.seconds - $first.seconds), 2)
    }
    $result = [ordered]@{
        label = $Label
        recordedAt = (Get-Date).ToString('o')
        phoneAddress = $target
        phoneAddressAtEnd = Get-PhoneAddress
        pcInterface = $route.InterfaceAlias
        radioAtStart = $radio
        radioAtEnd = Get-RadioInfo
        transport = 'RTP/H264, plaintext, authenticated control'
        requestedVideo = '1920x1080 30fps 6000000bps'
        warmupSeconds = $WarmupSeconds
        measurementSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
        pingPayloadBytes = 32
        pingTimeoutMs = 1000
        pingIntervalSleepMs = 100
        pingSent = $pings.Count
        pingReceived = $successes.Count
        pingFailed = $pings.Count - $successes.Count
        rttMinMs = Quantile $rtts 0
        rttMedianMs = Quantile $rtts 0.5
        rttP95Ms = Quantile $rtts 0.95
        rttP99Ms = Quantile $rtts 0.99
        rttMaxMs = Quantile $rtts 1
        decodedFps = $fps
        observedRtpDatagrams = $datagrams
        observedRtpLossEvents = $loss
        encoderStatsAtEnd = @(Read-Lines $stdout | Where-Object { $_ -match '^Stats fps=' }) | Select-Object -Last 1
        timingCaveat = 'ICMP network round-trip, not one-way or absolute video latency; host relative-delay values are diagnostics only. Frame rate uses observed 5-second log reports, not every frame.'
        frameReports = $reports.ToArray()
        pingSamples = $pings.ToArray()
    }
    if ($result.phoneAddressAtEnd -ne $target) { throw 'Phone switched networks during measurement; run invalid' }
    $result | ConvertTo-Json -Depth 8 | Set-Content "$prefix.json" -Encoding utf8
    $result.GetEnumerator() | Where-Object { $_.Key -notin @('frameReports','pingSamples') } | Format-Table -AutoSize
    "Saved measurement: $prefix.json"
    "Host left running: PID $($process.Id)"
} finally {
    $env:PHONECAM_SECRET = $previousSecret
}
