// Tray brightness control, Win32 + GDI+.
//
// Tray icon, flyout slider, presets and configurable global hotkeys, over the same
// coalescing DDC/CI engine the CLI uses.

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>

#include "engine.h"
#include "icon.h"

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

namespace {

constexpr UINT WM_TRAY      = WM_APP + 1;
constexpr UINT WM_CONFIRMED = WM_APP + 2;
constexpr UINT WM_DISMISS   = WM_APP + 3;

constexpr int IDM_RUNATLOGIN = 100;
constexpr int IDM_RELOAD     = 101;
constexpr int IDM_EXIT       = 102;

// Global hotkeys are system-wide: whatever combination is chosen is taken away
// from every other application, so it is configurable and can be turned off.
constexpr int IDM_PRESET_50  = 120;
constexpr int IDM_PRESET_100 = 121;

constexpr int IDM_HK_OFF        = 110;
constexpr int IDM_HK_CTRL_ALT   = 111;
constexpr int IDM_HK_WIN_CTRL   = 112;

// Only combinations verified to both register AND deliver WM_HOTKEY are offered.
// Anything with MOD_SHIFT registers but never fires; MOD_WIN with Alt or Shift is
// already owned by the shell (ERROR_HOTKEY_ALREADY_REGISTERED).
constexpr UINT kModsOff     = 0;
constexpr UINT kModsCtrlAlt = MOD_CONTROL | MOD_ALT;
constexpr UINT kModsWinCtrl = MOD_WIN | MOD_CONTROL;

constexpr int HOTKEY_UP   = 1;
constexpr int HOTKEY_DOWN = 2;
constexpr int kStep = 5;

// Flyout metrics at 96 dpi. ScaleMetrics derives the
// g* values actually used for layout, so the panel follows the monitor it opens on.
constexpr int kWidth  = 320;
constexpr int kEdge   = 14;
constexpr int kRowH   = 32;
constexpr int kRowGap = 6;
constexpr int kNameH  = 20;
constexpr int kReadoutW = 40;
constexpr int kThumbR = 9;
constexpr int kPad    = kThumbR + 2;
constexpr int kTrackH = 4;
constexpr int kFontPx = 12;

int gWidth = kWidth, gEdge = kEdge, gRowH = kRowH, gRowGap = kRowGap;
int gNameH = kNameH, gReadoutW = kReadoutW, gThumbR = kThumbR;
int gPad = kPad, gTrackH = kTrackH;
REAL gFontPx = static_cast<REAL>(kFontPx);

void ScaleMetrics(UINT dpi) {
    if (dpi == 0) dpi = 96;
    gWidth    = MulDiv(kWidth, dpi, 96);
    gEdge     = MulDiv(kEdge, dpi, 96);
    gRowH     = MulDiv(kRowH, dpi, 96);
    gRowGap   = MulDiv(kRowGap, dpi, 96);
    gNameH    = MulDiv(kNameH, dpi, 96);
    gReadoutW = MulDiv(kReadoutW, dpi, 96);
    gThumbR   = MulDiv(kThumbR, dpi, 96);
    gPad      = gThumbR + MulDiv(2, dpi, 96);
    gTrackH   = MulDiv(kTrackH, dpi, 96);
    gFontPx   = static_cast<REAL>(MulDiv(kFontPx, dpi, 96));
}

constexpr int DWMWA_USE_IMMERSIVE_DARK_MODE_ATTR = 20;
constexpr int DWMWA_WINDOW_CORNER_PREFERENCE_ATTR = 33;
constexpr int DWMWCP_ROUND_VALUE = 2;

HINSTANCE g_instance = nullptr;
HWND g_main = nullptr;
HWND g_flyout = nullptr;
HICON g_icon = nullptr;
ULONG_PTR g_gdiplus = 0;
std::unique_ptr<bright::Engine> g_engine;
bool g_flyout_visible = false;
ULONGLONG g_hidden_at = 0;
ULONGLONG g_shown_at = 0;
ULONGLONG g_last_select = 0;
HHOOK g_mouse_hook = nullptr;
UINT g_hotkey_mods = kModsCtrlAlt;
int g_drag_row = -1;

void Log(const wchar_t* format, ...) {
    static int enabled = -1;
    if (enabled < 0) {
        wchar_t probe[8];
        enabled = GetEnvironmentVariableW(L"BRIGHTTRAY_LOG", probe, 8) > 0 ? 1 : 0;
    }
    if (enabled != 1) return;

    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    lstrcatW(path, L"brighttray.log");

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a, ccs=UTF-8") != 0 || !file) return;

    va_list args;
    va_start(args, format);
    fwprintf(file, L"[%llu] ", GetTickCount64());
    vfwprintf(file, format, args);
    fputwc(10, file);  // LF
    va_end(args);
    fclose(file);
}

// ---------------------------------------------------------------- theme ----

DWORD ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return fallback;
    DWORD value = fallback, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS
        || type != REG_DWORD)
        value = fallback;
    RegCloseKey(key);
    return value;
}

void WriteDword(const wchar_t* sub, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
        != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

const wchar_t kPersonalize[] = LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)";
const wchar_t kRunKey[] = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
const wchar_t kRunValue[] = L"BrightTray";
const wchar_t kSettingsKey[] = LR"(Software\BrightTray)";
const wchar_t kHotkeyModsValue[] = L"HotkeyMods";

bool AppsAreDark()    { return ReadDword(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", 1) == 0; }
bool TaskbarIsDark()  { return ReadDword(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", 0) == 0; }

Color Background() { return AppsAreDark() ? Color(255, 32, 32, 32)  : Color(255, 249, 249, 249); }
Color Foreground() { return AppsAreDark() ? Color(255, 255, 255, 255) : Color(255, 26, 26, 26); }
Color Muted()      { return AppsAreDark() ? Color(255, 154, 154, 154) : Color(255, 96, 96, 96); }
Color TrackColor() { return AppsAreDark() ? Color(255, 72, 72, 72) : Color(255, 206, 206, 206); }

Color Accent() {
    DWORD abgr = ReadDword(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\DWM)", L"AccentColor", 0);
    if (abgr == 0) return Color(255, 0, 120, 212);
    // DWM stores AABBGGRR, not AARRGGBB.
    return Color(255, static_cast<BYTE>(abgr & 0xFF),
                      static_cast<BYTE>((abgr >> 8) & 0xFF),
                      static_cast<BYTE>((abgr >> 16) & 0xFF));
}

// ----------------------------------------------------------------- icon ----

HICON MakeIcon(DWORD value) {
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (size < 16) size = 16;
    return trayicon::MakeIcon(size, value, TaskbarIsDark());
}

// --------------------------------------------------------------- layout ----

bool ShowNames() { return g_engine && g_engine->count() > 1; }

int RowPitch() { return (ShowNames() ? gNameH : 0) + gRowH + gRowGap; }

int RowTop(int index) {
    return gEdge + index * RowPitch() + (ShowNames() ? gNameH : 0);
}

int FlyoutHeight() {
    int count = g_engine ? g_engine->count() : 1;
    return gEdge + count * RowPitch() - gRowGap + gEdge;
}

RECT SliderRect(int index) {
    RECT r;
    r.left = gEdge;
    r.top = RowTop(index);
    r.right = gWidth - gEdge - gReadoutW - 6;
    r.bottom = r.top + gRowH;
    return r;
}

int RowFromPoint(POINT pt) {
    if (!g_engine) return -1;
    for (int i = 0; i < g_engine->count(); ++i) {
        RECT r = SliderRect(i);
        if (pt.y >= r.top && pt.y <= r.bottom) return i;
    }
    return -1;
}

DWORD ValueFromX(int index, int x) {
    RECT r = SliderRect(index);
    int usable = (r.right - r.left) - gPad * 2;
    if (usable < 1) usable = 1;
    double fraction = static_cast<double>(x - r.left - gPad) / usable;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    return static_cast<DWORD>(fraction * g_engine->MaxOf(index) + 0.5);
}

// -------------------------------------------------------------- painting ---

void FillRounded(Graphics& g, Brush& brush, int x, int y, int w, int h) {
    if (w <= 0) return;
    int d = (h < w) ? h : w;
    GraphicsPath path;
    path.AddArc(x, y, d, d, 90.0f, 180.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 180.0f);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

void PaintFlyout(Graphics& g) {
    g.Clear(Background());
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    if (!g_engine) return;

    Font label(L"Segoe UI", gFontPx, FontStyleRegular, UnitPixel);
    SolidBrush foreground(Foreground());
    SolidBrush muted(Muted());
    SolidBrush track(TrackColor());
    SolidBrush accent(Accent());
    SolidBrush core(Background());

    for (int i = 0; i < g_engine->count(); ++i) {
        RECT r = SliderRect(i);
        DWORD value = g_engine->ValueOf(i);
        DWORD maximum = g_engine->MaxOf(i);

        if (ShowNames()) {
            StringFormat left;
            left.SetAlignment(StringAlignmentNear);
            left.SetLineAlignment(StringAlignmentCenter);
            RectF box(static_cast<REAL>(gEdge + 2), static_cast<REAL>(r.top - gNameH),
                      static_cast<REAL>(gWidth - gEdge * 2), static_cast<REAL>(gNameH));
            const std::wstring& name = g_engine->NameOf(i);
            g.DrawString(name.c_str(), -1, &label, box, &left, &muted);
        }

        int usable = (r.right - r.left) - gPad * 2;
        int cy = r.top + gRowH / 2;
        double fraction = (maximum == 0) ? 0.0 : static_cast<double>(value) / maximum;
        int thumbX = r.left + gPad + static_cast<int>(fraction * usable + 0.5);

        FillRounded(g, track, r.left + gPad, cy - gTrackH / 2, usable, gTrackH);
        if (thumbX > r.left + gPad)
            FillRounded(g, accent, r.left + gPad, cy - gTrackH / 2, thumbX - r.left - gPad, gTrackH);

        g.FillEllipse(&accent, thumbX - gThumbR, cy - gThumbR, gThumbR * 2, gThumbR * 2);
        int inner = (g_drag_row == i) ? 3 : 5;
        g.FillEllipse(&core, thumbX - inner, cy - inner, inner * 2, inner * 2);

        wchar_t text[8];
        wsprintfW(text, L"%u", value);
        StringFormat right;
        right.SetAlignment(StringAlignmentFar);
        right.SetLineAlignment(StringAlignmentCenter);
        RectF box(static_cast<REAL>(gWidth - gEdge - gReadoutW), static_cast<REAL>(r.top),
                  static_cast<REAL>(gReadoutW), static_cast<REAL>(gRowH));
        g.DrawString(text, -1, &label, box, &right, &foreground);
    }
}

// ---------------------------------------------------------------- flyout ---

LRESULT CALLBACK MouseHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_flyout && g_flyout_visible) {
        bool press = (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN ||
                      wparam == WM_MBUTTONDOWN || wparam == WM_NCLBUTTONDOWN);
        if (press) {
            const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
            RECT rc{};
            if (info && GetWindowRect(g_flyout, &rc) && !PtInRect(&rc, info->pt))
                PostMessageW(g_flyout, WM_DISMISS, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void ForceForeground(HWND hwnd) {
    HWND fore = GetForegroundWindow();
    if (fore == hwnd) return;

    DWORD other = GetWindowThreadProcessId(fore, nullptr);
    DWORD self = GetCurrentThreadId();
    bool attached = (other != 0 && other != self && AttachThreadInput(self, other, TRUE));

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (attached) AttachThreadInput(self, other, FALSE);
}

void HideFlyout() {
    if (!g_flyout) return;
    if (g_mouse_hook) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
    }
    ShowWindow(g_flyout, SW_HIDE);
    g_flyout_visible = false;
    g_hidden_at = GetTickCount64();
    Log(L"hide");
}

void ShowFlyout() {
    if (!g_flyout || !g_engine) return;

    ScaleMetrics(GetDpiForWindow(g_flyout));

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);

    int height = FlyoutHeight();
    int x = info.rcWork.right - gWidth - 12;
    int y = info.rcWork.bottom - height - 12;

    g_shown_at = GetTickCount64();
    SetWindowPos(g_flyout, HWND_TOPMOST, x, y, gWidth, height, SWP_NOACTIVATE);
    ShowWindow(g_flyout, SW_SHOW);
    ForceForeground(g_flyout);
    InvalidateRect(g_flyout, nullptr, FALSE);
    g_flyout_visible = true;
    if (!g_mouse_hook)
        g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_instance, 0);
    Log(L"show at %d,%d %dx%d fg=%p self=%p hook=%p", x, y, gWidth, height,
        GetForegroundWindow(), g_flyout, g_mouse_hook);
}

void ApplyFlyoutChrome(HWND hwnd) {
    BOOL dark = AppsAreDark() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_ATTR, &dark, sizeof(dark));
    int corner = DWMWCP_ROUND_VALUE;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE_ATTR, &corner, sizeof(corner));
}

LRESULT CALLBACK FlyoutProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP buffer = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ previous = SelectObject(mem, buffer);
        {
            Graphics g(mem);
            PaintFlyout(g);
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, previous);
        DeleteObject(buffer);
        DeleteDC(mem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int row = RowFromPoint(pt);
        if (row >= 0) {
            g_drag_row = row;
            SetCapture(hwnd);
            g_engine->Set(row, ValueFromX(row, pt.x));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_drag_row >= 0) {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            g_engine->Set(g_drag_row, ValueFromX(g_drag_row, pt.x));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_drag_row >= 0) {
            ReleaseCapture();
            g_drag_row = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        int row = RowFromPoint(pt);
        if (row < 0) row = 0;
        int steps = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (steps != 0 && g_engine) {
            g_engine->Nudge(row, steps * kStep);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            HideFlyout();
            return 0;
        }
        if ((wparam == VK_LEFT || wparam == VK_RIGHT) && g_engine) {
            g_engine->Nudge(0, wparam == VK_RIGHT ? 1 : -1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;

    case WM_DISMISS:
        Log(L"dismiss: click outside");
        HideFlyout();
        return 0;

    case WM_ACTIVATE:
        Log(L"activate=%d dt=%llu", (int)LOWORD(wparam), GetTickCount64() - g_shown_at);
        if (LOWORD(wparam) == WA_INACTIVE) HideFlyout();
        return 0;

    case WM_DPICHANGED:
        ScaleMetrics(HIWORD(wparam));
        SetWindowPos(hwnd, nullptr, 0, 0, gWidth, FlyoutHeight(),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------ tray ---

NOTIFYICONDATAW g_nid{};

void UpdateIcon() {
    if (!g_engine || g_engine->count() == 0) return;

    DWORD value = g_engine->ValueOf(0);
    HICON fresh = MakeIcon(value);
    HICON stale = g_icon;

    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = fresh;
    wsprintfW(g_nid.szTip, L"Brightness %u", value);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    g_icon = fresh;
    if (stale) DestroyIcon(stale);
}

void CreateTray(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Brightness");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void Balloon(const wchar_t* title, const wchar_t* text) {
    g_nid.uFlags = NIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, title, ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo));
    g_nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = 0;
}

UINT LoadHotkeyMods() {
    return ReadDword(HKEY_CURRENT_USER, kSettingsKey, kHotkeyModsValue, kModsCtrlAlt);
}

// Re-registers from scratch, so it doubles as both apply and disable.
void ApplyHotkeys() {
    UnregisterHotKey(g_main, HOTKEY_UP);
    UnregisterHotKey(g_main, HOTKEY_DOWN);
    if (g_hotkey_mods == kModsOff) return;

    UINT mods = g_hotkey_mods | MOD_NOREPEAT;
    bool up = RegisterHotKey(g_main, HOTKEY_UP, mods, VK_UP) != FALSE;
    bool down = RegisterHotKey(g_main, HOTKEY_DOWN, mods, VK_DOWN) != FALSE;
    Log(L"hotkeys mods=0x%X up=%d down=%d", g_hotkey_mods, up ? 1 : 0, down ? 1 : 0);

    if (!up || !down) {
        // Almost always means another application already owns the combination.
        UnregisterHotKey(g_main, HOTKEY_UP);
        UnregisterHotKey(g_main, HOTKEY_DOWN);
        g_hotkey_mods = kModsOff;
        WriteDword(kSettingsKey, kHotkeyModsValue, kModsOff);
        Balloon(L"Brightness",
                L"That hotkey is already taken by another application. "
                L"Global hotkeys have been turned off - pick a different combination "
                L"from the tray menu.");
    }
}

void SetHotkeyMods(UINT mods) {
    g_hotkey_mods = mods;
    WriteDword(kSettingsKey, kHotkeyModsValue, mods);
    ApplyHotkeys();
}

bool IsRunAtLogin() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool found = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

void ToggleRunAtLogin() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
        != ERROR_SUCCESS)
        return;

    if (IsRunAtLogin()) {
        RegDeleteValueW(key, kRunValue);
    } else {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"";
        quoted += path;
        quoted += L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

DWORD PresetValue(int index, int percent) {
    return static_cast<DWORD>(MulDiv(static_cast<int>(g_engine->MaxOf(index)), percent, 100));
}

// Presets are plain commands, never checkmarks: the cached value cannot be trusted
// to match the panel (it may wake from deep sleep at 100%), so each one is forced
// through to the panel even when the cache says it is already there.
void ApplyPreset(int percent) {
    if (!g_engine) return;
    for (int i = 0; i < g_engine->count(); ++i)
        g_engine->Force(i, PresetValue(i, percent));
}

void ShowMenu(HWND hwnd, POINT pt) {
    HMENU hotkeys = CreatePopupMenu();
    AppendMenuW(hotkeys, MF_STRING | (g_hotkey_mods == kModsOff ? MF_CHECKED : 0),
                IDM_HK_OFF, L"Off");
    AppendMenuW(hotkeys, MF_STRING | (g_hotkey_mods == kModsCtrlAlt ? MF_CHECKED : 0),
                IDM_HK_CTRL_ALT, L"Ctrl + Alt + Up / Down");
    AppendMenuW(hotkeys, MF_STRING | (g_hotkey_mods == kModsWinCtrl ? MF_CHECKED : 0),
                IDM_HK_WIN_CTRL, L"Win + Ctrl + Up / Down");

    HMENU menu = CreatePopupMenu();
    UINT preset = g_engine ? 0 : MF_GRAYED;
    AppendMenuW(menu, MF_STRING | preset, IDM_PRESET_50, L"50%");
    AppendMenuW(menu, MF_STRING | preset, IDM_PRESET_100, L"100%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsRunAtLogin() ? MF_CHECKED : 0), IDM_RUNATLOGIN, L"Run at login");
    // Ownership of the submenu passes to the parent; DestroyMenu below frees both.
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hotkeys), L"Global hotkeys");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"Reload displays");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

const wchar_t kFlyoutClass[] = L"BrightTrayFlyout";

void Reload() {
    if (g_flyout) {
        DestroyWindow(g_flyout);
        g_flyout = nullptr;
        g_flyout_visible = false;
    }
    g_engine.reset();
    g_engine = bright::Engine::Open();

    if (!g_engine) {
        g_nid.uFlags = NIF_TIP;
        lstrcpyW(g_nid.szTip, L"No DDC/CI display found");
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return;
    }

    // Both callbacks just post: all UI work happens on the message thread.
    g_engine->set_on_confirmed([](int index, DWORD value) {
        PostMessageW(g_main, WM_CONFIRMED, static_cast<WPARAM>(index), static_cast<LPARAM>(value));
    });
    g_engine->set_on_target_changed([](int, DWORD) {
        PostMessageW(g_main, WM_CONFIRMED, static_cast<WPARAM>(-1), 0);
    });

    g_flyout = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kFlyoutClass, L"",
                               WS_POPUP, 0, 0, gWidth, FlyoutHeight(),
                               g_main, nullptr, g_instance, nullptr);
    if (g_flyout) ApplyFlyoutChrome(g_flyout);
    UpdateIcon();
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_TRAY: {
        UINT event = LOWORD(lparam);
        ULONGLONG now = GetTickCount64();
        Log(L"tray 0x%04X visible=%d sinceSelect=%llu sinceHide=%llu", event,
            g_flyout_visible ? 1 : 0, now - g_last_select, now - g_hidden_at);

        if (event == WM_LBUTTONUP || event == NIN_SELECT) {
            if (now - g_last_select < 200) {
                Log(L"  -> ignored: duplicate delivery");
                return 0;
            }
            g_last_select = now;
            // Clicking the icon while the flyout is open deactivates it first, which
            // hides it. Without this guard the same click would immediately reopen it.
            if (g_flyout_visible) {
                Log(L"  -> toggle closed");
                HideFlyout();
            } else if (now - g_hidden_at > 250) {
                Log(L"  -> opening");
                ShowFlyout();
            } else {
                Log(L"  -> ignored: reopen guard (hidden %llu ms ago)", now - g_hidden_at);
            }
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            POINT pt{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
            ShowMenu(hwnd, pt);
        }
        return 0;
    }

    case WM_HOTKEY: {
        Log(L"hotkey id=%d", (int)wparam);
        if (!g_engine) return 0;
        int delta = (wparam == HOTKEY_UP) ? kStep : -kStep;
        for (int i = 0; i < g_engine->count(); ++i)
            g_engine->Nudge(i, delta);
        return 0;
    }

    case WM_CONFIRMED:
        UpdateIcon();
        if (g_flyout && g_flyout_visible) InvalidateRect(g_flyout, nullptr, FALSE);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDM_PRESET_50:  ApplyPreset(50);    return 0;
        case IDM_PRESET_100: ApplyPreset(100);   return 0;
        case IDM_RUNATLOGIN: ToggleRunAtLogin(); return 0;
        case IDM_RELOAD:     Reload();           return 0;
        case IDM_EXIT:       DestroyWindow(hwnd); return 0;
        case IDM_HK_OFF:        SetHotkeyMods(kModsOff);       return 0;
        case IDM_HK_CTRL_ALT:   SetHotkeyMods(kModsCtrlAlt);   return 0;
        case IDM_HK_WIN_CTRL:   SetHotkeyMods(kModsWinCtrl);   return 0;
        }
        return 0;

    case WM_DISPLAYCHANGE:
        Reload();
        return 0;

    case WM_SETTINGCHANGE:
        if (g_flyout) ApplyFlyoutChrome(g_flyout);
        UpdateIcon();
        if (g_flyout && g_flyout_visible) InvalidateRect(g_flyout, nullptr, TRUE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    HANDLE only = CreateMutexW(nullptr, TRUE, LR"(Local\BrightTray.SingleInstance)");
    if (!only || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ScaleMetrics(GetDpiForSystem());

    g_instance = instance;
    GdiplusStartupInput startup;
    GdiplusStartup(&g_gdiplus, &startup, nullptr);

    const wchar_t kMainClass[] = L"BrightTrayMain";

    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.lpfnWndProc = MainProc;
    main_class.hInstance = instance;
    main_class.lpszClassName = kMainClass;
    RegisterClassExW(&main_class);

    WNDCLASSEXW flyout_class{};
    flyout_class.cbSize = sizeof(flyout_class);
    flyout_class.lpfnWndProc = FlyoutProc;
    flyout_class.hInstance = instance;
    flyout_class.lpszClassName = kFlyoutClass;
    flyout_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&flyout_class);

    g_main = CreateWindowExW(0, kMainClass, L"BrightTray", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    CreateTray(g_main);
    g_hotkey_mods = LoadHotkeyMods();
    ApplyHotkeys();
    Reload();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_main, HOTKEY_UP);
    UnregisterHotKey(g_main, HOTKEY_DOWN);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_icon) DestroyIcon(g_icon);
    g_engine.reset();
    GdiplusShutdown(g_gdiplus);
    ReleaseMutex(only);
    CloseHandle(only);
    return 0;
}
