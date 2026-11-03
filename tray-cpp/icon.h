#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// Tray icon glyph.
//
// Rendered with GDI, not GDI+. This matters more than the choice of font: at 16 px
// the GDI rasterizer is what the shell itself uses for the taskbar clock, and its
// hinting is visibly crisper than anything GDI+ produces - GDI+ AntiAliasGridFit
// still comes out soft by comparison.
//
// GDI cannot write an alpha channel, so the glyph is drawn white-on-black into a
// 32bpp DIB and the alpha is recovered from the coverage afterwards, then
// premultiplied into the wanted colour. That is what lets a hinted GDI glyph become
// a transparent icon.
namespace trayicon {

struct Face {
    const wchar_t* name;
    int weight;      // FW_NORMAL, FW_SEMIBOLD, ...
    DWORD quality;   // ANTIALIASED_QUALITY, CLEARTYPE_QUALITY, NONANTIALIASED_QUALITY
    bool subpixel;   // keep ClearType per-channel coverage instead of averaging it
    float gamma;     // <1 thickens antialiased coverage; 1.0 leaves it linear
};

// Tahoma, not Segoe UI. Segoe UI is the right face everywhere else in this app, but
// at 16 px its 7 is a sub-pixel-width diagonal: hinting can snap a vertical stem onto
// a whole pixel and cannot snap a diagonal, so 7 renders visibly lighter than 0 or 5
// and looks out of place. Tahoma was hand-hinted for exactly this size and resolves
// diagonals into full-intensity pixel staircases, so every digit carries equal
// weight. Verdana does the same but is too wide for three digits.
//
// On ClearType: an icon carries one alpha per pixel, so true subpixel coverage
// cannot be represented. It can be approximated - see `subpixel` below - by putting
// the per-channel coverage in the colour and the strongest channel in alpha. The
// compositor then blends all three channels with that single alpha, so the fringes
// come out as garish orange and blue rather than the subtle thing ClearType does.
// Rendered and rejected; do not re-litigate without looking at build-preview output.
//
// Gamma stays at 1.0: Tahoma's hinting already yields full-coverage pixels, so the
// correction is a no-op here. It earns its keep only with a softer-hinting face.
constexpr Face kDefaultFace{L"Tahoma", FW_NORMAL, ANTIALIASED_QUALITY, false, 1.0f};

// A hair of margin keeps glyphs off the icon edge, but three digits need every pixel
// of the 16 px width, so they get none.
inline int MarginFor(const wchar_t* text) {
    return lstrlenW(text) >= 3 ? 0 : 1;
}

inline HFONT CreateFace(int em, const Face& face) {
    return CreateFontW(-em, 0, 0, 0, face.weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       face.quality, DEFAULT_PITCH | FF_DONTCARE, face.name);
}

// Ink extent of a digit string with the current font, i.e. the advance width less
// the side bearings at each end. Those bearings are blank, and at 16 px giving them
// away costs two or three pixels of glyph - enough to matter for "100".
struct Extent {
    int advance;  // pen movement, including the blank side bearings
    int ink;      // width of the actual marks
    int bearing;  // left side bearing, to offset by when drawing
    int height;   // cap height; digits are lining figures so this is the ink height
};

inline Extent MeasureInk(HDC dc, const wchar_t* text, int len) {
    SIZE advance{};
    TEXTMETRICW tm{};
    GetTextExtentPoint32W(dc, text, len, &advance);
    GetTextMetricsW(dc, &tm);

    ABC abc[10]{};
    int left = 0, right = 0;
    if (GetCharABCWidthsW(dc, L'0', L'9', abc)) {
        left = abc[text[0] - L'0'].abcA;
        right = abc[text[len - 1] - L'0'].abcC;
    }

    Extent e{};
    e.advance = advance.cx;
    e.ink = advance.cx - left - right;
    e.bearing = left;
    e.height = tm.tmAscent - tm.tmInternalLeading;
    return e;
}

// Largest em that fits the box.
//
// One and two digit values are fitted on the advance width, which keeps the glyph a
// comfortable size with air around it. Three digits are fitted on the ink instead:
// the side bearings are blank and giving them away costs two or three pixels, which
// at this size is the difference between "100" being readable and not. Fitting
// everything on ink makes single digits balloon until they fill the icon edge to
// edge and go soft, so the concession is deliberately limited to the case that needs
// it.
inline int FitEm(HDC dc, const wchar_t* text, int size, const Face& face) {
    const int len = lstrlenW(text);
    const bool tight = len >= 3;
    const int maxW = size - MarginFor(text) * 2;

    // Ceiling of `size`: beyond that, one and two digit values grow until they fill
    // the icon edge to edge and lose their hinting. Three digits are width-bound
    // well below it, so the cap costs them nothing.
    for (int em = size; em >= 4; --em) {
        HFONT font = CreateFace(em, face);
        HGDIOBJ previous = SelectObject(dc, font);
        Extent e = MeasureInk(dc, text, len);
        SelectObject(dc, previous);
        DeleteObject(font);

        if ((tight ? e.ink : e.advance) <= maxW && e.height <= size)
            return em;
    }
    return 4;
}

// Fills `out` with premultiplied BGRA, top-down, size*size pixels.
inline void RenderBgra(int size, DWORD value, bool dark_taskbar, const Face& face,
                       std::vector<DWORD>& out) {
    wchar_t text[8];
    swprintf_s(text, L"%u", value);
    const int len = lstrlenW(text);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    out.assign(static_cast<size_t>(size) * size, 0u);
    if (!dib || !dc) {
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        return;
    }

    HGDIOBJ previous_bmp = SelectObject(dc, dib);
    ZeroMemory(bits, static_cast<size_t>(size) * size * 4);  // black, fully covered by nothing

    int em = FitEm(dc, text, size, face);
    HFONT font = CreateFace(em, face);
    HGDIOBJ previous_font = SelectObject(dc, font);

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    Extent e = MeasureInk(dc, text, len);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_LEFT | TA_TOP);

    // Centre the ink, not the advance box, and back off by the left side bearing so
    // the marks themselves land where intended.
    const int x = (size - e.ink) / 2 - e.bearing;
    const int baseline = (size + e.height) / 2;
    TextOutW(dc, x, baseline - tm.tmAscent, text, len);

    GdiFlush();

    // Coverage -> premultiplied BGRA.
    BYTE curve[256];
    for (int i = 0; i < 256; ++i) {
        float v = (face.gamma > 0.0f && face.gamma != 1.0f)
                      ? powf(i / 255.0f, face.gamma) * 255.0f
                      : static_cast<float>(i);
        curve[i] = static_cast<BYTE>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }

    const DWORD* src = static_cast<const DWORD*>(bits);
    const int fg = dark_taskbar ? 255 : 0;
    for (size_t i = 0; i < out.size(); ++i) {
        DWORD pixel = src[i];
        int b = pixel & 0xFF, g = (pixel >> 8) & 0xFF, r = (pixel >> 16) & 0xFF;

        if (face.subpixel) {
            // Strongest channel drives alpha; the per-channel coverage stays in the
            // colour, which is what produces the ClearType fringe.
            r = curve[r]; g = curve[g]; b = curve[b];
            int alpha = r > g ? (r > b ? r : b) : (g > b ? g : b);
            out[i] = (static_cast<DWORD>(alpha) << 24) |
                     (static_cast<DWORD>(fg * r / 255) << 16) |
                     (static_cast<DWORD>(fg * g / 255) << 8) |
                     static_cast<DWORD>(fg * b / 255);
        } else {
            int alpha = curve[(r + g + b) / 3];
            int premul = fg * alpha / 255;
            out[i] = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(premul) << 16) |
                     (static_cast<DWORD>(premul) << 8) | static_cast<DWORD>(premul);
        }
    }

    SelectObject(dc, previous_font);
    DeleteObject(font);
    SelectObject(dc, previous_bmp);
    DeleteObject(dib);
    DeleteDC(dc);
}

inline HICON MakeIcon(int size, DWORD value, bool dark_taskbar,
                      const Face& face = kDefaultFace) {
    if (size < 16) size = 16;

    std::vector<DWORD> pixels;
    RenderBgra(size, value, dark_taskbar, face, pixels);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP colour = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colour) return nullptr;
    memcpy(bits, pixels.data(), pixels.size() * sizeof(DWORD));

    // The alpha channel carries the shape, so the mask is just zeroes.
    std::vector<BYTE> mask_bits(static_cast<size_t>(size) * size / 8, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = colour;
    icon_info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&icon_info);

    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

// For the preview tool: same pixels, as a GDI+ bitmap.
inline void DrawGlyph(Gdiplus::Bitmap& bitmap, DWORD value, bool dark_taskbar,
                      const Face& face = kDefaultFace) {
    const int size = static_cast<int>(bitmap.GetWidth());
    std::vector<DWORD> pixels;
    RenderBgra(size, value, dark_taskbar, face, pixels);

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, size, size);
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeWrite,
                        PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
        return;
    for (int y = 0; y < size; ++y)
        memcpy(static_cast<BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride,
               pixels.data() + static_cast<size_t>(y) * size, static_cast<size_t>(size) * 4);
    bitmap.UnlockBits(&data);
}

}  // namespace trayicon
