#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Changes MagicMouseFlt settings and restarts the mouse so they take effect.

.EXAMPLE
    .\Set-MagicMouseOptions.ps1 -Show
.EXAMPLE
    .\Set-MagicMouseOptions.ps1 -MiddleClick TwoFinger
.EXAMPLE
    .\Set-MagicMouseOptions.ps1 -MiddleClick Off -NaturalScrolling $false -ScrollSpeed 150
.EXAMPLE
    .\Set-MagicMouseOptions.ps1 -Inertia $false -AxisLock $false
#>
param(
    # Middle click simulation: Off, Zone (click in the centre strip),
    # TwoFinger (click with two fingers on the surface), or Both.
    [ValidateSet('Off', 'Zone', 'TwoFinger', 'Both')]
    [string]$MiddleClick,

    # Half width of the centre strip for Zone mode, in touch units (the surface is ~2400 wide).
    [ValidateRange(50, 1200)]
    [int]$MiddleZoneWidth,

    [bool]$Scroll,
    [bool]$HorizontalScroll,
    # $true: content follows the finger (macOS default). $false: behaves like a wheel.
    [bool]$NaturalScrolling,
    # Percent. 100 = default.
    [ValidateRange(10, 500)]
    [int]$ScrollSpeed,
    [bool]$Acceleration,
    # Momentum after flicking.
    [bool]$Inertia,
    # Lock a clearly vertical/horizontal swipe to its axis.
    [bool]$AxisLock,
    # Touch units a finger must move before it starts scrolling (stops resting fingers scrolling).
    [ValidateRange(1, 400)]
    [int]$ScrollThreshold,

    [switch]$Show,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'

$service = 'HKLM:\SYSTEM\CurrentControlSet\Services\MagicMouseFlt'
if (-not (Test-Path $service)) { throw 'MagicMouseFlt is not installed. Run Install.ps1 first.' }
$key = Join-Path $service 'Parameters'
if (-not (Test-Path $key)) { New-Item -Path $key | Out-Null }

function Set-Value([string]$name, [int]$value) {
    New-ItemProperty -Path $key -Name $name -Value $value -PropertyType DWord -Force | Out-Null
}

$changed = $false
$map = @{ Off = 0; Zone = 1; TwoFinger = 2; Both = 3 }
if ($PSBoundParameters.ContainsKey('MiddleClick'))      { Set-Value MiddleClickMode  $map[$MiddleClick]; $changed = $true }
if ($PSBoundParameters.ContainsKey('MiddleZoneWidth'))  { Set-Value MiddleZoneWidth  $MiddleZoneWidth; $changed = $true }
if ($PSBoundParameters.ContainsKey('Scroll'))           { Set-Value ScrollEnabled    ([int]$Scroll); $changed = $true }
if ($PSBoundParameters.ContainsKey('HorizontalScroll')) { Set-Value HorizontalScroll ([int]$HorizontalScroll); $changed = $true }
if ($PSBoundParameters.ContainsKey('NaturalScrolling')) { Set-Value NaturalScrolling ([int]$NaturalScrolling); $changed = $true }
if ($PSBoundParameters.ContainsKey('ScrollSpeed'))      { Set-Value ScrollSpeed      $ScrollSpeed; $changed = $true }
if ($PSBoundParameters.ContainsKey('Acceleration'))     { Set-Value Acceleration     ([int]$Acceleration); $changed = $true }
if ($PSBoundParameters.ContainsKey('Inertia'))          { Set-Value Inertia          ([int]$Inertia); $changed = $true }
if ($PSBoundParameters.ContainsKey('AxisLock'))         { Set-Value AxisLock         ([int]$AxisLock); $changed = $true }
if ($PSBoundParameters.ContainsKey('ScrollThreshold'))  { Set-Value ScrollThreshold  $ScrollThreshold; $changed = $true }

if ($Show -or -not $changed) {
    Get-ItemProperty -Path $key |
        Select-Object MiddleClickMode, MiddleZoneWidth, ScrollEnabled, HorizontalScroll, NaturalScrolling,
                      ScrollSpeed, Acceleration, Inertia, AxisLock, ScrollThreshold |
        Format-List
    Write-Host 'MiddleClickMode: 0 = off, 1 = centre zone, 2 = two-finger, 3 = both'
}

if ($changed -and -not $NoRestart) {
    # Settings are read when the device starts, so restart each Magic Mouse.
    $mice = Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -match 'BTHENUM\\\{00001124-0000-1000-8000-00805F9B34FB\}_VID&(000205AC_PID&030D|0001004C_PID&0269|0001004C_PID&0323)'
    }
    foreach ($m in $mice) {
        pnputil /restart-device "$($m.InstanceId)" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Disable-PnpDevice -InstanceId $m.InstanceId -Confirm:$false
            Enable-PnpDevice  -InstanceId $m.InstanceId -Confirm:$false
        }
        Write-Host "Restarted $($m.FriendlyName)"
    }
    if (-not $mice) { Write-Host 'Saved. The mouse picks up the new settings next time it connects.' }
}
