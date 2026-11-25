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
// A panel's picture fitted into (ix, iy, iw, ih): its crop at `aspect`, or the
// empty grey of a tile still to come. False on malformed pixels.
bool DrawTile (Graphics& g, const ReportPanel& t, float ix, float iy, float iw, float ih, float aspect)
{
    if (t.rgba.empty ()) {
        SolidBrush empty (Color (255, 243, 244, 244));
        g.FillRectangle (&empty, ix, iy, iw, ih);
        return true;
    }
    if (!t.width || !t.height || t.rgba.size () != size_t (t.width) * t.height * 4)
        return false;
    auto bgra = t.rgba;
    for (size_t k = 0; k < bgra.size (); k += 4)
        std::swap (bgra[k], bgra[k + 2]);
    Bitmap bitmap (t.width, t.height, t.width * 4, PixelFormat32bppARGB, bgra.data ());
    const float sw = (std::min) (float (t.width), t.height * aspect), sh = sw / aspect;
    return g.DrawImage (&bitmap, RectF (ix, iy, iw, ih), (t.width - sw) / 2, (t.height - sh) / 2, sw, sh, UnitPixel) ==
           Ok;
}

std::string Area (double m2)
{
    char text[32];
    std::snprintf (text, sizeof (text), m2 >= 10.0 ? "%.0f" : "%.1f", m2);
    return text;
}

// The planting page (#278): a row of layer views, each named above and its class
// shares below; the area table (layer x class, unavailable, measured); the
// notes; then the screenshot legend strip -- mini plan, north arrow, location,
// period and the class swatches -- pasted under the rule by the caller.
bool DrawPlanting (Graphics& g, Drawing& d, const LayoutReportPage& p, float W, float footer,
                   std::vector<PaperViewportRect>* viewports, float H)
{
    const float m = 54, gap = 18;
    const unsigned cols = unsigned (p.panels.size ());
    d.Text (p.subtitle, m, 96, W - 2 * m, 36, 22);
    const float rowH = 30, tableH = rowH * float (p.areaRows.size () + 1) + 16;
    const float notesH = 24.0f * float (p.notes.size ()) + (p.notes.empty () ? 0 : 8);
    const float top = 190, bottom = footer - tableH - notesH - 24;
    const float cw = (W - 2 * m - (cols - 1) * gap) / cols, maxH = bottom - top - 40;
    if (maxH < 60)
        return false;
    bool ok = true;
    for (unsigned c = 0; c < cols && ok; ++c) {
        const auto& t = p.panels[c];
        const float aspect = p.tileAspect > 0 ? p.tileAspect : t.width && t.height ? float (t.width) / t.height : 1.5f;
        const float x = m + c * (cw + gap);
        const float iw = (std::min) (cw, maxH * aspect), ih = iw / aspect, ix = x + (cw - iw) / 2,
                    iy = top + (maxH - ih) / 2;
        if (viewports)
            viewports->push_back ({ ix / W, iy / H, iw / W, ih / H });
        d.Text (t.date, ix, iy - 38, iw, 34, 22, true);
        ok = DrawTile (g, t, ix, iy, iw, ih, aspect);
        d.Text (t.metric, x, iy + ih + 4, cw, 30, (std::min) (17.f, cw / 16), false, true);
    }
    // The table: the layer column, then one per class, N/A and measured.
    const float tableTop = bottom + 12, nameW = 330;
    const size_t columns = p.classNames.size () + 2;
    const float colW = (W - 2 * m - nameW) / float (columns);
    Pen rule (Color (255, 204, 207, 208), 1);
    d.Text ("Plan area, m²", m, tableTop, nameW, rowH, 17, true);
    for (size_t k = 0; k < columns; ++k) {
        const std::string head = k < p.classNames.size ()    ? p.classNames[k]
                                 : k == p.classNames.size () ? "N/A"
                                                             : "Measured";
        d.Text (head, m + nameW + float (k) * colW, tableTop, colW, rowH, 17, true);
    }
    g.DrawLine (&rule, m, tableTop + rowH, W - m, tableTop + rowH);
    for (size_t r = 0; r < p.areaRows.size (); ++r) {
        const auto& row = p.areaRows[r];
        const float y = tableTop + rowH * float (r + 1) + 4;
        d.Text (row.name, m, y, nameW, rowH, 17);
        for (size_t k = 0; k < columns; ++k) {
            const double value = k < row.classAreas.size ()  ? row.classAreas[k]
                                 : k == p.classNames.size () ? row.unavailable
                                                             : row.measured;
            d.Text (Area (value), m + nameW + float (k) * colW, y, colW, rowH, 17);
        }
    }
    float y = tableTop + tableH;
    for (const std::string& note : p.notes) {
        d.Text (note, m, y, W - 2 * m, 24, 15);
        y += 24;
    }
    return ok;
}

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
    const bool planting = p.type == ReportTemplate::Planting;
    const unsigned cols = planting ? unsigned (p.panels.size ())
                          : matrix ? unsigned (p.clocks.size ())
                                   : 3,
                   rows = matrix ? 3 : 1;
    if (!cols || cols > (planting ? unsigned (kPlantingMaxTiles) : 9u) || p.panels.size () != size_t (cols) * rows)
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
        if (planting) {
            const float footer = H - (p.legendHeight ? p.legendHeight / scale + 12 : 85);
            ok = DrawPlanting (g, d, p, W, footer, viewports, H);
            Pen rule (Color (255, 204, 207, 208), 1);
            g.DrawLine (&rule, m, footer, W - m, footer);
            if (!p.legendHeight)
                d.Text (p.site, m, footer + 5, 710, 55, 17);
        }
        else {
            d.Text (matrix ? "Measured receiver area in shade at each local time"
                    : p.type == ReportTemplate::Patterns
                        ? "Distribution of measured shadows before and after solar noon"
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
                if (!DrawTile (g, t, ix, iy, iw, ih, aspect)) {
                    ok = false;
                    break;
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
