$LOG = Join-Path $env:TEMP "brighttray.log"
$EXE = Join-Path $PSScriptRoot "brighttray.exe"

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class M {
    public delegate bool Proc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Proc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int t, bool r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(100);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(50);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Drag(int x1, int y1, int x2, int y2) {
        SetCursorPos(x1, y1);
        System.Threading.Thread.Sleep(100);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
        for (int i = 1; i <= 8; i++) {
            SetCursorPos(x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8);
            System.Threading.Thread.Sleep(40);
        }
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
    }
    public static IntPtr Find(uint target, string cls) {
        IntPtr r = IntPtr.Zero;
        EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target) { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
                if (sb.ToString() == cls) { r = h; return false; } } return true; }, IntPtr.Zero);
        return r; }
}
'@

function Restart-Tray {
    Get-Process brighttray -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 600
    if (Test-Path $LOG) { Clear-Content $LOG }
    $env:BRIGHTTRAY_LOG = "1"
    $script:proc = Start-Process $EXE -PassThru
    Start-Sleep -Seconds 2
    $script:main = [M]::Find([uint32]$script:proc.Id, "BrightTrayMain")
    $script:fly  = [M]::Find([uint32]$script:proc.Id, "BrightTrayFlyout")
}
function Open-Flyout {
    [void][M]::PostMessageW($script:main, 0x8001, [IntPtr]::Zero, [IntPtr]0x0400)
    Start-Sleep -Milliseconds 700
}
function Test-Visible { [M]::IsWindowVisible($script:fly) }
function Get-FlyRect { $r = New-Object M+RECT; [void][M]::GetWindowRect($script:fly, [ref]$r); $r }

# a real window to click on, parked out of the way of the flyout
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$np = Start-Process notepad -PassThru
Start-Sleep -Seconds 2
$npw = (Get-Process | Where-Object { $_.ProcessName -eq 'notepad' -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
[void][M]::MoveWindow($npw, 100, 100, 700, 500, $true)
# Let the helper window settle before the first case: moving it right as the tray app
# opens its flyout makes case A flake on Opened=False.
Start-Sleep -Milliseconds 1800
$saved = New-Object M+POINT; [void][M]::GetCursorPos([ref]$saved)

$results = @()

# --- A: open, then click outside ---
Restart-Tray; Open-Flyout
$a1 = Test-Visible
[M]::Click(450, 350)
Start-Sleep -Milliseconds 900
$results += [pscustomobject]@{ Case="A open -> click outside"; Opened=$a1; DismissedAfterOutsideClick=(-not (Test-Visible)) }

# --- B: open, click inside (padding, not the slider), then click outside ---
Restart-Tray; Open-Flyout
$b1 = Test-Visible
$r = Get-FlyRect
[M]::Click(($r.Left + 160), ($r.Top + 6))     # top padding strip, above the slider row
Start-Sleep -Milliseconds 400
[M]::Click(450, 350)
Start-Sleep -Milliseconds 900
$results += [pscustomobject]@{ Case="B open -> click inside padding -> outside"; Opened=$b1; DismissedAfterOutsideClick=(-not (Test-Visible)) }

# --- C: open, drag the slider, then click outside ---
Restart-Tray; Open-Flyout
$c1 = Test-Visible
$r = Get-FlyRect
$y = $r.Top + 30
[M]::Drag(($r.Left + 60), $y, ($r.Left + 120), $y)
Start-Sleep -Milliseconds 600
[M]::Click(450, 350)
Start-Sleep -Milliseconds 900
$results += [pscustomobject]@{ Case="C open -> drag slider -> outside"; Opened=$c1; DismissedAfterOutsideClick=(-not (Test-Visible)) }

# --- D: open, then click empty TASKBAR area (WS_EX_NOACTIVATE surface) ---
Restart-Tray; Open-Flyout
$d1 = Test-Visible
[M]::Click(300, 1420)
Start-Sleep -Milliseconds 900
$results += [pscustomobject]@{ Case="D open -> click taskbar"; Opened=$d1; DismissedAfterOutsideClick=(-not (Test-Visible)) }

# --- E: open, drag slider, then click TASKBAR ---
Restart-Tray; Open-Flyout
$e1 = Test-Visible
$r = Get-FlyRect
$y = $r.Top + 30
[M]::Drag(($r.Left + 60), $y, ($r.Left + 120), $y)
Start-Sleep -Milliseconds 600
[M]::Click(300, 1420)
Start-Sleep -Milliseconds 900
$results += [pscustomobject]@{ Case="E open -> drag slider -> taskbar"; Opened=$e1; DismissedAfterOutsideClick=(-not (Test-Visible)) }

# --- F: reopen after dismissal still works ---
Open-Flyout
$results += [pscustomobject]@{ Case="F reopen after dismissal"; Opened=(Test-Visible); DismissedAfterOutsideClick="n/a" }

[void][M]::SetCursorPos($saved.X, $saved.Y)
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
$results | Format-Table -AutoSize
""
"=== log from cases E/F ==="
Get-Content $LOG
