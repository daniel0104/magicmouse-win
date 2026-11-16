<#
.SYNOPSIS
    Collects MagicMouseFlt diagnostics into MagicMouse-diagnostics.txt on the desktop.
    Asks you to use the mouse for a few seconds so the driver's counters move.
#>
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    exit
}
$ErrorActionPreference = 'Continue'
$statusKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\MagicMouseFlt\Parameters\Status'
$miceRegex = 'BTHENUM\\\{00001124-0000-1000-8000-00805F9B34FB\}_VID&(000205AC_PID&030D|0001004C_PID&0269|0001004C_PID&0323)'

function Get-Status {
    $p = Get-ItemProperty -Path $statusKey -ErrorAction SilentlyContinue
    if (-not $p) { return $null }
    $h = [ordered]@{}
    foreach ($prop in $p.PSObject.Properties) { if ($prop.Name -notlike 'PS*') { $h[$prop.Name] = $prop.Value } }
    return $h
}
function Hex($v) { '0x{0:X8}' -f ([uint32]([int64]$v -band 0xFFFFFFFF)) }
function Bytes($b) { if ($b) { ($b | ForEach-Object { '{0:X2}' -f $_ }) -join ' ' } else { '(none)' } }

Write-Host 'Magic Mouse diagnostics' -ForegroundColor Cyan
Write-Host 'Make sure the mouse is switched on. Then, when the countdown starts,'
Write-Host 'swipe a finger up/down and left/right on it, move it, and click a few times.'
Read-Host 'Press Enter to start'
$before = Get-Status
for ($i = 12; $i -gt 0; $i--) { Write-Host -NoNewline "`r  Use the mouse now... $i s  "; Start-Sleep 1 }
Write-Host "`r  Collecting...            "
Start-Sleep 2
$after = Get-Status

$out = New-Object System.Collections.Generic.List[string]
function W([string]$s = '') { $out.Add($s) }

W "MagicMouseFlt diagnostics  $(Get-Date -Format o)"
W "Windows: $([Environment]::OSVersion.VersionString)  arch: $env:PROCESSOR_ARCHITECTURE"
$bcd = (bcdedit /enum '{current}') -join "`n"
W "Test signing: $(if ($bcd -match 'testsigning\s+Yes') { 'ON' } else { 'off' })"
try { W "Secure Boot: $(Confirm-SecureBootUEFI -ErrorAction Stop)" } catch { W "Secure Boot: n/a ($($_.Exception.Message))" }
$setup = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6E3B7A41-2C1D-4F7B-9B2E-4D6A1C9F0A51}_is1' -ErrorAction SilentlyContinue
W "Installed version: $($setup.DisplayVersion)"
W

W '== Services =='
foreach ($svc in 'MagicMouseFlt', 'vhf', 'HidBth') {
    $s = Get-CimInstance Win32_SystemDriver -Filter "Name='$svc'" -ErrorAction SilentlyContinue
    if ($s) { W ("{0,-14} state={1,-8} start={2,-8} path={3}" -f $svc, $s.State, $s.StartMode, $s.PathName) } else { W "$svc : not found" }
}
W

W '== Magic Mouse devices =='
$mice = Get-PnpDevice | Where-Object { $_.InstanceId -match $miceRegex }
if (-not $mice) { W 'No Magic Mouse device found.' }
foreach ($m in $mice) {
    $props = @{}
    foreach ($k in 'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_LowerFilters', 'DEVPKEY_Device_Service',
                   'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_Children',
                   'DEVPKEY_Device_IsPresent', 'DEVPKEY_Device_ProblemStatus') {
        $props[$k] = (Get-PnpDeviceProperty -InstanceId $m.InstanceId -KeyName $k -ErrorAction SilentlyContinue).Data
    }
    W "$($m.FriendlyName)"
    W "  instance:      $($m.InstanceId)"
    W "  status:        $($m.Status)  present=$($props['DEVPKEY_Device_IsPresent'])  problem=$($props['DEVPKEY_Device_ProblemCode'])  problemStatus=$(if ($props['DEVPKEY_Device_ProblemStatus']) { Hex $props['DEVPKEY_Device_ProblemStatus'] })"
    W "  service:       $($props['DEVPKEY_Device_Service'])"
    W "  lower filters: $($props['DEVPKEY_Device_LowerFilters'] -join ', ')"
    W "  driver:        $($props['DEVPKEY_Device_DriverInfPath']) $($props['DEVPKEY_Device_DriverVersion'])"
    W '  children:'
    foreach ($c in @($props['DEVPKEY_Device_Children'])) {
        if (-not $c) { continue }
        $cd = Get-PnpDevice -InstanceId $c -ErrorAction SilentlyContinue
        $svc = (Get-PnpDeviceProperty -InstanceId $c -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        W "    $c  [$($cd.Status)] $($cd.FriendlyName) svc=$svc"
        $gc = (Get-PnpDeviceProperty -InstanceId $c -KeyName 'DEVPKEY_Device_Children' -ErrorAction SilentlyContinue).Data
        foreach ($g in @($gc)) {
            if (-not $g) { continue }
            $gd = Get-PnpDevice -InstanceId $g -ErrorAction SilentlyContinue
            W "      $g  [$($gd.Status)] $($gd.FriendlyName)"
        }
    }
}
W
W '== Virtual mouse (VID_1209) =='
$v = Get-PnpDevice | Where-Object { $_.InstanceId -like '*VID_1209*PID_0001*' }
if ($v) { foreach ($d in $v) { W "  $($d.InstanceId)  [$($d.Status)] $($d.FriendlyName)" } } else { W '  none' }
W

W '== Driver status counters =='
if (-not $after) {
    W 'No Status key: the driver has never run (not loaded, or an older version).'
} else {
    foreach ($k in $after.Keys) {
        $val = $after[$k]
        if ($k -in 'ReportIds', 'BrbTypes', 'LastInput', 'LastControlIn') { continue }
        $delta = ''
        if ($before -and $before.Contains($k) -and $val -is [int] -and $k -ne 'Updated') {
            $d = [int64]$val - [int64]$before[$k]; if ($d -ne 0) { $delta = "   (+$d while testing)" }
        }
        if ($k -like '*Status') { $val = Hex $val }
        if ($k -eq 'LastHidVidPid' -and $val -is [int] -and $val -ne 0) {
            $val = 'VID {0:X4} PID {1:X4}' -f (($val -shr 16) -band 0xFFFF), ($val -band 0xFFFF)
        }
        W ("  {0,-24} {1}{2}" -f $k, $val, $delta)
    }
    $ids = $after['ReportIds']
    if ($ids) {
        $list = for ($i = 0; $i -lt 256; $i++) {
            $n = [BitConverter]::ToInt32($ids, $i * 4); if ($n) { '0x{0:X2}={1}' -f $i, $n }
        }
        W "  ReportIds            $($list -join '  ')"
    }
    $bt = $after['BrbTypes']
    if ($bt) {
        $types = for ($i = 0; $i + 1 -lt $bt.Length; $i += 2) { '0x{0:X4}' -f [BitConverter]::ToUInt16($bt, $i) }
        W "  BrbTypes             $($types -join ' ')"
    }
    W "  LastInput            $(Bytes $after['LastInput'])"
    W "  LastControlIn        $(Bytes $after['LastControlIn'])"
}
W
W '== Settings =='
$s = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MagicMouseFlt\Parameters' -ErrorAction SilentlyContinue
if ($s) { foreach ($p in $s.PSObject.Properties) { if ($p.Name -notlike 'PS*') { W "  $($p.Name) = $($p.Value)" } } }
W
W '== Recent driver install events =='
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Kernel-PnP/Configuration'; StartTime = (Get-Date).AddDays(-1) } -MaxEvents 200 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match '0323|0269|030D|MagicMouseFlt|VID_1209' } | Select-Object -First 15 |
    ForEach-Object { W "  $($_.TimeCreated.ToString('HH:mm:ss')) [$($_.Id)] $(($_.Message -split "`n")[0..3] -join ' | ')" }

$file = Join-Path ([Environment]::GetFolderPath('Desktop')) 'MagicMouse-diagnostics.txt'
$out | Set-Content -Path $file -Encoding UTF8
Write-Host "Saved to $file" -ForegroundColor Green
Start-Process notepad.exe $file
