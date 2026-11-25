#include "ArchViz/LayoutReportPage.hpp"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdio>
#pragma comment(lib, "gdiplus.lib")

namespace geomsrv::archviz {
namespace {
using namespace Gdiplus;
struct Drawing {
    Graphics& g;
    FontFamily family { L"Segoe UI" };
    void Text (const std::string& s, float x, float y, float w, float h, float size, bool bold = false,
               bool centre = false)
    {
        if (s.empty ())
            return;
        const int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str (), -1, nullptr, 0);
        std::wstring ws (n, L'\0');
        MultiByteToWideChar (CP_UTF8, 0, s.c_str (), -1, ws.data (), n);
        Font font (&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        SolidBrush brush (bold ? Color (255, 34, 37, 39) : Color (255, 67, 72, 75));
        StringFormat fmt;
        fmt.SetFormatFlags (StringFormatFlagsNoWrap);
        fmt.SetAlignment (centre ? StringAlignmentCenter : StringAlignmentNear);
        fmt.SetLineAlignment (StringAlignmentCenter);
        g.DrawString (ws.c_str (), -1, &font, RectF (x, y, w, h), &fmt, &brush);
    }
    void Key (float& x, float y, ARGB rgb, const char* name, float width)
    {
        SolidBrush brush { Color (rgb) };
        g.FillRectangle (&brush, x, y + 6, 20.f, 14.f);
        Text (name, x + 27, y, width - 27, 26, 15);
        x += width;
    }
};
} // namespace
void StoreReportTile (ReportPanel& p, std::vector<uint8_t>& source)
{
    // Retain at most a page column of source detail across seasonal reruns.
    const unsigned n = (std::max) (p.width, p.height);
    if (n <= 1280) {
        p.rgba.swap (source);
        return;
    }
    const unsigned w = (std::max) (1u, 1280 * p.width / n), h = (std::max) (1u, 1280 * p.height / n);
    p.rgba.resize (size_t (w) * h * 4);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const unsigned a = x * p.width / w, b = (x + 1) * p.width / w, c = y * p.height / h,
                           d = (y + 1) * p.height / h;
            unsigned sums[4] = {};
            for (unsigned j = c; j < d; ++j)
                for (unsigned i = a; i < b; ++i)
                    for (int k = 0; k < 4; ++k)
                        sums[k] += source[(size_t (j) * p.width + i) * 4 + k];
            for (int k = 0; k < 4; ++k)
                p.rgba[(size_t (y) * w + x) * 4 + k] = uint8_t (sums[k] / ((b - a) * (d - c)));
        }
    p.width = w;
    p.height = h;
}
bool ComposeReportPage (const LayoutReportPage& p, std::vector<uint8_t>& rgba,
                        std::vector<PaperViewportRect>* viewports)
{
    using namespace Gdiplus;
    const bool matrix = p.type == ReportTemplate::Analysis;
    const unsigned cols = matrix ? unsigned (p.clocks.size ()) : 3, rows = matrix ? 3 : 1;
    if (!cols || cols > 9 || p.panels.size () != size_t (cols) * rows)
        return false;
    GdiplusStartupInput init;
    ULONG_PTR token = 0;
    if (GdiplusStartup (&token, &init, nullptr) != Ok)
        return false;
    const auto page = ReportPageFormat (p.format);
    rgba.assign (size_t (page.width) * page.height * 4, 255);
    bool ok = true;
    if (viewports)
        viewports->clear ();
    {
        Bitmap paper (page.width, page.height, page.width * 4, PixelFormat32bppARGB, rgba.data ());
        Graphics g (&paper);
        g.SetPageUnit (UnitPixel);
        g.SetInterpolationMode (InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint (TextRenderingHintAntiAliasGridFit);
        const float scale = float (page.width) / 1600, H = page.height / scale, W = 1600;
        g.ScaleTransform (scale, scale);
        Drawing d { g };
        const float m = 54, gap = 12, left = m + (matrix ? 30 : 0), cw = (W - m - left - (cols - 1) * gap) / cols;
        d.Text (ReportTitle (p.type), m, 32, 1100, 64, 42, true);
        d.Text (matrix                               ? "Measured receiver area in shade at each local time"
                : p.type == ReportTemplate::Patterns ? "Distribution of measured shadows before and after solar noon"
                                                     : "Hours of direct sunlight on measured receivers",
                m, 96, 1300, 36, 22);
        const float header = matrix ? 178 : 220, footer = H - (p.legendHeight ? p.legendHeight / scale + 12 : 85);
        const float rowH = (footer - header - 25) / rows;
        for (unsigned i = 0; i < p.panels.size (); ++i) {
            const auto& t = p.panels[i];
            const unsigned c = i % cols, r = i / cols;
            const float x = left + c * (cw + gap), y = header + r * rowH, maxH = rowH - 48;
            // Fit the whole crop, then anchor labels to the fitted picture edges.
            const float aspect = p.tileAspect > 0      ? p.tileAspect
                                 : t.width && t.height ? float (t.width) / t.height
                                                       : 1.5f;
            const float iw = (std::min) (cw, maxH * aspect), ih = iw / aspect, ix = x + (cw - iw) / 2,
                        iy = y + (maxH - ih) / 2;
            if (viewports)
                viewports->push_back ({ ix / W, iy / H, iw / W, ih / H });
            if (matrix && !r) {
                char clock[16];
                std::snprintf (clock, sizeof (clock), "%02d:%02d", p.clocks[c] / 60, p.clocks[c] % 60);
                d.Text (clock, x, iy - 36, cw, 34, 25, true, true);
            }
            if (matrix && !c) {
                auto save = g.Save ();
                g.TranslateTransform (ix - 8, iy + ih * .5f);
                g.RotateTransform (-90);
                d.Text (t.date + "  |  " + t.season, -ih * .5f, -24, ih, 24, 18, true, true);
                g.Restore (save);
            }
            if (!matrix) {
                d.Text (t.date + "  |  " + t.season, ix, iy - 64, iw, 32, 23, true);
                d.Text (t.window, ix, iy - 33, iw, 28, 17);
            }
            if (!t.rgba.empty ()) {
                if (!t.width || !t.height || t.rgba.size () != size_t (t.width) * t.height * 4) {
                    ok = false;
                    break;
                }
                auto bgra = t.rgba;
                for (size_t k = 0; k < bgra.size (); k += 4)
                    std::swap (bgra[k], bgra[k + 2]);
                Bitmap bitmap (t.width, t.height, t.width * 4, PixelFormat32bppARGB, bgra.data ());
                const float sw = (std::min) (float (t.width), t.height * aspect), sh = sw / aspect;
                if (g.DrawImage (&bitmap, RectF (ix, iy, iw, ih), (t.width - sw) / 2, (t.height - sh) / 2, sw, sh,
                                 UnitPixel) != Ok) {
                    ok = false;
                    break;
                }
            }
            else {
                SolidBrush empty (Color (255, 243, 244, 244));
                g.FillRectangle (&empty, ix, iy, iw, ih);
            }
            d.Text (t.metric, x, iy + ih + 2, cw, 32, (std::min) (18.f, cw / 14), false, true);
        }
        Pen rule (Color (255, 204, 207, 208), 1);
        g.DrawLine (&rule, m, footer, W - m, footer);
        if (!p.legendHeight) {
            d.Text (p.site, m, footer + 5, 710, 55, 17);
            float k = 800;
            if (p.pending) {
            }
            else if (matrix) {
                d.Key (k, footer + 18, 0xffeeeeee, "Sunlit", 150);
                d.Key (k, footer + 18, 0xff545454, "Shaded", 150);
            }
            else if (p.type == ReportTemplate::Patterns) {
                d.Key (k, footer + 18, 0xffeeeeee, "Neither", 145);
                d.Key (k, footer + 18, 0xff796ab1, "Morning", 155);
                d.Key (k, footer + 18, 0xffdf9792, "Evening", 155);
                d.Key (k, footer + 18, 0xffac819e, "Both", 130);
            }
            else {
                d.Text ("Direct sun (hours)", k, footer + 10, 190, 32, 16, true);
                k += 178;
                LinearGradientBrush ramp (PointF (k, 0), PointF (k + 310, 0), Color (255, 107, 55, 26),
                                          Color (255, 255, 224, 138));
                g.FillRectangle (&ramp, k, footer + 16, 310.f, 13.f);
                d.Text ("0 h", k, footer + 30, 100, 25, 15);
                d.Text ("9 h+", k + 273, footer + 30, 80, 25, 15);
            }
        }
        g.Flush (FlushIntentionSync);
    }
    GdiplusShutdown (token);
    for (size_t i = 0; i < rgba.size (); i += 4)
        std::swap (rgba[i], rgba[i + 2]);
    if (p.legendHeight) {
        if (p.legend.size () != size_t (page.width) * p.legendHeight * 4 || p.legend.size () > rgba.size ())
            return false;
        std::copy (p.legend.begin (), p.legend.end (), rgba.end () - p.legend.size ());
    }
    return ok;
}
} // namespace geomsrv::archviz
