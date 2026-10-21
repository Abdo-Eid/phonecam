param([Parameter(Mandatory=$true)][int]$ProcessId, [switch]$TestOffline)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class PhoneCamUi {
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr hwnd, uint msg, IntPtr w, string text);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr ReadText(IntPtr hwnd, uint msg, IntPtr w, StringBuilder text);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
}
'@
function Wait-Until([scriptblock]$Condition, [string]$Failure, [int]$Seconds = 25) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do { if (& $Condition) { return }; Start-Sleep -Milliseconds 200 } while ((Get-Date) -lt $deadline)
    throw $Failure
}
function Text([IntPtr]$Window) {
    $buffer = [Text.StringBuilder]::new(512)
    [void][PhoneCamUi]::GetWindowTextW($Window, $buffer, $buffer.Capacity)
    return $buffer.ToString()
}
function Status { return Text ([PhoneCamUi]::GetDlgItem($script:dialog, 1006)) }
function Click([int]$Id) { [void][PhoneCamUi]::SendMessageW([PhoneCamUi]::GetDlgItem($script:dialog, $Id), 0x00F5, 0, 0) }
function Tray-Command([int]$Id) {
    # Build the real context menu before invoking its dynamic command, as an actual tray click does.
    [void][PhoneCamUi]::PostMessageW($script:tray, 0x8001, 0, 0x0205)
    Start-Sleep -Milliseconds 250
    [void][PhoneCamUi]::PostMessageW($script:tray, 0x001F, 0, 0)
    Start-Sleep -Milliseconds 150
    [void][PhoneCamUi]::PostMessageW($script:tray, 0x0111, $Id, 0)
}
function Open-Manual {
    Tray-Command 200
    Wait-Until { $script:qr = [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Scan to connect'); $script:qr -ne 0 } 'QR tray dialog did not open'
    [void][PhoneCamUi]::SendMessageW([PhoneCamUi]::GetDlgItem($script:qr, 1104), 0x00F5, 0, 0)
}

$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root 'windows\build\tray_gui.err.log'
$xml = (adb shell 'run-as io.github.abdoeid.phonecam cat shared_prefs/phonecam_pairing.xml') -join "`n"
$code = ([xml]$xml).map.string.'#text'
if (-not $code) { throw 'Current debug-phone pairing code unavailable' }
$phoneInfo = (adb shell ip -4 addr show wlan0) -join "`n"
if ($phoneInfo -notmatch 'inet (\d+\.\d+\.\d+\.\d+)/') { throw 'No phone Wi-Fi address' }
$address = $Matches[1]

Wait-Until { $script:tray = [PhoneCamUi]::FindWindowExW([IntPtr](-3), 0, 'PhoneCamTrayWindow', $null); $script:tray -ne 0 } 'Tray window not found'
$owner = 0u
[void][PhoneCamUi]::GetWindowThreadProcessId($script:tray, [ref]$owner)
if ($owner -ne $ProcessId) { throw 'Refusing to drive another host process' }
$settingsPath = 'HKCU:\Software\PhoneCam\Connection'
$originalWifi = (Get-ItemProperty $settingsPath -ErrorAction SilentlyContinue).UseWifi
try {
Open-Manual
Wait-Until { $script:dialog = [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi'); $script:dialog -ne 0 } 'Wi-Fi tray dialog did not open'
Click 2
Wait-Until { [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi') -eq 0 } 'Closing during scan failed'
if ($TestOffline) {
    $window = (adb shell dumpsys window) -join "`n"
    if ($window -match 'isKeyguardShowing=true') { throw 'Unlock phone first for the optional cold-start/offline test' }
    adb shell am force-stop io.github.abdoeid.phonecam
}
Open-Manual
Wait-Until { $script:dialog = [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi'); $script:dialog -ne 0 } 'Dialog did not reopen after cancelled scan'
if ($TestOffline) {
    Wait-Until { (Status) -like '*No phone found*' } 'No-phone feedback missing'
    'PASS no-phone feedback without freezing tray'
    adb shell am start -n io.github.abdoeid.phonecam/.MainActivity | Out-Null
    Click 1002
}
$list = [PhoneCamUi]::GetDlgItem($script:dialog, 1001)
Wait-Until { [PhoneCamUi]::SendMessageW($list, 0x018B, 0, 0).ToInt64() -gt 0 } 'No phones discovered in GUI'
'PASS close/reopen while scanning and rescan recovery'
Add-Type -AssemblyName System.Drawing
$rect = [PhoneCamUi+Rect]::new()
[void][PhoneCamUi]::GetWindowRect($script:dialog, [ref]$rect)
$image = [Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
$graphics = [Drawing.Graphics]::FromImage($image)
$dc = $graphics.GetHdc()
try { [void][PhoneCamUi]::PrintWindow($script:dialog, $dc, 2) } finally { $graphics.ReleaseHdc($dc) }
$image.Save((Join-Path $root 'windows\build\tray-wifi-dialog.png'))
$graphics.Dispose(); $image.Dispose()
$count = [PhoneCamUi]::SendMessageW($list, 0x018B, 0, 0).ToInt32()
$selected = -1
for ($i = 0; $i -lt $count; ++$i) {
    $item = [Text.StringBuilder]::new(256)
    [void][PhoneCamUi]::ReadText($list, 0x0189, $i, $item)
    if ($item.ToString().Contains($address)) { $selected = $i; break }
}
if ($selected -lt 0) { throw 'Real phone missing from discovery picker' }
[void][PhoneCamUi]::SendMessageW($list, 0x0186, $selected, 0)
[void][PhoneCamUi]::SendMessageW($script:dialog, 0x0111, (1001 -bor (1 -shl 16)), $list)
$edit = [PhoneCamUi]::GetDlgItem($script:dialog, 1003)
[void][PhoneCamUi]::SetText($edit, 0x000C, 0, '')
Click 1007
if ((Status) -notlike '*12-character*') { throw 'Empty code did not produce GUI validation feedback' }
'PASS tray opens discovery picker and validates empty code'
[void][PhoneCamUi]::SetText($edit, 0x000C, 0, 'AAAAAAAAAAAA')
Click 1007
Wait-Until { (Status) -like '*code rejected*' } 'Wrong-code rejection not shown in GUI'
'PASS wrong pairing code shown in GUI'
[void][PhoneCamUi]::SetText($edit, 0x000C, 0, $code)
[void][PhoneCamUi]::SendMessageW([PhoneCamUi]::GetDlgItem($script:dialog, 1004), 0x00F1, 0, 0)
Click 1007
Wait-Until { [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi') -eq 0 } 'Successful UDP connection did not dismiss its dialog'
Wait-Until { @(Get-Content $log) -match 'Connection: Streaming via Wi-Fi \(low latency\)' } 'Wi-Fi UDP did not stream through GUI'
'PASS GUI-only Wi-Fi UDP connection, no process restart'
Open-Manual
Wait-Until { $script:dialog = [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi'); $script:dialog -ne 0 } 'Manual dialog did not reopen'
$list = [PhoneCamUi]::GetDlgItem($script:dialog, 1001)
Wait-Until { [PhoneCamUi]::SendMessageW($list, 0x018B, 0, 0).ToInt64() -gt 0 } 'No phones on second scan'
$edit = [PhoneCamUi]::GetDlgItem($script:dialog, 1003)
[void][PhoneCamUi]::SetText($edit, 0x000C, 0, $code)
[void][PhoneCamUi]::SendMessageW([PhoneCamUi]::GetDlgItem($script:dialog, 1005), 0x014E, 1, 0)
Click 1007
Wait-Until { [PhoneCamUi]::FindWindowW('#32770', 'PhoneCam - Connect over Wi-Fi') -eq 0 } 'Successful TCP connection did not dismiss its dialog'
Wait-Until { @(Get-Content $log) -match 'Connection: Streaming via Wi-Fi \(TCP\)' } 'Wi-Fi TCP switch did not stream through GUI'
'PASS live switch to authenticated Wi-Fi TCP'
ffmpeg -hide_banner -loglevel error -rtbufsize 128M -f dshow -i 'video=PhoneCam (Windows Virtual Camera)' -frames:v 1 -an -f null NUL
if ($LASTEXITCODE -ne 0) { throw 'Virtual camera capture failed after live switch' }
'PASS DirectShow frame from the unchanged virtual camera'
Click 2
$start = @(Get-Content $log).Count
Tray-Command 201
Wait-Until { @(Get-Content $log | Select-Object -Skip $start) -match 'Connection: Streaming via USB \(ADB\)' } 'USB return did not stream'
'PASS tray return to automatic USB/ADB'
$start = @(Get-Content $log).Count
Tray-Command 202
Wait-Until { @(Get-Content $log | Select-Object -Skip $start) -match 'Connection: Disconnected' } 'Disconnect action failed'
'PASS disconnect without removing virtual camera'
$blackPath = Join-Path $root 'windows\build\tray-disconnected.png'
ffmpeg -hide_banner -loglevel error -y -rtbufsize 128M -f dshow -i 'video=PhoneCam (Windows Virtual Camera)' -frames:v 1 -update 1 $blackPath
if ($LASTEXITCODE -ne 0) { throw 'Disconnected virtual camera unavailable' }
$black = [Drawing.Bitmap]::new($blackPath)
try {
    $pixel = $black.GetPixel([int]($black.Width / 2), [int]($black.Height / 2))
    if ($pixel.R -gt 20 -or $pixel.G -gt 20 -or $pixel.B -gt 20) { throw 'Disconnect retained a non-black camera frame' }
} finally { $black.Dispose() }
'PASS disconnect clears the last video frame'
if (@(Select-String -Path $log -Pattern 'Virtual camera started').Count -ne 1) { throw 'Virtual camera was re-registered during switching' }
if ((Get-Process -Id $ProcessId).HasExited) { throw 'Host exited during switching' }
'PASS one host process and one virtual-camera registration throughout'
} finally {
    if ($null -ne $originalWifi) { Set-ItemProperty $settingsPath -Name UseWifi -Value $originalWifi }
    else { Remove-ItemProperty $settingsPath -Name UseWifi -ErrorAction SilentlyContinue }
}
