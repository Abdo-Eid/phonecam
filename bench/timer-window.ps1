param(
  [int]$Width = 960,
  [int]$Height = 1080,
  [int]$Left = 0,
  [int]$Top = 0,
  [int]$AutoCloseSeconds = 0,
  [switch]$UnixMilliseconds
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$form.Bounds = New-Object System.Drawing.Rectangle $Left, $Top, $Width, $Height
$form.BackColor = [System.Drawing.Color]::Black
$form.TopMost = $true
$form.ShowInTaskbar = $false

$label = New-Object System.Windows.Forms.Label
$label.Dock = [System.Windows.Forms.DockStyle]::Fill
$label.TextAlign = [System.Drawing.ContentAlignment]::MiddleCenter
$label.BackColor = [System.Drawing.Color]::Black
$label.ForeColor = [System.Drawing.Color]::Lime
$fontSize = [Math]::Max(36, [Math]::Floor($Width / 12))
$label.Font = New-Object System.Drawing.Font(
  'Consolas',
  $fontSize,
  [System.Drawing.FontStyle]::Bold,
  [System.Drawing.GraphicsUnit]::Point
)
$label.Text = '--:--:--.---'
$form.Controls.Add($label)

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 10
$timer.Add_Tick({
  $label.Text = if ($UnixMilliseconds) {
    [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
  } else {
    (Get-Date).ToString('HH:mm:ss.fff')
  }
})
$timer.Start()

if ($AutoCloseSeconds -gt 0) {
  $closeTimer = New-Object System.Windows.Forms.Timer
  $closeTimer.Interval = $AutoCloseSeconds * 1000
  $closeTimer.Add_Tick({
    $closeTimer.Stop()
    $form.Close()
  })
  $closeTimer.Start()
}

[System.Windows.Forms.Application]::Run($form)
