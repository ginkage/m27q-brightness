// Renders the tray glyph to a PNG contact sheet so it can be judged at real size.
// Not part of the app; build it with build-preview.cmd when tuning icon.h.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>

#include "icon.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

static int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT num = 0, bytes = 0;
    GetImageEncodersSize(&num, &bytes);
    if (bytes == 0) return -1;
    ImageCodecInfo* info = static_cast<ImageCodecInfo*>(malloc(bytes));
    if (!info) return -1;
    GetImageEncoders(num, bytes, info);
    int found = -1;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(info[i].MimeType, format) == 0) { *clsid = info[i].Clsid; found = i; break; }
    free(info);
    return found;
}

int wmain(int argc, wchar_t** argv) {
    ULONG_PTR token = 0;
    GdiplusStartupInput startup;
    GdiplusStartup(&token, &startup, nullptr);

    const DWORD values[] = {0, 7, 30, 47, 50, 70, 100};
    const int count = ARRAYSIZE(values);

    const int icon = 16;
    const int zoom = 7;
    const int cell = icon * zoom;
    const int gap = 10;
    const int half = gap + cell + gap + icon + gap;

    const int width = gap + count * (cell + gap);
    const int height = 2 * half;

    {
    Bitmap sheet(width, height, PixelFormat32bppARGB);
    {
        Graphics g(&sheet);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetSmoothingMode(SmoothingModeNone);

        SolidBrush dark(Color(255, 32, 32, 32));
        SolidBrush light(Color(255, 243, 243, 243));
        g.FillRectangle(&dark, 0, 0, width, half);
        g.FillRectangle(&light, 0, half, width, height - half);

        for (int pass = 0; pass < 2; ++pass) {
            bool on_dark = (pass == 0);
            int top = gap + pass * half;
            for (int i = 0; i < count; ++i) {
                Bitmap glyph(icon, icon, PixelFormat32bppPARGB);
                trayicon::DrawGlyph(glyph, values[i], on_dark);

                int x = gap + i * (cell + gap);
                g.DrawImage(&glyph, Rect(x, top, cell, cell), 0, 0, icon, icon, UnitPixel);
                for (int k = 0; k < 3; ++k)
                    g.DrawImage(&glyph, Rect(x + k * (icon + 5), top + cell + gap, icon, icon),
                                0, 0, icon, icon, UnitPixel);
            }
        }
    }

    CLSID png;
    if (GetEncoderClsid(L"image/png", &png) >= 0) {
        const wchar_t* out = (argc > 1) ? argv[1] : L"icon-preview.png";
        sheet.Save(out, &png, nullptr);
        wprintf(L"wrote %s (%dx%d)\n", out, width, height);
    } else {
        wprintf(L"no PNG encoder\n");
    }

    }

    GdiplusShutdown(token);
    return 0;
}
