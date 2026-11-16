<#
.SYNOPSIS
    Tests whether a report request on the Magic Mouse's vendor collection forces
    HidBth onto its control channel (so the installed driver can learn the
    handle and send the multitouch enable). No build, no admin.

    Run with the v1.0.5 driver installed. Keep moving/swiping the mouse the
    whole time. Afterwards, try scrolling, then run Magic Mouse Diagnostics and
    send the file (check whether ControlKnown became 1).
#>
$ErrorActionPreference = 'Continue'

$cs = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Hid2 {
    [StructLayout(LayoutKind.Sequential)]
    public struct HIDD_ATTRIBUTES { public int Size; public ushort VendorID, ProductID, VersionNumber; }
    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS {
        public ushort Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort a,b,c,d,e,f,g,h,i,j;
    }
    [DllImport("hid.dll")] public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll")] public static extern bool HidD_GetAttributes(SafeFileHandle h, ref HIDD_ATTRIBUTES a);
    [DllImport("hid.dll")] public static extern bool HidD_GetPreparsedData(SafeFileHandle h, out IntPtr p);
    [DllImport("hid.dll")] public static extern bool HidD_FreePreparsedData(IntPtr p);
    [DllImport("hid.dll")] public static extern int  HidP_GetCaps(IntPtr p, out HIDP_CAPS c);
    [DllImport("hid.dll")] public static extern bool HidD_GetInputReport(SafeFileHandle h, byte[] b, int len);
    [DllImport("hid.dll")] public static extern bool HidD_GetFeature(SafeFileHandle h, byte[] b, int len);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr w, int f);
    [DllImport("setupapi.dll")] public static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref SP_DEVICE_INTERFACE_DATA a);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)] public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref SP_DEVICE_INTERFACE_DATA a, IntPtr d, int sz, ref int req, IntPtr dd);
    [DllImport("setupapi.dll")] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
    [StructLayout(LayoutKind.Sequential)] public struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid g; public int Flags; public IntPtr Reserved; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sec, uint d, uint f, IntPtr t);
    public static System.Collections.Generic.List<string> Paths() {
        var list = new System.Collections.Generic.List<string>();
        Guid g; HidD_GetHidGuid(out g);
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
        var ifd = new SP_DEVICE_INTERFACE_DATA(); ifd.cbSize = Marshal.SizeOf(ifd);
        for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref ifd); i++) {
            int req = 0; SetupDiGetDeviceInterfaceDetail(set, ref ifd, IntPtr.Zero, 0, ref req, IntPtr.Zero);
            if (req == 0) continue;
            IntPtr det = Marshal.AllocHGlobal(req); Marshal.WriteInt32(det, IntPtr.Size == 8 ? 8 : 6);
            if (SetupDiGetDeviceInterfaceDetail(set, ref ifd, det, req, ref req, IntPtr.Zero))
                list.Add(Marshal.PtrToStringUni((IntPtr)(det.ToInt64() + 4)));
            Marshal.FreeHGlobal(det);
        }
        SetupDiDestroyDeviceInfoList(set); return list;
    }
}
'@
Add-Type -TypeDefinition $cs -ErrorAction Stop
$GENERIC_RW = 0xC0000000; $GENERIC_READ = 0x80000000; $SHARE_RW = 3; $OPEN = 3

# Find the openable Magic Mouse (PID 0323) vendor collection.
$target = $null
foreach ($path in [Hid2]::Paths()) {
    $h = [Hid2]::CreateFile($path, $GENERIC_RW, $SHARE_RW, [IntPtr]::Zero, $OPEN, 0, [IntPtr]::Zero)
    if ($h.IsInvalid) { $h = [Hid2]::CreateFile($path, $GENERIC_READ, $SHARE_RW, [IntPtr]::Zero, $OPEN, 0, [IntPtr]::Zero) }
    if ($h.IsInvalid) { continue }
    $a = New-Object Hid2+HIDD_ATTRIBUTES; $a.Size = [Runtime.InteropServices.Marshal]::SizeOf($a)
    [void][Hid2]::HidD_GetAttributes($h, [ref]$a)
    if ($a.VendorID -eq 0x004C -and $a.ProductID -eq 0x0323) {
        $caps = New-Object Hid2+HIDP_CAPS; $pp = [IntPtr]::Zero
        if ([Hid2]::HidD_GetPreparsedData($h, [ref]$pp)) { [void][Hid2]::HidP_GetCaps($pp, [ref]$caps); [void][Hid2]::HidD_FreePreparsedData($pp) }
        $target = [pscustomobject]@{ Path = $path; In = [int]$caps.InputReportByteLength; Feat = [int]$caps.FeatureReportByteLength; Handle = $h }
        Write-Host ("Found Magic Mouse vendor collection: in={0} feat={1}" -f $caps.InputReportByteLength, $caps.FeatureReportByteLength)
        break
    }
    $h.Close()
}
if (-not $target) { Write-Host 'Magic Mouse (PID 0323) openable collection not found. Is it on?'; return }

Write-Host ''
Write-Host 'Baiting control-channel traffic for ~10 seconds.'
Write-Host '>>> Keep swiping and moving the mouse the whole time, and try to scroll. <<<'
Start-Sleep 1

$okGet = 0; $okFeat = 0; $tries = 0
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline) {
    $tries++
    # GET input report 0x90 (battery): forces a GET_REPORT on the control channel.
    $buf = New-Object byte[] ([Math]::Max($target.In, 3)); $buf[0] = 0x90
    if ([Hid2]::HidD_GetInputReport($target.Handle, $buf, $buf.Length)) { $okGet++ }
    # Also try a feature GET on report 0x55 if the collection has features.
    if ($target.Feat -ge 2) {
        $fb = New-Object byte[] $target.Feat; $fb[0] = 0x55
        if ([Hid2]::HidD_GetFeature($target.Handle, $fb, $fb.Length)) { $okFeat++ }
    }
    Start-Sleep -Milliseconds 400
}
$target.Handle.Close()

Write-Host ''
Write-Host ("Done. GetInputReport ok {0}/{1}, GetFeature ok {2}." -f $okGet, $tries, $okFeat)
Write-Host 'Now: did scrolling start working? Either way, run "Magic Mouse Diagnostics"'
Write-Host 'and send the file. The key line is ControlKnown (1 = the bait worked).'
