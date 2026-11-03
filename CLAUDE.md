# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A minimal replacement for GIGABYTE OSD Sidekick that does one thing: set monitor
brightness. Two programs share one idea and one hard-won set of timing constants:

| path | what it is | toolchain |
| --- | --- | --- |
| `Program.cs` | `bright.exe`, the one-shot CLI | in-box `csc.exe`, **C# 5 only** |
| `tray-cpp/` | the resident tray app | MSVC 14.51, **C++20** |

A second tray app in C# 14 / WinForms (`tray-cs/`) was built as a side-by-side
comparison and **removed on 2026-09-11** once C++ had won on every axis that
differed: a third of the memory, a fifth of the startup, no runtime dependency, and
the same source-line count. The measurements are kept in README.md. Note the C# build
never suffered the flyout show/dismiss bugs — those were specific to the hand-rolled
Win32 shell integration, not to a deficiency it needed fixing for.

## Toolchain

Installed 2026-09-10: Visual Studio Community 2026 (18.10) with MSVC 14.51,
Windows SDK 10.0.26100, and .NET SDK 10.0.401.

`dotnet` works. Earlier notes in this file called it "broken" — that was a host
with runtimes but **no SDK installed**, which is now fixed. NuGet has only the
Visual Studio offline feed configured, so anything needing nuget.org (a
self-contained or single-file `publish`, any PackageReference) will fail to
restore until a nuget.org source is added.

`Program.cs` is still compiled by the in-box `csc.exe 4.8.9221.0`, which reports
"for C# 5". That is a hard ceiling, not a style choice: **keep `Program.cs` free of
string interpolation, `out var`, and expression-bodied members.** `tray-cpp` is under
no such limit — it is built by MSVC.

## Build and run

```
build.cmd                    # -> bright.exe
.\bright.exe                 # read
.\bright.exe 70              # set absolute
.\bright.exe +10             # relative, clamped to 0..max
.\bright.exe -10
.\bright.exe --list          # enumerate displays (-l)
.\bright.exe --monitor 1 70  # target a display from --list (-m); defaults to index 0

cd tray-cpp && .\build.cmd                  # -> tray-cpp/brighttray.exe
```

`tray-cpp/build.cmd` locates MSVC through `vswhere`; it does not hardcode a VS path.
It compiles **only `main.cpp`**: `ddc.h`, `engine.h` and `icon.h` are header-only and
the app is one translation unit, so a new `.cpp` file is not built until it is added
to the `cl` line. Libraries come in through `#pragma comment(lib, ...)` in `main.cpp`,
not through the build script.

`tray-cpp/build-preview.cmd` builds a separate throwaway tool,
`tray-cpp/obj/icon-preview.exe`, which renders the tray glyph to a PNG contact sheet
at real size and magnified, on both a dark and a light taskbar. It writes
`icon-preview.png` to the current directory, or to the path given as its one
argument. It is the only sane way to judge a 16 px icon — use it before and after
touching `icon.h`.

The tray app takes a single-instance mutex named `Local\BrightTray.SingleInstance`,
so a second copy exits silently rather than fighting the first over the DDC/CI bus.
The consequence when iterating: **stop the running copy before rebuilding and
relaunching** (`Stop-Process -Name brighttray`). Otherwise the link fails on a locked
`brighttray.exe`, or — if it succeeds — the new launch exits silently and you end up
testing the old build.

**Leave the tray running by launching it through Explorer**
(`explorer.exe <path>\brighttray.exe`), not with `Start-Process` from an agent shell.
A process started from the shell belongs to the Claude Code session's process tree
and is killed when that session restarts, which looks exactly like a crash. A
copy started from the shell is fine for a test that ends with the session.

### Verifying a change

There is no test suite. Verification is running against the attached monitor.

For the CLI, check the printed value and the exit code:

| code | meaning |
| --- | --- |
| 0 | ok |
| 1 | no DDC/CI display found |
| 2 | bad arguments or bad `--monitor` index |
| 3 | read failed |
| 4 | write failed after retries |

`.\bright.exe +0` is the cheap smoke test: it exercises the full write-and-read-back
path without visibly changing the panel. A healthy round trip is well under a second.

For the tray app, the end-to-end test that actually proves the engine works is to
synthesize the global hotkey and read the panel back **from a separate process**:
launch it, send Ctrl+Alt+Down via `keybd_event`, wait ~2 s, then run `bright.exe` and
confirm the value dropped by 5.

Beware that `bright.exe` and the tray app then both drive the same bus. If you change
brightness with the CLI while the tray app is resident, its cached target goes stale
and the next nudge moves from *its* value, not the panel's — which looks like an
off-by-one-step bug and is not. Settle state before measuring.

Note that Windows 11 parks new tray icons in the overflow area, so the icon glyph
will not be visible until it is dragged onto the taskbar.

## Settings

`tray-cpp` stores user settings under `HKCU\Software\BrightTray`:

| value | type | meaning |
| --- | --- | --- |
| `HotkeyMods` | DWORD | `RegisterHotKey` modifier mask for the Up/Down hotkeys. `0` disables them. Absent means the default, `MOD_CONTROL|MOD_ALT` (3). |

Run-at-login lives separately in the standard `HKCU\...\CurrentVersion\Run`, under
the value name `BrightTray`.

The tray menu also carries fixed **50% / 100%** presets, applied as a percentage of
each display's own reported maximum rather than as a raw value. They are plain
commands, deliberately **not** checkmarks, and go through `Engine::Force` rather than
`Set`. The panel can change behind the app's back — after deep sleep it may wake at
100% — so a checkmark derived from the cached value would lie. A plain `Set` would
also be skipped as a no-op whenever the stale cache happened to match the preset.

**The M27Q resets itself to 100 whenever the display wakes**, even after a 4 s
display-off. Measured by polling `bright.exe` after a wake: it answers DDC/CI at
once, reports a transitional value (50) for about 1 s, then jumps to 100 at about
1.4 s. So the tray registers for `GUID_CONSOLE_DISPLAY_STATE`, and on an off → on
transition it calls `Engine::Resync` at 2 s and again at 10 s. The worker then reads
every panel back and adopts what it finds. A channel with a pending or unverified
write is skipped, because user input wins. `Resync` deliberately does not set
`wake_`: that flag means "input arrived", and setting it would cut a quiet window
short and skip its verified write.

To test this, turn the displays off with `PostMessage(HWND_BROADCAST, WM_SYSCOMMAND,
SC_MONITORPOWER, 2)`, wake them with a one-pixel `mouse_event` move, and run the
tray with `BRIGHTTRAY_LOG=1`. The log shows `display state` and `resync` lines.

## Architecture

`tray-cpp/icon.h` owns the tray glyph and nothing else, so `icon-preview.cpp` can
render exactly what the app renders. Three things decide whether a 16 px number is
readable, and all three were settled by looking at rendered output, not by reasoning:

1. **Render with GDI, not GDI+.** This matters more than the font does. GDI is the
   rasterizer the shell itself uses, and its hinting at 16 px is visibly crisper than
   GDI+ at any `TextRenderingHint` — GDI+ `AntiAliasGridFit` still comes out soft.
   GDI cannot write alpha, so the glyph is drawn white-on-black into a 32bpp DIB and
   the alpha is recovered from coverage, then premultiplied into the wanted colour.
2. **`ANTIALIASED_QUALITY`** — GDI's *hinted* grayscale AA, which is a different
   thing from the GDI+ antialiasing this replaced. With Tahoma the setting is very
   nearly a no-op: its hinting already lands almost every pixel on full coverage, so
   `ANTIALIASED_QUALITY` and `NONANTIALIASED_QUALITY` render near-identically. AA is
   kept on because it costs nothing and softens the few genuinely partial pixels.

   Re-checked after the fitting fix below, in case undersized glyphs had been making
   things look worse than they were: Segoe UI is still soft under the corrected
   pipeline, so the mushiness was always the face's hinting at 16 px and never the
   sizing.

   **ClearType is not available here, and this has been measured, not assumed.** An
   icon carries one alpha per pixel, so subpixel coverage cannot be represented. The
   approximation — per-channel coverage in the colour, strongest channel in alpha —
   is implemented behind `Face::subpixel` and rendered for comparison: the fringes
   come out as garish orange and blue, because the compositor blends all three
   channels with the single alpha instead of each with its own. Averaging ClearType
   coverage to grey instead is simply softer than plain `ANTIALIASED_QUALITY`. Both
   were tried; neither is worth having. Do not re-litigate this without looking at
   `build-preview.cmd` output first.
3. **Tahoma, not Segoe UI.** Segoe UI is right everywhere else in this app, but at
   16 px its 7 is a sub-pixel-width diagonal, and since hinting can snap a vertical
   stem onto a whole pixel but cannot snap a diagonal, the 7 renders visibly lighter
   than 0 or 5 and looks out of place. Tahoma was hand-hinted for this size and turns
   diagonals into full-intensity pixel staircases, so every digit weighs the same.
   Verdana does likewise but is too wide for three digits. Arial Narrow is crisper
   still but reads harsh beside the rest of the shell; any bold weight closes up the
   counters in 0, 5 and 8. All were rendered and compared.

Font size is fitted by measuring the string, not taken as a fraction of the icon
size — a fixed `size * 0.66` leaves "5" small and shrinks "100" into mush. Two
details of that fit are load-bearing:

- **Three digits are fitted on ink width, everything else on advance width.**
  `GetTextExtentPoint32` returns the advance, which includes the blank side bearings
  at each end; subtracting them (via `GetCharABCWidthsW`) buys two or three pixels,
  which at 16 px is the difference between "100" being readable and not. Fitting
  *everything* on ink makes one and two digit values balloon to the icon edge and go
  soft, so the concession is limited to the case that needs it.
- **The em search starts at `size`, not higher.** Raising that ceiling lets short
  strings grow until cap height fills the icon, at which point they lose their
  hinting and look mushy. Three-digit values are width-bound well below it, so the
  cap costs them nothing.

`Face::gamma` boosts partial coverage, which lifts antialiased diagonals relative to
stems. It is set to 1.0 — a no-op — because Tahoma's hinting already produces
full-coverage pixels. It exists because it was the first attempt at the thin-7
problem, and it is worth reaching for again only if the face ever changes to one that
hints more softly. On Segoe UI it helped but could not fully close the gap: by the
time the 7 looked right, every other edge had bloated.

Three-digit values also drop the 1 px margin — at that width every pixel counts.

`Display` wraps the Windows Monitor Configuration API (`dxva2.dll`) and writes
**VCP code 0x10** (luminance) over plain DDC/CI. This exists twice
(`Program.cs`, `tray-cpp/ddc.h`) with identical semantics.

`DisplaySet` owns the `PHYSICAL_MONITOR` arrays and is the only thing that calls
`DestroyPhysicalMonitors`. Keep it that way: one HMONITOR can yield several
physical monitors sharing a single array, so per-`Display` disposal double-frees
it. `Display` deliberately holds nothing but a handle and a name.

In `Program.cs`, `DisplaySet.Open` passes a managed delegate to
`EnumDisplayMonitors`; the `GC.KeepAlive` on it is load-bearing, not decoration.

### The measured constraints

Three numbers drive every design decision here. They were measured on this panel;
the raw tables are in README.md.

1. **`SetVCPFeature` blocks for ~35-60 ms.** It is not a queued request — the call
   sits there while the I2C transaction runs over the display link. This is the
   floor on how fast brightness can possibly change, and it is identical in C# and
   C++ (measured 56.7 ms and 57.5 ms). No language or runtime choice moves it.
2. **The M27Q drops roughly every other DDC/CI transaction issued back to back.**
   Anything under a 60 ms gap is unreliable; 60 ms was the only spacing that landed
   5/5 in testing. `SettleMs = 60` is not a guess — do not trim it.
3. **`SetVCPFeature` returning `true` does not mean the write landed.** Writes are
   confirmed by reading the value back. Also prefer low-level `SetVCPFeature` over
   the `SetMonitorBrightness` wrapper, which fails consistently on this monitor.

### How the tray app stays responsive anyway

`bright::Engine` exists entirely to reconcile "instant" with a 60 ms floor:

- The UI updates **optimistically** and never waits on the panel.
- While input is arriving, each display holds a **single latest-desired slot, not a
  queue**. The worker fires *unverified* writes; each call self-paces at ~35 ms, so
  the panel tracks the slider and stale values are overwritten instead of backing up.
- **200 ms after input stops**, the worker does one authoritative write with
  read-back verification at the full 60 ms pacing. That is the only place
  correctness is enforced.

If you "optimise" this by verifying every write, dragging the slider will queue
seconds of backlog. If you drop verification entirely, values will silently fail to
stick. Both failure modes have already been hit.

Retries nest: a *failing* verified write costs ~4 s and a failing read ~0.5 s before
returning. That is the expected cost of a genuine failure, not a hang.

Threading: the engine's worker is the only thread that touches DDC/CI. Its callbacks
do nothing but `PostMessage` `WM_CONFIRMED` to the main window (`wparam = -1` for "the
target changed, repaint"). Every bit of UI work happens on the message thread. Keep
any new callback to the same rule. `Engine::Open` drops displays that fail their
first read, so a channel index is **not** the same as an index into
`DisplaySet::displays()`; go through `Channel::index`. `WM_DISPLAYCHANGE` and the
*Reload displays* menu item tear down and rebuild the whole engine and the flyout
window rather than patching them.

Only the tray *icon* is drawn with GDI. The flyout slider is painted with GDI+, and
the GDI-over-GDI+ reasoning above is about hinting 16 px text, not about the slider.

Debugging a "brightness does not stick" report starts with the three constraints
above, not with the VCP code. If reads fail outright, check that **DDC/CI is enabled
in the monitor OSD menu** — it is per-monitor and some firmware ships with it off.

## Gotchas already paid for

- **C++ registry paths use raw literals** (`LR"(Software\Microsoft\...)"`). An eaten
  backslash makes `RegOpenKeyEx` quietly return the fallback, so theme detection and
  run-at-login fail silently rather than erroring.
- **`GetHicon` hands back an HICON you own.** The tray app destroys the previous one
  after swapping it in; without that the handle count grows on every change.
- **Clicking the tray icon while the flyout is open** deactivates the flyout first,
  which hides it — so the click would immediately reopen it. Guarded with a 250 ms
  window.
- **The shell sends two tray events for one click.** With `NOTIFYICON_VERSION_4`
  a single left click delivers `WM_LBUTTONUP` (0x0202) *and* `NIN_SELECT` (0x0400)
  about 16 ms apart. Untreated, the second toggles the flyout straight back off and
  the window outline just flashes. Hence the ~200 ms debounce on `g_last_select`.
  The old WinForms build never saw this because `NotifyIcon` does not opt into
  version 4.

- **Dismiss the flyout from a `WH_MOUSE_LL` hook, never from `WA_INACTIVE`.** The
  Windows 11 taskbar and other shell surfaces are `WS_EX_NOACTIVATE`: clicking them
  does not move the foreground window, so no deactivation message is ever sent and
  an activation-based flyout cannot know it should close. The hook is installed in
  `ShowFlyout`, removed in `HideFlyout`, and posts `WM_DISMISS` when a button goes
  down outside the window rect. `WA_INACTIVE` is kept only as a fast path for
  Alt-Tab.

  Three activation-based schemes were tried first and all failed:
  - plain hide on `WA_INACTIVE` — cannot see clicks on non-activating surfaces;
  - a 400 ms *grace window* re-asserting foreground — when `SetForegroundWindow`
    loses, the window is visible but never activated, so no further `WA_INACTIVE`
    arrives and the flyout can never be dismissed at all;
  - a foreground-polling *timer* — fires on any transient foreground loss and closes
    the flyout ~470 ms after it opens.

- **`AttachThreadInput` wins foreground but not clean activation.** It is still used
  in `ForceForeground` so the flyout takes keyboard focus for Esc and arrow keys, but
  do not build dismissal on top of it: the window reports itself foreground while the
  system does not route deactivation to it. The symptom is oddly specific — the
  flyout dismisses correctly only *after* a click has landed inside it, because that
  click activates it properly via `WM_MOUSEACTIVATE`.

- **Most modifier combinations are not usable for global hotkeys.** Measured on this
  machine, for `VK_UP`/`VK_DOWN`:

  | combination | result |
  | --- | --- |
  | Ctrl+Alt | registers and delivers |
  | Win+Ctrl | registers and delivers |
  | Ctrl+Shift, Alt+Shift, Ctrl+Alt+Shift | register, but `WM_HOTKEY` never arrives |
  | Win+Alt, Win+Shift | `RegisterHotKey` fails, error 1409 (shell owns them) |

  Only the two verified ones are offered in the menu. Note the Shift results were
  obtained with `keybd_event`-synthesised input, where a zero scan code may not
  resolve Shift to left/right for the hotkey matcher; they may behave differently
  under real keypresses. Do not add a preset without testing that it *delivers*, not
  merely that it registers — `ApplyHotkeys` only checks the latter.

- **A global hotkey is taken from every other application.** Ctrl+Alt+Up/Down is
  Sublime Text multi-cursor, which is why the combination is configurable and can be
  switched off entirely. Default to leaving it available, not to grabbing keys.

- **Testing this needs synthesized real clicks, not `PostMessage`.** Posting
  `NIN_SELECT` produces a false pass: with nothing competing for foreground the
  flyout stays open, which looks identical to correct behaviour *and* to the
  cannot-dismiss bug. It fooled this repo twice. The harness in
  `tray-cpp/test-dismiss.ps1` drives `mouse_event` for real, and **must include a
  click on the empty taskbar** — clicking a normal window passes even when the bug
  is present. Set `BRIGHTTRAY_LOG=1` for a trace at `%TEMP%\brighttray.log`.

  The harness is tied to this machine and is not side-effect free: the taskbar click
  is at a fixed `(300, 1420)`, which assumes a 1440 px tall primary display; and it
  force-kills any running `brighttray` **and every `notepad` process** before
  starting. It also takes over the real mouse while it runs. It opens the flyout by
  posting `WM_TRAY` (`0x8001`) with `NIN_SELECT`. That is fine for *opening*; only
  the dismissal must be a real click. Run it with `powershell -File tray-cpp\test-dismiss.ps1` and read the
  table it prints: every case should show `DismissedAfterOutsideClick = True`.

## The vendor install

`C:\Program Files (x86)\GIGABYTE\OSDSidekick` is the stock app being replaced. It
contains **no source**, only binaries, and is worth opening only as protocol
reference. Note that it drives the panel over `libusb-win32` against a VIA Labs
billboard device (`USB\VID_2109&PID_8883`) — that entire stack is unnecessary for
brightness and should not be reintroduced here.

**`OSDSidekick.exe` may be running resident and will overwrite brightness behind
this tool.** If values change on their own, check for that process before
suspecting a bug in this code.
