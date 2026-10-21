param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [ValidateSet('Stop', 'Resume')][string]$Action = 'Stop',
    [string]$LogPath = ''
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class StopUi {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
}
'@
function Wait-Until([scriptblock]$Condition, [string]$Failure, [int]$Seconds = 30) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do { if (& $Condition) { return }; Start-Sleep -Milliseconds 150 } while ((Get-Date) -lt $deadline)
    throw $Failure
}
$tray = [StopUi]::FindWindowExW([IntPtr](-3), 0, 'PhoneCamTrayWindow', $null)
$owner = 0u
[void][StopUi]::GetWindowThreadProcessId($tray, [ref]$owner)
if ($owner -ne $ProcessId) { throw 'Refusing to drive another host process' }
function Tray-Command([int]$Id) {
    [void][StopUi]::PostMessageW($tray, 0x8001, 0, 0x205)
    Start-Sleep -Milliseconds 250
    [void][StopUi]::PostMessageW($tray, 0x1F, 0, 0)
    Start-Sleep -Milliseconds 150
    [void][StopUi]::PostMessageW($tray, 0x111, $Id, 0)
}
$root = Split-Path $PSScriptRoot -Parent
$log = if ($LogPath) { $LogPath } else { Join-Path $root 'windows\build\tray_gui.err.log' }
$start = @(Get-Content $log).Count
$before = (adb shell run-as io.github.abdoeid.phonecam cat shared_prefs/phonecam_session.xml 2>$null) -join "`n"
if ($Action -eq 'Resume' -and $before -notmatch 'name="stopped" value="true"') { throw 'Resume test requires a stopped phone' }
if ($Action -eq 'Stop' -and $before -match 'name="stopped" value="true"') { throw 'Stop test requires a streaming phone' }
# The tray's Start/Stop entry is only offered once the control session is ready.
Wait-Until { (@(Get-Content $log) | Where-Object { $_ -match 'Connection: ' } | Select-Object -Last 1) -match 'Connection: (Streaming via|Stopped|Paired; waiting|Connecting - waiting)' } 'No live session to drive'
# The first three dynamic actions are Wi-Fi, USB, Disconnect; the next is Start/Stop.
Tray-Command 203
if ($Action -eq 'Resume') {
    Wait-Until { @(Get-Content $log | Select-Object -Skip $start) -match 'Connection: Streaming via' } 'Explicit tray Start did not resume'
    'PASS explicit tray Start resumes remembered connection'
} else {
    Wait-Until { @(Get-Content $log | Select-Object -Skip $start) -match 'Connection: Stopped' } 'Tray Stop did not latch'
    Start-Sleep -Seconds 12
    $recent = @(Get-Content $log | Select-Object -Skip $start)
    if ($recent -match 'Connection: Streaming via') { throw 'Watchdog restarted a stopped stream' }
    $prefs = (adb shell run-as io.github.abdoeid.phonecam cat shared_prefs/phonecam_session.xml) -join "`n"
    if ($prefs -notmatch 'name="stopped" value="true"') { throw 'Phone did not persist stopped intent' }
    'PASS Stop persists on the phone through four watchdog intervals'
    $blackPath = Join-Path $root 'windows\build\tray-stopped.png'
    ffmpeg -hide_banner -loglevel error -y -rtbufsize 128M -f dshow -i 'video=PhoneCam (Windows Virtual Camera)' -frames:v 1 -update 1 $blackPath
    if ($LASTEXITCODE -ne 0) { throw 'Stopped virtual camera unavailable' }
    Add-Type -AssemblyName System.Drawing
    $black = [Drawing.Bitmap]::new($blackPath)
    try {
        foreach ($x in @(0, [int]($black.Width / 2), ($black.Width - 1))) {
            foreach ($y in @(0, [int]($black.Height / 2), ($black.Height - 1))) {
                $p = $black.GetPixel($x, $y)
                if ($p.R -gt 20 -or $p.G -gt 20 -or $p.B -gt 20) { throw 'Stopped output retained a live frame' }
            }
        }
    } finally { $black.Dispose() }
    'PASS stopped virtual camera remains available and black'
    Tray-Command 200
    Wait-Until { [StopUi]::FindWindowW('#32770', 'PhoneCam - Scan to connect') -ne 0 } 'QR window did not open'
    $qr = [StopUi]::FindWindowW('#32770', 'PhoneCam - Scan to connect')
    [void][StopUi]::SendMessageW([StopUi]::GetDlgItem($qr, 1105), 0xF5, 0, 0)
    Wait-Until { [StopUi]::FindWindowW('#32770', 'PhoneCam - Scan to connect') -eq 0 } 'Remembered reconnect did not dismiss its window'
    Start-Sleep -Seconds 8
    $recent = @(Get-Content $log | Select-Object -Skip $start)
    if ($recent -match 'Connection: Streaming via') { throw 'Remembered reconnect undid Stop' }
    'PASS remembered reconnect closes its window and preserves Stop'
}
if (@(Select-String -Path $log -Pattern 'Virtual camera started').Count -ne 1) { throw 'Virtual camera re-registered' }
if ((Get-Process -Id $ProcessId).HasExited) { throw 'Host exited' }
'PASS one host and unchanged virtual-camera registration'
