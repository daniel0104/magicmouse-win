# MagicMouseFlt — Apple Magic Mouse driver for Windows

A KMDF kernel filter that makes an Apple Magic Mouse behave like a full mouse on Windows over Bluetooth:

- **2D scrolling**: swipe one finger in any direction. It scrolls vertically and horizontally with smooth high-resolution scrolling (120 steps per notch, like a precision wheel).
- **Momentum**: a flick keeps scrolling and slows down, like on macOS. Touching or clicking stops it.
- **Middle click simulation** (configurable):
  - `Zone` (default): click with your finger in the centre strip of the mouse.
  - `TwoFinger`: click while two fingers are on the surface.
  - `Both` or `Off`.
- Natural or wheel-style direction, speed, acceleration, axis lock, and the resting-finger threshold are all settings.
- Left/right click, pointer movement and battery reporting still work as before.

Supported models (Bluetooth):

| Model | Hardware ID |
|---|---|
| Magic Mouse (1st gen, AA batteries) | `BTHENUM\{00001124-…}_VID&000205ac_PID&030d` |
| Magic Mouse 2 (Lightning) | `BTHENUM\{00001124-…}_VID&0001004c_PID&0269` |
| Magic Mouse USB-C (2024) | `BTHENUM\{00001124-…}_VID&0001004c_PID&0323` |

## How it works

```
 mouhid / Windows input            mouhid / Windows input
          │                                  │
   in-box mouse (goes quiet)        virtual mouse (VHF)  ◄─ scroll, pan, 3 buttons
          │                                  ▲
   hidclass + HidBth  (in-box)               │
          │  BRBs (L2CAP packets)            │
   ┌──────▼──────────────────────────────────┴───┐
   │  MagicMouseFlt  (this driver, lower filter)  │
   └──────┬───────────────────────────────────────┘
   vhf.sys (in-box Virtual HID Framework)
          │
   BTHENUM PDO  ──  Bluetooth  ──  Magic Mouse
```

1. The filter sits under the in-box Bluetooth HID driver, which is the same place Apple's Boot Camp driver sits. It sees every L2CAP packet as a Bluetooth Request Block (BRB).
2. **Finding the control channel.** The enable command must go on the mouse's HID *control* channel, but on the 2024 USB-C model that channel is never opened through the filter, and a second one can't be opened (the device rejects it). So the filter "baits" it: it opens the mouse's own HID collection and issues a feature read (`GET_REPORT` of report `0x55`). That makes HidBth transact on its control channel, and the filter records the channel handle from that traffic.
3. **Enabling multitouch.** On the learned control channel the filter sends the raw `SET_REPORT(Feature)` that switches the mouse into multitouch mode (`D7 01` for the first-gen mouse, `F1 02 01` for Magic Mouse 2 / USB-C) — the same bytes macOS and Linux send. Sending it raw on L2CAP is necessary because that report isn't in the descriptor Windows parsed, so the normal HID path would reject it.
4. **Reading the full reports.** HidBth sizes its read buffer to the 8-byte boot mouse report, which would truncate the longer multitouch reports (`14 + 8·N` bytes). The filter substitutes its own larger read buffer so the whole report arrives, then copies back only what fits into HidBth's buffer for anything it passes through.
5. The multitouch reports are taken off the interrupt channel before HidBth sees them. A gesture engine (`src/gesture.c`) turns them into a standard HID mouse report with buttons, X/Y, Wheel and AC Pan, sent to Windows through a virtual HID device created with VHF. Its descriptor has Resolution Multipliers, so Windows turns on hi-res scrolling for both axes.
6. Everything else (battery, other reports) passes through unchanged. If anything fails during setup (VHF, the Bluetooth interface), the filter becomes a pure pass-through and the mouse works exactly as it did without the driver.

The report formats and enable sequences come from the Linux `hid-magicmouse` driver. Confirmed working on the 2024 USB-C Magic Mouse (`PID 0323`) over Bluetooth.

If scrolling ever stops after re-pairing or a long sleep, open **Magic Mouse Settings** or re-run the installer's **Repair** shortcut; the driver re-arms multitouch on its own when it sees the mouse fall back to plain reports.

## Install with Setup.exe (no build needed)

GitHub builds the installer automatically (`.github/workflows/build.yml`). Download `MagicMouseDriver-Setup-<version>.exe` from the repository's **Releases** page, or from the latest run under **Actions**.

1. Pair the Magic Mouse (Settings → Bluetooth & devices).
2. Turn off **Secure Boot** in your PC's firmware settings. Windows won't load a driver that isn't Microsoft-signed while it's on. Setup checks this and explains how if needed. If you use BitLocker, have your recovery key ready.
3. Run Setup. SmartScreen warns because the installer isn't signed: choose "More info" → "Run anyway". Setup turns on test-signing mode, trusts the driver's certificate, installs the driver on your mouse, and asks you to restart.
4. After the restart, move the mouse once and scrolling starts. Windows shows a small "Test Mode" label on the desktop; that's expected.

Start menu → **Magic Mouse** has:
- **Magic Mouse Settings**: a window for middle click, direction, speed, momentum and the other options.
- **Repair Magic Mouse Driver**: run it after you re-pair the mouse.
- **Uninstall**: removes the driver and the certificate, and turns test-signing mode back off if Setup turned it on.

Each build is signed with a fresh certificate whose private key is deleted when the build finishes, so nobody can sign anything else with the certificate your PC trusts.

## Requirements (building it yourself)

- Windows 10 1709 or later, or Windows 11; x64 or ARM64
- Visual Studio 2022 with the "Desktop development with C++" workload
- Windows Driver Kit (WDK) matching your SDK
- To load a self-built driver you need **test-signing mode**, which requires **Secure Boot off**. The alternative is signing it yourself through Microsoft's attestation signing, which needs an EV certificate and a Partner Center account.

> If Apple's Boot Camp driver (`applewirelessmouse`) or a commercial Magic Mouse utility is installed, uninstall it first. Two filters fighting over the same mouse won't work.

## Build

1. Open `MagicMouseFlt.vcxproj` in Visual Studio (it creates a solution for you).
2. Choose `Release | x64` (or `ARM64`) and build.

   Or, from a Developer Command Prompt:
   ```
   msbuild MagicMouseFlt.vcxproj /p:Configuration=Release /p:Platform=x64
   ```
3. The package lands in `x64\Release\MagicMouseFlt\`: `MagicMouseFlt.inf`, `MagicMouseFlt.sys`, `magicmouseflt.cat`, all test-signed with the WDK's auto-generated certificate.

## Install

1. Pair the Magic Mouse normally (Settings → Bluetooth & devices).
2. One time only, enable test signing (Secure Boot must be off in your firmware settings). In an **admin** terminal:
   ```
   bcdedit /set testsigning on
   ```
   Then reboot.
3. Install (admin PowerShell, from the project folder):
   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass
   .\tools\Install.ps1
   ```
   The script trusts the test certificate, adds the package to the driver store, binds it to every paired Magic Mouse, and prints the resulting lower filters. You should see `vhf, MagicMouseFlt`.
4. Move the mouse once. Within about a second it switches to multitouch mode and scrolling starts working. Device Manager also shows a new HID-compliant mouse: that is the virtual mouse.

If you unpair and re-pair the mouse, Windows creates a new device. It normally picks this driver again by itself; if scrolling stops working, run `Install.ps1` again.

## Options

Change settings with the script. It saves them and restarts the mouse so they take effect:

```powershell
.\tools\Set-MagicMouseOptions.ps1 -Show
.\tools\Set-MagicMouseOptions.ps1 -MiddleClick TwoFinger
.\tools\Set-MagicMouseOptions.ps1 -MiddleClick Off
.\tools\Set-MagicMouseOptions.ps1 -NaturalScrolling $false -ScrollSpeed 150
.\tools\Set-MagicMouseOptions.ps1 -Inertia $false -AxisLock $false
```

The values live in `HKLM\SYSTEM\CurrentControlSet\Services\MagicMouseFlt\Parameters` (DWORDs):

| Value | Default | Meaning |
|---|---|---|
| `MiddleClickMode` | 1 | 0 off · 1 centre zone · 2 two-finger · 3 both |
| `MiddleZoneWidth` | 350 | Half width of the centre strip in touch units (the surface is about 2400 wide). Bigger means an easier middle click. |
| `ScrollEnabled` | 1 | Master switch for touch scrolling |
| `HorizontalScroll` | 1 | 0 = vertical only |
| `NaturalScrolling` | 1 | 1: content follows the finger (macOS). 0: behaves like a wheel. |
| `ScrollSpeed` | 100 | Percent, 10–500 |
| `Acceleration` | 1 | Faster swipes scroll further (up to 3×) |
| `Inertia` | 1 | Momentum after a flick |
| `AxisLock` | 1 | A clearly vertical or horizontal swipe is locked to that axis; diagonal swipes stay 2D |
| `ScrollThreshold` | 25 | How far a finger must move before it scrolls. This stops fingers resting on the shell from scrolling. |

Settings apply **live** — the driver re-reads them within about two seconds, with no device restart. (Changing a setting used to restart the mouse device, which could disturb other Bluetooth drivers on the system.) If you edit the registry by hand, bump `SettingsGeneration` to any new value to make the driver reload.

### Running alongside Magic Utilities or other Apple-device drivers

The driver never restarts the Bluetooth device during normal use (settings are live, and it re-arms multitouch on the existing connection). The one exception is the **one-time install**, which has to restart the mouse device once to attach the filter. Some third-party drivers — notably Magic Utilities' `MagicKeyboard.sys` — mishandle that brief Bluetooth disturbance and can bugcheck. If you have such a driver, turn your Apple keyboard **off** during the install (the installer reminds you). After install there are no further device restarts, so the two coexist cleanly.

## Using it

- **Scroll**: one finger, any direction. Fingers just resting on the mouse are ignored.
- **Flick**: lift while moving to get momentum. Touch the mouse to stop it.
- **Left / right click**: as usual; the mouse decides from where your finger is.
- **Middle click**: click in the centre (Zone mode) or with two fingers down (TwoFinger mode). The button picked when you press is held until you release, so middle-drag works.
- Scrolling is paused while a button is held, and fingers that were on the mouse during a click don't scroll until lifted. This avoids accidental scrolling when you click.

## Troubleshooting

- **Check it's loaded**: Device Manager → Bluetooth → your mouse → Properties → Details → "Lower filters" should list `vhf` and `MagicMouseFlt`.
- **Logs**: run Sysinternals **DebugView** as admin with *Capture Kernel* on. Lines start with `MagicMouseFlt:`. In Release builds, also set `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter`, value `IHVDRIVER` = DWORD `0xF`, and reboot. Useful lines include `channel open`, `sending multitouch enable`, `multitouch enable handshake result 0`, `multitouch reports active`, `resolution multiplier set to 0x05`.
- **Scrolling is in steps rather than smooth**: an app that only handles whole notches will show steps. Windows itself gets hi-res values once it logs `resolution multiplier set`.
- **Wrong direction**: `-NaturalScrolling $false`.
- **Too fast or slow**: `-ScrollSpeed 60` … `-ScrollSpeed 200`.

## Uninstall

```powershell
.\tools\Uninstall.ps1
```
The mouse goes back to the in-box driver. To leave test mode afterwards, run `bcdedit /set testsigning off` and reboot.

**Recovery**: this is kernel code. If a bug ever crashes Windows when the mouse connects, switch the mouse off, boot, and run `Uninstall.ps1`. If you can't get that far, boot into Safe Mode, where Bluetooth devices don't start, and uninstall from there.

## Status and caveats

This driver was written without access to the hardware, so treat the first run as a test:

- The protocol parser and gesture engine are unit-tested: run `test/` on any OS with a C compiler (see the comment at the top of `test/test_gesture.c`). That covers decoding of every report format, scrolling in both axes, direction, axis lock, inertia, resting-finger rejection, all middle-click modes, button latching and low-res fallback.
- The kernel parts passed a compile check against stub headers, not a real WDK build or a run on a real machine. The parts most likely to need adjustment on real hardware:
  - how many bytes a completed L2CAP read reports (`BufferSize - RemainingBufferSize`);
  - telling the control channel from the interrupt channel when the mouse (not the PC) opens the connection;
  - swallowing the handshake that answers the multitouch enable packet.

  The debug log lines cover each of these.
- Bluetooth only. Over a USB cable the Magic Mouse 2/USB-C behaves as a regular mouse.

## Layout

```
MagicMouseFlt.inf        INF: HidBth + lower filters vhf, MagicMouseFlt; default settings
MagicMouseFlt.vcxproj    WDK project (KMDF 1.15, x64/ARM64, test-signed)
src/driver.c             DriverEntry, device add, PnP/power, settings, VHF device
src/bthfilter.c          BRB interception, channel tracking, multitouch enable, report routing
src/gesture.c/.h         touch → buttons/scroll/inertia/middle click (portable C)
src/mmproto.c/.h         Magic Mouse report parsing, enable packets, HID descriptor (portable C)
src/mmtypes.h            shared types (kernel or user mode)
test/test_gesture.c      unit tests for gesture.c and mmproto.c
tools/*.ps1              install, options, uninstall
```
