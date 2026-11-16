<#
.SYNOPSIS
    Installs the MagicMouseFlt driver and binds it to every paired Magic Mouse.
    Used by Setup.exe, and can be run again by hand (it elevates itself).

.PARAMETER PackageDir  Folder with MagicMouseFlt.inf/.sys/.cat and MagicMouseFlt.cer.
.PARAMETER StateDir    Folder for the log and the "we enabled test signing" marker.
.PARAMETER CheckOnly   Only check whether the driver can be installed (Secure Boot).
.PARAMETER ShowResult  Show a message box with the outcome (used by the Start menu "Repair" shortcut).

Exit codes: 0 done · 3010 done, reboot needed · 2 Secure Boot blocks test signing
            4 driver added but no paired Magic Mouse found · 1 error
#>
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot 'driver'),
    [string]$StateDir = $PSScriptRoot,
    [switch]$CheckOnly,
    [switch]$ShowResult
)

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                 '-PackageDir', "`"$PackageDir`"", '-StateDir', "`"$StateDir`"")
    if ($CheckOnly) { $argList += '-CheckOnly' }
    if ($ShowResult) { $argList += '-ShowResult' }
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

function Test-SecureBoot {
    try { return [bool](Confirm-SecureBootUEFI -ErrorAction Stop) } catch { return $false }   # legacy BIOS: no Secure Boot
}
function Test-TestSigning {
    return ((bcdedit /enum '{current}') -join "`n") -match 'testsigning\s+Yes'
}

if ($CheckOnly) {
    if (-not (Test-TestSigning) -and (Test-SecureBoot)) { exit 2 }
    exit 0
}

New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
$log = Join-Path $StateDir 'install.log'
Start-Transcript -Path $log -Append | Out-Null
$exitCode = 0
try {
    $ErrorActionPreference = 'Stop'
    $PackageDir = (Resolve-Path $PackageDir).Path
    $inf = Join-Path $PackageDir 'MagicMouseFlt.inf'
    $cer = Join-Path $PackageDir 'MagicMouseFlt.cer'
    foreach ($f in $inf, $cer, (Join-Path $PackageDir 'MagicMouseFlt.sys'), (Join-Path $PackageDir 'magicmouseflt.cat')) {
        if (-not (Test-Path $f)) { throw "Missing $f" }
    }

    # 1. Test signing (the driver is not Microsoft-signed).
    $reboot = $false
    if (-not (Test-TestSigning)) {
        if (Test-SecureBoot) { throw [System.InvalidOperationException]'Secure Boot is on; test signing cannot be enabled.' }
        bcdedit /set testsigning on | Out-Host
        if ($LASTEXITCODE -ne 0) { throw [System.InvalidOperationException]'bcdedit could not enable test signing (Secure Boot or a policy is blocking it).' }
        Set-Content -Path (Join-Path $StateDir 'testsigning-enabled.flag') -Value (Get-Date -Format o)
        $reboot = $true
        Write-Host 'Test signing enabled (takes effect after reboot).'
    }

    # 2. Trust the certificate the package was signed with.
    $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
    foreach ($storeName in 'Root', 'TrustedPublisher') {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        $store.Open('ReadWrite'); $store.Add($cert); $store.Close()
    }
    Write-Host "Trusted certificate $($cert.Thumbprint) ($($cert.Subject))"

    # 3. Driver store.
    pnputil /add-driver "$inf" | Out-Host

    # 4. Bind to the paired mice (forces it over the in-box driver).
    if (-not ('Win32.NewDev' -as [type])) {
        Add-Type -Namespace Win32 -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool UpdateDriverForPlugAndPlayDevices(
    IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool bRebootRequired);
'@
    }
    $hardwareIds = @(
        'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&030d',
        'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0269',
        'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0323'
    )
    $bound = 0
    foreach ($id in $hardwareIds) {
        $needReboot = $false
        if ([Win32.NewDev]::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero, $id, $inf, 1, [ref]$needReboot)) {
            Write-Host "Bound to $id"
            $bound++
            if ($needReboot) { $reboot = $true }
        } else {
            $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            if ($err -ne 0xE000020B) {
                # The driver can't start until the reboot after enabling test mode; that's expected.
                Write-Host ("{0}: 0x{1:X8}" -f $id, $err)
                if ($reboot) { $bound++ }
            }
        }
    }

    if ($reboot) { $exitCode = 3010 }
    elseif ($bound -eq 0) { $exitCode = 4 }
    else { $exitCode = 0 }
} catch [System.InvalidOperationException] {
    Write-Host $_.Exception.Message
    $exitCode = 2
} catch {
    Write-Host "ERROR: $_"
    $exitCode = 1
} finally {
    Stop-Transcript | Out-Null
}

if ($ShowResult) {
    Add-Type -AssemblyName System.Windows.Forms
    $msg = switch ($exitCode) {
        0    { 'The Magic Mouse driver is installed and active. Move the mouse once to switch on scrolling.' }
        3010 { 'Done. Restart Windows to finish.' }
        2    { 'Secure Boot is on, so test-signing mode cannot be enabled. Turn off Secure Boot in your firmware settings and try again.' }
        4    { 'No paired Magic Mouse was found. Pair it in Settings > Bluetooth & devices and try again.' }
        default { "Something went wrong. Details are in $log" }
    }
    [System.Windows.Forms.MessageBox]::Show($msg, 'Magic Mouse Driver') | Out-Null
}
exit $exitCode
