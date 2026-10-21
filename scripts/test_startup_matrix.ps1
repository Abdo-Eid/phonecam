param(
    [string]$Serial = '17cdb62f',
    [switch]$IncludeCleanData,
    [switch]$ManualPermissions,
    [ValidateSet('Keep', 'Cache', 'Data')][string[]]$ResetModes = @(),
    [ValidateSet('Wifi', 'Usb')][string[]]$Transports = @('Wifi', 'Usb'),
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$hostExe = Join-Path $root 'windows\build\host\Release\phonecam-host.exe'
$stateTool = Join-Path $PSScriptRoot 'phonecam_test_state.py'
$adbExe = (Get-Command adb -CommandType Application | Select-Object -First 1).Source
$package = 'io.github.abdoeid.phonecam'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root ('windows\build\startup-matrix\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$backup = Join-Path $env:LOCALAPPDATA ('Temp\opencode\phonecam-startup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dpapi')
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MatrixUi {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr h, uint m, IntPtr w, string s);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
}
'@
function Adb {
    $result = & $adbExe -s $Serial @args 2>&1
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($args[0..([Math]::Min(2,$args.Count-1))] -join ' '): $result" }
    return $result
}
function State-Tool([string]$Action) {
    [string[]]$extra = @()
    if ($Action -eq 'clear-data') { $extra += '--allow-data-reset' }
    if ($ManualPermissions) { $extra += '--manual-permissions' }
    & python $stateTool $Action --serial $Serial --backup $backup @extra
    if ($LASTEXITCODE -ne 0) { throw "State operation failed: $Action. Backup: $backup" }
}
function Wait-Until([scriptblock]$Condition, [string]$Failure, [int]$Seconds = 25) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    throw $Failure
}
function Tray {
    $window = [MatrixUi]::FindWindowExW([IntPtr](-3), 0, 'PhoneCamTrayWindow', $null)
    if ($window -eq 0) { return [IntPtr]::Zero }
    $owner = 0u
    [void][MatrixUi]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($script:hostProcess -and $owner -ne $script:hostProcess.Id) { throw 'Unexpected tray owner' }
    return $window
}
function Tray-Command([int]$Id) {
    $window = Tray
    if ($window -eq 0) { throw 'No tray window' }
    [void][MatrixUi]::PostMessageW($window, 0x8001, 0, 0x205)
    Start-Sleep -Milliseconds 200
    [void][MatrixUi]::PostMessageW($window, 0x1f, 0, 0)
    Start-Sleep -Milliseconds 100
    [void][MatrixUi]::PostMessageW($window, 0x111, $Id, 0)
}
function Stop-Host {
    # Only this repository's Release host, never another installation/process.
    foreach ($process in @(Get-Process phonecam-host -ErrorAction SilentlyContinue | Where-Object Path -eq $hostExe)) {
        $window = [MatrixUi]::FindWindowExW([IntPtr](-3), 0, 'PhoneCamTrayWindow', $null)
        $owner = 0u
        [void][MatrixUi]::GetWindowThreadProcessId($window, [ref]$owner)
        if ($owner -eq $process.Id) { [void][MatrixUi]::PostMessageW($window, 0x10, 0, 0) }
        if (!$process.WaitForExit(8000)) {
            if ($script:hostProcess -and $script:hostProcess.Id -eq $process.Id) {
                # This script created the process; ensure cleanup can restore
                # credentials without a still-running test host overwriting them.
                $process.Kill()
                $process.WaitForExit()
            } else { throw 'Existing tray host did not exit cleanly; refusing to force-kill it' }
        }
    }
    $script:hostProcess = $null
}
function Start-Host([string]$Name) {
    $script:log = Join-Path $OutputDirectory ($Name + '.err.log')
    $script:hostProcess = Start-Process $hostExe -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory ($Name + '.out.log')) -RedirectStandardError $script:log
    Wait-Until { (Tray) -ne 0 } 'Tray did not start'
}
function Open-App {
    Adb shell am start -W -n "$package/.MainActivity" | Out-Null
    Wait-Until { ((Adb shell dumpsys activity services $package) -join "`n") -match 'ServiceRecord' } 'Phone service did not start'
}
function Log-Text { if (Test-Path $script:log) { return (Get-Content $script:log) -join "`n" }; return '' }
function Manual-Wifi {
    Tray-Command 200
    Wait-Until { [MatrixUi]::FindWindowW('#32770', 'PhoneCam - Scan to connect') -ne 0 } 'QR dialog did not open'
    $qr = [MatrixUi]::FindWindowW('#32770', 'PhoneCam - Scan to connect')
    [void][MatrixUi]::SendMessageW([MatrixUi]::GetDlgItem($qr, 1104), 0xf5, 0, 0)
    Wait-Until { [MatrixUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi') -ne 0 } 'Code dialog did not open'
    $dialog = [MatrixUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi')
    $list = [MatrixUi]::GetDlgItem($dialog, 1001)
    Wait-Until { [MatrixUi]::SendMessageW($list, 0x18b, 0, 0).ToInt64() -gt 0 } 'No phone discovered in tray'
    $xml = (Adb shell run-as $package cat shared_prefs/phonecam_pairing.xml) -join "`n"
    $code = ([xml]$xml).map.string.'#text'
    if (!$code) { throw 'Phone pairing code unavailable' }
    [void][MatrixUi]::SendMessageW($list, 0x186, 0, 0)
    [void][MatrixUi]::SendMessageW($dialog, 0x111, (1001 -bor (1 -shl 16)), $list)
    [void][MatrixUi]::SetText([MatrixUi]::GetDlgItem($dialog, 1003), 0xc, 0, $code)
    [void][MatrixUi]::SendMessageW([MatrixUi]::GetDlgItem($dialog, 1007), 0xf5, 0, 0)
    Wait-Until { [MatrixUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi') -eq 0 } 'Code connection did not dismiss its dialog'
}

$results = [Collections.Generic.List[object]]::new()
$saved = $false
try {
    Stop-Host
    State-Tool backup
    $saved = $true
    $modes = @('Keep', 'Cache')
    if ($IncludeCleanData) { $modes += 'Data' }
    if ($ResetModes.Count) { $modes = $ResetModes }
    if ('Data' -in $modes -and !$IncludeCleanData) { throw 'Data cases require -IncludeCleanData' }
    foreach ($reset in $modes) {
        foreach ($transport in $Transports) {
            foreach ($order in @('AppFirst', 'TrayFirst')) {
                $name = "$reset-$transport-$order"
                $timer = [Diagnostics.Stopwatch]::StartNew()
                try {
                    Stop-Host
                    Adb shell am force-stop $package | Out-Null
                    if ($reset -eq 'Cache') { State-Tool clear-cache }
                    if ($reset -eq 'Data') {
                        State-Tool clear-data
                        # Clean app data remains blank. Either grant for unattended
                        # capture or let the person use the real permission prompt.
                        if (!$ManualPermissions) { Adb shell pm grant $package android.permission.CAMERA | Out-Null }
                    }
                    if ($reset -ne 'Data') {
                        if (!(Test-Path 'HKCU:\Software\PhoneCam\Connection')) {
                            New-Item 'HKCU:\Software\PhoneCam\Connection' | Out-Null
                        }
                        Set-ItemProperty 'HKCU:\Software\PhoneCam\Connection' UseWifi $(if ($transport -eq 'Wifi') { '1' } else { '0' })
                    }
                    if ($order -eq 'AppFirst') {
                        Open-App
                        Start-Sleep -Seconds 2
                        Start-Host $name
                    } else {
                        Start-Host $name
                        Start-Sleep -Seconds 2
                        if ((Log-Text) -match 'Connection: Streaming via') { throw 'Video before phone app launched' }
                        Open-App
                    }
                    if ($reset -eq 'Data' -and $ManualPermissions) {
                        "ACTION $name`: tap Grant access, then Allow on the phone"
                        Wait-Until { ((Adb shell dumpsys package $package) -join "`n") -match 'android.permission.CAMERA: granted=true' } 'Camera permission was not allowed on the phone' 90
                    }
                    if ($transport -eq 'Wifi' -and $reset -eq 'Data') { Manual-Wifi }
                    Wait-Until { (Log-Text) -match 'Connection: (Stopped|Streaming via)' } 'No authenticated ready/streaming state'
                    $lastConnection = Get-Content $script:log | Where-Object { $_ -match 'Connection: ' } | Select-Object -Last 1
                    if ($lastConnection -match 'Stopped') { Tray-Command 203 }
                    $expected = if ($transport -eq 'Wifi') { 'Wi-Fi \(low latency\)' } else { 'USB \(ADB\)' }
                    Wait-Until { (Log-Text) -match "Connection: Streaming via $expected" } "No decoded $transport video"
                    # Streaming is only published after decoded frames. A brief sample also
                    # confirms this goes through the normal registered virtual webcam.
                    & ffmpeg -hide_banner -loglevel error -f dshow -i 'video=PhoneCam (Windows Virtual Camera)' -frames:v 1 -f null NUL 2> (Join-Path $OutputDirectory ($name + '.camera.log'))
                    if ($LASTEXITCODE -ne 0) { throw 'DirectShow endpoint did not deliver a frame' }
                    if (@(Get-Process phonecam-host -ErrorAction SilentlyContinue).Count -ne 1) { throw 'Duplicate host process' }
                    $detail = if ($transport -eq 'Usb') { 'Automatic USB (ADB)' }
                              elseif ($reset -eq 'Data') { 'Fresh manual-code connection' }
                              else { 'Remembered QR pairing' }
                    $results.Add([pscustomobject]@{ Case=$name; Result='PASS'; Seconds=[Math]::Round($timer.Elapsed.TotalSeconds,1); Detail=$detail })
                    "PASS $name"
                } catch {
                    $results.Add([pscustomobject]@{ Case=$name; Result='FAIL'; Seconds=[Math]::Round($timer.Elapsed.TotalSeconds,1); Detail=$_.Exception.Message })
                    "FAIL $name`: $($_.Exception.Message)"
                }
                $results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutputDirectory 'results.json')
            }
        }
    }
} finally {
    Stop-Host
    if ($saved) {
        State-Tool restore
        Start-Host 'restored'
        Open-App
    }
    "Encrypted recovery backup: $backup"
}
$results | Format-Table -AutoSize
if (@($results | Where-Object Result -eq 'FAIL').Count) { exit 1 }
