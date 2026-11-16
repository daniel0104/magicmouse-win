<#
.SYNOPSIS
    Removes MagicMouseFlt: driver package, trusted certificate, and test signing
    (only if our installer turned it on). The mouse goes back to the in-box driver.
#>
param([string]$StateDir = $PSScriptRoot)

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-StateDir', "`"$StateDir`"")
    exit $p.ExitCode
}

Start-Transcript -Path (Join-Path $env:TEMP 'MagicMouseFlt-uninstall.log') -Append | Out-Null
try {
    Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\magicmouseflt.inf' } | ForEach-Object {
        Write-Host "Removing $($_.Driver)"
        pnputil /delete-driver $_.Driver /uninstall /force | Out-Host
    }
    pnputil /scan-devices | Out-Null

    $cer = Join-Path $StateDir 'driver\MagicMouseFlt.cer'
    if (Test-Path $cer) {
        $thumb = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)).Thumbprint
        foreach ($storeName in 'Root', 'TrustedPublisher') {
            Remove-Item -Path "Cert:\LocalMachine\$storeName\$thumb" -ErrorAction SilentlyContinue
        }
        Write-Host "Removed certificate $thumb"
    }

    $flag = Join-Path $StateDir 'testsigning-enabled.flag'
    if (Test-Path $flag) {
        bcdedit /set testsigning off | Out-Host
        Remove-Item $flag -Force
        Write-Host 'Test signing turned off (after reboot).'
    }
} finally {
    Stop-Transcript | Out-Null
}
exit 0
