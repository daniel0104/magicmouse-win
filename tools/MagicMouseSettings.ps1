<#
.SYNOPSIS
    Settings window for MagicMouseFlt. Saves to the driver's registry key and
    restarts the mouse so changes apply. Elevates itself (settings live in HKLM).
#>
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$PSCommandPath`"")
    exit
}

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$service = 'HKLM:\SYSTEM\CurrentControlSet\Services\MagicMouseFlt'
$key = Join-Path $service 'Parameters'
if (-not (Test-Path $service)) {
    [System.Windows.Forms.MessageBox]::Show('The Magic Mouse driver is not installed.', 'Magic Mouse') | Out-Null
    exit
}
if (-not (Test-Path $key)) { New-Item -Path $key | Out-Null }

$defaults = [ordered]@{
    MiddleClickMode = 1; MiddleZoneWidth = 350; ScrollEnabled = 1; HorizontalScroll = 1
    NaturalScrolling = 1; ScrollSpeed = 100; Acceleration = 1; Inertia = 1; AxisLock = 1; ScrollThreshold = 25
}
function Get-Settings {
    $cur = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
    $s = [ordered]@{}
    foreach ($k in $defaults.Keys) {
        $v = $cur.$k
        $s[$k] = if ($null -ne $v) { [int]$v } else { $defaults[$k] }
    }
    return $s
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Magic Mouse settings'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(420, 505)
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$y = 14
function Add-Label([string]$text, [int]$top, [bool]$bold = $false) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.AutoSize = $true; $l.Location = New-Object System.Drawing.Point(16, $top)
    if ($bold) { $l.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold) }
    $form.Controls.Add($l); return $l
}
function Add-Check([string]$text, [int]$top) {
    $c = New-Object System.Windows.Forms.CheckBox
    $c.Text = $text; $c.AutoSize = $true; $c.Location = New-Object System.Drawing.Point(20, $top)
    $form.Controls.Add($c); return $c
}
function Add-Slider([int]$min, [int]$max, [int]$top, [int]$tick) {
    $t = New-Object System.Windows.Forms.TrackBar
    $t.Minimum = $min; $t.Maximum = $max; $t.TickFrequency = $tick; $t.SmallChange = 1; $t.LargeChange = $tick
    $t.Location = New-Object System.Drawing.Point(16, $top); $t.Size = New-Object System.Drawing.Size(320, 40)
    $form.Controls.Add($t); return $t
}
function Add-ValueLabel([int]$top) {
    $l = New-Object System.Windows.Forms.Label
    $l.AutoSize = $true; $l.Location = New-Object System.Drawing.Point(345, ($top + 6))
    $form.Controls.Add($l); return $l
}

Add-Label 'Middle click' $y $true | Out-Null; $y += 24
$middle = New-Object System.Windows.Forms.ComboBox
$middle.DropDownStyle = 'DropDownList'
$middle.Items.AddRange(@('Off', 'Click in the centre of the mouse', 'Click with two fingers on the mouse', 'Either'))
$middle.Location = New-Object System.Drawing.Point(20, $y); $middle.Size = New-Object System.Drawing.Size(380, 24)
$form.Controls.Add($middle); $y += 32
Add-Label 'Centre area width' $y | Out-Null; $y += 18
$zone = Add-Slider 100 900 $y 100; $zoneVal = Add-ValueLabel $y; $y += 46

Add-Label 'Scrolling' $y $true | Out-Null; $y += 24
$scroll   = Add-Check 'Scroll by swiping on the mouse' $y; $y += 24
$horiz    = Add-Check 'Horizontal scrolling' $y; $y += 24
$natural  = Add-Check 'Natural direction (content follows your finger, like a Mac)' $y; $y += 24
$accel    = Add-Check 'Faster swipes scroll further' $y; $y += 24
$inertia  = Add-Check 'Momentum after a flick' $y; $y += 24
$axis     = Add-Check 'Keep straight swipes on one axis' $y; $y += 30
Add-Label 'Scroll speed' $y | Out-Null; $y += 18
$speed = Add-Slider 10 300 $y 25; $speedVal = Add-ValueLabel $y; $y += 46
Add-Label 'Movement before scrolling starts (higher ignores resting fingers)' $y | Out-Null; $y += 18
$thresh = Add-Slider 5 150 $y 15; $threshVal = Add-ValueLabel $y; $y += 50

$zone.Add_ValueChanged({ $zoneVal.Text = "$($zone.Value)" })
$speed.Add_ValueChanged({ $speedVal.Text = "$($speed.Value)%" })
$thresh.Add_ValueChanged({ $threshVal.Text = "$($thresh.Value)" })
$middle.Add_SelectedIndexChanged({ $zone.Enabled = ($middle.SelectedIndex -eq 1 -or $middle.SelectedIndex -eq 3) })
$scroll.Add_CheckedChanged({ foreach ($c in $horiz, $natural, $accel, $inertia, $axis, $speed, $thresh) { $c.Enabled = $scroll.Checked } })

function Show-Settings($s) {
    $middle.SelectedIndex = [Math]::Min(3, [Math]::Max(0, $s.MiddleClickMode))
    $zone.Value = [Math]::Min($zone.Maximum, [Math]::Max($zone.Minimum, $s.MiddleZoneWidth))
    $scroll.Checked = [bool]$s.ScrollEnabled; $horiz.Checked = [bool]$s.HorizontalScroll
    $natural.Checked = [bool]$s.NaturalScrolling; $accel.Checked = [bool]$s.Acceleration
    $inertia.Checked = [bool]$s.Inertia; $axis.Checked = [bool]$s.AxisLock
    $speed.Value = [Math]::Min($speed.Maximum, [Math]::Max($speed.Minimum, $s.ScrollSpeed))
    $thresh.Value = [Math]::Min($thresh.Maximum, [Math]::Max($thresh.Minimum, $s.ScrollThreshold))
    $zoneVal.Text = "$($zone.Value)"; $speedVal.Text = "$($speed.Value)%"; $threshVal.Text = "$($thresh.Value)"
}

function Restart-Mice {
    $mice = Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -match 'BTHENUM\\\{00001124-0000-1000-8000-00805F9B34FB\}_VID&(000205AC_PID&030D|0001004C_PID&0269|0001004C_PID&0323)'
    }
    foreach ($m in $mice) {
        pnputil /restart-device "$($m.InstanceId)" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Disable-PnpDevice -InstanceId $m.InstanceId -Confirm:$false
            Enable-PnpDevice -InstanceId $m.InstanceId -Confirm:$false
        }
    }
    return @($mice).Count
}

$status = New-Object System.Windows.Forms.Label
$status.AutoSize = $true; $status.ForeColor = [System.Drawing.Color]::DimGray
$status.Location = New-Object System.Drawing.Point(16, ($y + 34))
$form.Controls.Add($status)

$apply = New-Object System.Windows.Forms.Button
$apply.Text = 'Apply'; $apply.Location = New-Object System.Drawing.Point(232, $y); $apply.Size = New-Object System.Drawing.Size(80, 28)
$close = New-Object System.Windows.Forms.Button
$close.Text = 'Close'; $close.Location = New-Object System.Drawing.Point(320, $y); $close.Size = New-Object System.Drawing.Size(80, 28)
$reset = New-Object System.Windows.Forms.Button
$reset.Text = 'Defaults'; $reset.Location = New-Object System.Drawing.Point(16, $y); $reset.Size = New-Object System.Drawing.Size(80, 28)
$form.Controls.AddRange(@($apply, $close, $reset))
$form.AcceptButton = $apply; $form.CancelButton = $close

$apply.Add_Click({
    $values = [ordered]@{
        MiddleClickMode = $middle.SelectedIndex; MiddleZoneWidth = $zone.Value
        ScrollEnabled = [int]$scroll.Checked; HorizontalScroll = [int]$horiz.Checked
        NaturalScrolling = [int]$natural.Checked; ScrollSpeed = $speed.Value
        Acceleration = [int]$accel.Checked; Inertia = [int]$inertia.Checked
        AxisLock = [int]$axis.Checked; ScrollThreshold = $thresh.Value
    }
    foreach ($k in $values.Keys) {
        New-ItemProperty -Path $key -Name $k -Value $values[$k] -PropertyType DWord -Force | Out-Null
    }
    $form.Cursor = 'WaitCursor'; $status.Text = 'Restarting the mouse...'; $form.Refresh()
    $n = Restart-Mice
    $form.Cursor = 'Default'
    $status.Text = if ($n -gt 0) { 'Saved. The mouse reconnects in a few seconds.' } else { 'Saved. Applies next time the mouse connects.' }
})
$reset.Add_Click({ Show-Settings $defaults; $status.Text = 'Defaults loaded - press Apply to save.' })
$close.Add_Click({ $form.Close() })

Show-Settings (Get-Settings)
[void]$form.ShowDialog()
