# m27q-brightness

A minimal replacement for the one thing GIGABYTE OSD Sidekick is usually wanted for:
setting the monitor backlight brightness.

Tested against a **GIGABYTE M27Q** (EDID `GBT270D`) on Windows 11.

## Build

```
build.cmd
```

Uses the in-box .NET Framework compiler (`%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe`).
No SDK, no NuGet, no project file. Output is a single self-contained `bright.exe` (~8 KB).

## Usage

```
bright                 # print current brightness, e.g. "50   (0..100 on GIGABYTE M27Q)"
bright 70              # set to 70
bright +10             # relative, clamped to 0..max
bright -10
bright --list          # enumerate displays with their current brightness
bright --monitor 1 70  # target a specific display from --list
```

Exit codes: `0` ok, `1` no DDC/CI display, `2` bad arguments, `3` read failed, `4` write failed.

## How it works

Plain **DDC/CI** through the Windows Monitor Configuration API (`dxva2.dll`), writing
**VCP code 0x10** (luminance). No vendor driver, no USB access, no elevation.

This matters, because it is *not* what OSD Sidekick does. The stock app talks to the
monitor over `libusb-win32` against the panel's VIA Labs billboard device
(`USB\VID_2109&PID_8883`), which is why its installer ships `libusb0.dll`,
`GHidApi.dll`, `GLUSBFLT_SETUP_V3.13.exe` and a USB filter driver. None of that is
required just to move the backlight.

The M27Q reports MCCS 2.0 and advertises:

```
(prot(monitor)type(lcd)model(M27Q)cmds(01 02 03 07 0C 4E F3 E3)
 vcp(02 04 05 08 0B 0C 10 12 14(04 05 08 0B) 16 18 1A 8D 62 6C 6E 70 AC AE B6 C0
     C6 C8 C9 CA CC(...) D6(01 02 04) DF 60(11 12 0F))
 mswhql(1)mccs_ver(2.0)asset_eep(32)mpu_ver(01))
```

### The one real gotcha

**The M27Q drops roughly every other DDC/CI transaction when they are issued back to back.**
Measured over 10 set+readback cycles:

| inter-transaction delay | set ok | get ok | readback matched |
| --- | --- | --- | --- |
| 0 ms | 4/10 | 7/10 | 1/10 |
| 50 ms | 8/10 | 10/10 | 8/10 |
| 100 ms | 10/10 | 8/10 | 8/10 |

Two consequences, both handled in `Display`:

- Every transaction is followed by a **60 ms settle delay** and retried up to 8 times.
- **A `true` return from `SetVCPFeature` is not trustworthy on its own** — writes are
  confirmed by reading the value back.

Also note `SetMonitorBrightness` (the high-level wrapper) failed consistently here while
the lower-level `SetVCPFeature(0x10, …)` worked. The code uses the latter.

If reads fail outright, check that **DDC/CI is enabled in the monitor's own OSD menu** —
it is a per-monitor setting and some firmware ships with it off.

### If brightness changes on its own

The stock `OSDSidekick.exe` runs resident and pushes its own brightness to the monitor,
which will silently undo changes made here. Check for it before suspecting a bug:

```powershell
Get-Process OSDSidekick -ErrorAction SilentlyContinue
Stop-Process -Name OSDSidekick        # for the current session
```

## Tray app

`bright.exe` is a one-shot CLI. For everyday use there is a resident tray app:

```
cd tray-cpp && .\build.cmd      # C++20 / Win32 / GDI+, ~155 KB, no dependencies
```

Tray icon shows the current level. Left-click for a flyout with a slider (mouse wheel
works over it), right-click for the menu:

- **50% / 100%** presets, applied as a percentage of the display's reported maximum
- **Global hotkeys** submenu (see below)
- **Run at login**, **Reload displays**, **Exit**

Windows 11 puts new tray icons in the overflow area, so drag the icon onto the
taskbar to see the brightness readout.

The icon draws the current level as a number. At 16 px that is a genuinely hard
typesetting problem. It is drawn in Tahoma — hand-hinted for small screen sizes, so every digit carries
equal weight where Segoe UI leaves a thin, out-of-place 7 — and
rendered through **GDI** rather than GDI+ — GDI is what the shell uses and its
hinting at this size is far crisper — with hinted grayscale antialiasing. (ClearType
cannot be used: an icon has one alpha channel, which cannot carry subpixel coverage.) The font is fitted to the box by measurement so
"5" and "100" both fill it. `build-preview.cmd` renders a contact sheet for checking
any change to it.

### Global hotkeys are optional

A global hotkey is taken away from every other application — `Ctrl+Alt+Up/Down` is
Sublime Text multi-cursor, for instance. So the menu lets you change or disable it:

| choice | notes |
| --- | --- |
| Off | no keys are registered at all |
| Ctrl + Alt + Up / Down | the default |
| Win + Ctrl + Up / Down | free of the usual editor bindings |

The choice is stored in `HKCU\Software\BrightTray\HotkeyMods` and applied
immediately, no restart. If a combination turns out to be owned by another app,
registration fails, hotkeys switch themselves off and a balloon says so.

Only those two combinations are offered because they are the only ones measured to
both register *and* actually deliver `WM_HOTKEY`: `Win+Alt` and `Win+Shift` are
already owned by the shell, and anything involving Shift registers but never fires.

### Making it feel instant

A 60 ms floor per DDC/CI transaction is not something you can optimise away, so the
engine works around it instead:

- the UI updates optimistically and never waits on the panel;
- while you are dragging, each display keeps a single *latest desired* slot rather
  than a queue, and the worker fires unverified writes - each call self-paces at
  ~35 ms, so the panel tracks the slider and stale values get overwritten instead of
  backing up;
- 200 ms after input stops, one authoritative write with read-back verification at
  the full 60 ms pacing. That is the only place correctness is enforced, and by then
  nobody is watching the clock.

Verifying every write instead would queue seconds of backlog behind a slider drag.
Skipping verification would let values silently fail to stick. Both were tried.

### Why C++ and not C#

A second tray app was written in C# 14 / .NET 10 / WinForms, feature for feature, and
the two were measured against each other before the C# one was removed:

| | C# 14 / .NET 10 | C++20 / Win32 |
| --- | --- | --- |
| working set | 41.7 MB | **13.5 MB** |
| private bytes | 9.5 MB | **2.5 MB** |
| threads | 14 | **6** |
| handles | 284 | **146** |
| startup to message loop | 116 ms avg | **23 ms avg** |
| shipped bytes | 190 KB | **155 KB** |
| also needs installed | .NET 10 Desktop Runtime (~173 MB) | **nothing** |
| source lines | 955 | 948 |

The source-line row is the surprise: the Win32 version cost essentially nothing
extra, because most of the code in *both* was the hand-painted slider and the DDC/CI
engine, neither of which WinForms helps with.

What the numbers do not show is any difference in responsiveness, because there is
none. Both spent effectively all their time blocked inside `dxva2.dll` waiting on the
I2C round trip - 56.7 ms mean in C#, 57.5 ms in C++.

## Possible next steps

- Mouse wheel directly over the tray icon (needs `WH_MOUSE_LL` plus
  `Shell_NotifyIconGetRect`, which in turn needs registering the icon by GUID).
- Other VCP codes are available from the same `Display` class: contrast is `0x12`,
  input source `0x60`.
