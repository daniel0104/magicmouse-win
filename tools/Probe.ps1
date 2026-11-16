<#
.SYNOPSIS
    User-mode probe for the Magic Mouse multitouch enable. No driver needed.
    Lists HID collections, sends the enable feature report with HidD_SetFeature,
    and watches for multitouch input reports. Writes MagicMouse-probe.txt to the
    desktop. Run as your normal user (no admin needed); the Magic Mouse must be
    connected and switched on.
#>
$ErrorActionPreference = 'Continue'

$cs = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class Hid {
    [StructLayout(LayoutKind.Sequential)]
    public struct HIDD_ATTRIBUTES { public int Size; public ushort VendorID, ProductID, VersionNumber; }

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS {
        public ushort Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
        public ushort NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
    }

    [DllImport("hid.dll")] public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll")] public static extern bool HidD_GetAttributes(SafeFileHandle h, ref HIDD_ATTRIBUTES a);
    [DllImport("hid.dll")] public static extern bool HidD_GetPreparsedData(SafeFileHandle h, out IntPtr p);
    [DllImport("hid.dll")] public static extern bool HidD_FreePreparsedData(IntPtr p);
    [DllImport("hid.dll")] public static extern int  HidP_GetCaps(IntPtr p, out HIDP_CAPS c);
    [DllImport("hid.dll")] public static extern bool HidD_SetFeature(SafeFileHandle h, byte[] b, int len);
    [DllImport("hid.dll", CharSet = CharSet.Unicode)] public static extern bool HidD_GetProductString(SafeFileHandle h, byte[] b, int len);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr w, int f);
    [DllImport("setupapi.dll")]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref SP_DEVICE_INTERFACE_DATA a);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref SP_DEVICE_INTERFACE_DATA a, IntPtr d, int sz, ref int req, IntPtr dd);
    [DllImport("setupapi.dll")]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);

    public const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_RW = 3, OPEN_EXISTING = 3, FILE_FLAG_OVERLAPPED = 0x40000000;
    public const int DIGCF_PRESENT = 2, DIGCF_DEVICEINTERFACE = 0x10;

    public static System.Collections.Generic.List<string> Paths() {
        var list = new System.Collections.Generic.List<string>();
        Guid g; HidD_GetHidGuid(out g);
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        var ifd = new SP_DEVICE_INTERFACE_DATA(); ifd.cbSize = Marshal.SizeOf(ifd);
        for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref ifd); i++) {
            int req = 0;
            SetupDiGetDeviceInterfaceDetail(set, ref ifd, IntPtr.Zero, 0, ref req, IntPtr.Zero);
            if (req == 0) continue;
            IntPtr det = Marshal.AllocHGlobal(req);
            Marshal.WriteInt32(det, IntPtr.Size == 8 ? 8 : 6);
            if (SetupDiGetDeviceInterfaceDetail(set, ref ifd, det, req, ref req, IntPtr.Zero))
                list.Add(Marshal.PtrToStringUni((IntPtr)(det.ToInt64() + 4)));
            Marshal.FreeHGlobal(det);
        }
        SetupDiDestroyDeviceInfoList(set);
        return list;
    }
}
'@
Add-Type -TypeDefinition $cs -ErrorAction Stop

$out = New-Object System.Collections.Generic.List[string]
function W([string]$s = '') { $out.Add($s); Write-Host $s }

W "Magic Mouse user-mode probe  $(Get-Date -Format o)"
W "Windows: $([Environment]::OSVersion.VersionString)"
W

$apple = @{ 0x05AC = 'USB'; 0x004C = 'BT' }
$applePids = @(0x030D, 0x0269, 0x0323)
$targets = @()

W '== HID collections =='
foreach ($path in [Hid]::Paths()) {
    $h = [Hid]::CreateFile($path, [Hid]::GENERIC_READ -bor [Hid]::GENERIC_WRITE,
                           [Hid]::FILE_SHARE_RW, [IntPtr]::Zero, [Hid]::OPEN_EXISTING, 0, [IntPtr]::Zero)
    if ($h.IsInvalid) {
        # retry read-only (some collections refuse write open)
        $h = [Hid]::CreateFile($path, [Hid]::GENERIC_READ, [Hid]::FILE_SHARE_RW, [IntPtr]::Zero, [Hid]::OPEN_EXISTING, 0, [IntPtr]::Zero)
    }
    if ($h.IsInvalid) { W ("  [cannot open] {0}" -f $path); continue }

    $attr = New-Object Hid+HIDD_ATTRIBUTES; $attr.Size = [Runtime.InteropServices.Marshal]::SizeOf($attr)
    [void][Hid]::HidD_GetAttributes($h, [ref]$attr)
    $caps = New-Object Hid+HIDP_CAPS
    $pp = [IntPtr]::Zero
    if ([Hid]::HidD_GetPreparsedData($h, [ref]$pp)) { [void][Hid]::HidP_GetCaps($pp, [ref]$caps); [void][Hid]::HidD_FreePreparsedData($pp) }
    $name = New-Object byte[] 256
    [void][Hid]::HidD_GetProductString($h, $name, $name.Length)
    $product = ([Text.Encoding]::Unicode.GetString($name)).Trim([char]0)

    $isApple = $apple.ContainsKey([int]$attr.VendorID) -and ($applePids -contains [int]$attr.ProductID)
    $mark = if ($isApple) { ' <== Magic Mouse' } else { '' }
    W ("  VID {0:X4} PID {1:X4}  UsagePage {2:X4} Usage {3:X2}  in={4} out={5} feat={6}  '{7}'{8}" -f `
        $attr.VendorID, $attr.ProductID, $caps.UsagePage, $caps.Usage,
        $caps.InputReportByteLength, $caps.OutputReportByteLength, $caps.FeatureReportByteLength, $product, $mark)
    W ("      $path")

    if ($isApple) {
        $targets += [pscustomobject]@{ Path = $path; Vid = [int]$attr.VendorID; Pid = [int]$attr.ProductID
                                       Feat = [int]$caps.FeatureReportByteLength; In = [int]$caps.InputReportByteLength
                                       Usage = [int]$caps.Usage; UsagePage = [int]$caps.UsagePage }
    }
    $h.Close()
}
W

if (-not $targets) {
    W 'No Apple Magic Mouse HID collection found. Is the mouse on and connected?'
    $out | Set-Content (Join-Path ([Environment]::GetFolderPath('Desktop')) 'MagicMouse-probe.txt') -Encoding UTF8
    return
}

W '== Enable attempts (HidD_SetFeature) =='
# USB-C / MM2 => F1 02 01 ;  MM1 => D7 01. Try the model's bytes on each Apple collection.
foreach ($t in $targets) {
    if ($t.Feat -lt 2) { W ("  skip (no feature reports)  PID {0:X4} usage {1:X4}:{2:X2}" -f $t.Pid, $t.UsagePage, $t.Usage); continue }
    $enable = if ($t.Pid -eq 0x030D) { @(0xD7, 0x01) } else { @(0xF1, 0x02, 0x01) }
    $buf = New-Object byte[] $t.Feat
    for ($i = 0; $i -lt $enable.Count -and $i -lt $buf.Length; $i++) { $buf[$i] = $enable[$i] }

    $h = [Hid]::CreateFile($t.Path, [Hid]::GENERIC_READ -bor [Hid]::GENERIC_WRITE, [Hid]::FILE_SHARE_RW,
                           [IntPtr]::Zero, [Hid]::OPEN_EXISTING, 0, [IntPtr]::Zero)
    if ($h.IsInvalid) { W ("  open-for-write FAILED (err {0})  usage {1:X4}:{2:X2}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error(), $t.UsagePage, $t.Usage); continue }
    $ok = [Hid]::HidD_SetFeature($h, $buf, $buf.Length)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    W ("  SetFeature [{0}] on usage {1:X4}:{2:X2} feat={3}: {4}{5}" -f `
        (($enable | ForEach-Object { '{0:X2}' -f $_ }) -join ' '), $t.UsagePage, $t.Usage, $t.Feat,
        $(if ($ok) { 'OK' } else { 'FAILED' }), $(if ($ok) { '' } else { " (err $err)" }))
    $h.Close()
}
W

# Watch for multitouch input reports on the largest-input Apple collection.
$watch = $targets | Sort-Object In -Descending | Select-Object -First 1
W ("== Input reports for 4s (move/swipe now) on usage {0:X4}:{1:X2}, in={2} ==" -f $watch.UsagePage, $watch.Usage, $watch.In)
try {
    $h = [Hid]::CreateFile($watch.Path, [Hid]::GENERIC_READ, [Hid]::FILE_SHARE_RW, [IntPtr]::Zero,
                           [Hid]::OPEN_EXISTING, [Hid]::FILE_FLAG_OVERLAPPED, [IntPtr]::Zero)
    if ($h.IsInvalid) { W "  cannot open for read" }
    else {
        $fs = New-Object System.IO.FileStream($h, [System.IO.FileAccess]::Read, [Math]::Max($watch.In,1), $true)
        $deadline = (Get-Date).AddSeconds(4); $seen = 0
        while ((Get-Date) -lt $deadline -and $seen -lt 8) {
            $buf = New-Object byte[] ([Math]::Max($watch.In,1))
            $task = $fs.ReadAsync($buf, 0, $buf.Length)
            if ($task.Wait(1500)) {
                $n = $task.Result
                W ("  [{0,3}] {1}" -f $n, (($buf[0..([Math]::Min($n,24)-1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '))
                $seen++
            }
        }
        if ($seen -eq 0) { W '  (no reports - was the mouse moving? is it held by another reader?)' }
        $fs.Close()
    }
} catch { W "  read error: $_" }

$file = Join-Path ([Environment]::GetFolderPath('Desktop')) 'MagicMouse-probe.txt'
$out | Set-Content $file -Encoding UTF8
W
W "Saved to $file"
