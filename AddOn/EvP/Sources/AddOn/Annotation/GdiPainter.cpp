#include "Annotation/GdiPainter.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace geomsrv::annotation {
namespace {

POINT Project (const Point3& point, const Transform2D& transform, const GdiPaintOptions& options)
{
    return { options.originX + static_cast<LONG> (std::lround (transform.offX + point.x * transform.scaleX)),
             options.originY + static_cast<LONG> (std::lround (transform.offY + point.y * transform.scaleY)) };
}

void Line (HDC hdc, POINT from, POINT to)
{
    MoveToEx (hdc, from.x, from.y, nullptr);
    LineTo (hdc, to.x, to.y);
}

void Arrowhead (HDC hdc, POINT tail, POINT tip, double dpiScale)
{
    const double dx = static_cast<double> (tip.x - tail.x);
    const double dy = static_cast<double> (tip.y - tail.y);
    const double length = std::hypot (dx, dy);
    if (length < 1.0)
        return;
    const double ux = dx / length, uy = dy / length;
    const double along = 10.0 * dpiScale;
    const double across = 4.5 * dpiScale;
    POINT triangle[] = { tip,
                         { static_cast<LONG> (std::lround (tip.x - along * ux + across * uy)),
                           static_cast<LONG> (std::lround (tip.y - along * uy - across * ux)) },
                         { static_cast<LONG> (std::lround (tip.x - along * ux - across * uy)),
                           static_cast<LONG> (std::lround (tip.y - along * uy + across * ux)) } };
    Polygon (hdc, triangle, 3);
}

std::wstring Wide (const std::string& text)
{
    if (text.empty ())
        return {};
    const int count = MultiByteToWideChar (CP_UTF8, 0, text.data (), static_cast<int> (text.size ()), nullptr, 0);
    std::wstring result (static_cast<size_t> ((std::max) (count, 0)), L'\0');
    if (count > 0)
        MultiByteToWideChar (CP_UTF8, 0, text.data (), static_cast<int> (text.size ()), result.data (), count);
    return result;
}

void Text (HDC hdc, const std::string& text, POINT anchor, COLORREF colour, double dpiScale, bool centered = false,
           double fontPixels = 0.0)
{
    const std::wstring wide = Wide (text);
    if (wide.empty ())
        return;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    if (fontPixels > 0.0) {
        font = CreateFontW (-static_cast<int> (std::lround (fontPixels)), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        oldFont = SelectObject (hdc, font);
    }
    SetBkMode (hdc, OPAQUE);
    SetBkColor (hdc, RGB (255, 255, 255));
    SetTextColor (hdc, colour);
    SIZE extent {};
    GetTextExtentPoint32W (hdc, wide.c_str (), static_cast<int> (wide.size ()), &extent);
    const LONG x = centered ? anchor.x - extent.cx / 2 : anchor.x + static_cast<LONG> (std::lround (5.0 * dpiScale));
    const LONG y = centered ? anchor.y : anchor.y - extent.cy - static_cast<LONG> (std::lround (3.0 * dpiScale));
    TextOutW (hdc, x, y, wide.c_str (), static_cast<int> (wide.size ()));
    if (oldFont != nullptr)
        SelectObject (hdc, oldFont);
    if (font != nullptr)
        DeleteObject (font);
}

POINT ToPoint (const ScreenPoint& point)
{
    return { static_cast<LONG> (std::lround (point.x)), static_cast<LONG> (std::lround (point.y)) };
}

void FilledTriangle (HDC hdc, const ScreenTriangle& triangle)
{
    POINT points[] = { ToPoint (triangle.first), ToPoint (triangle.second), ToPoint (triangle.third) };
    Polygon (hdc, points, 3);
}

} // namespace

COLORREF SemanticColour (SemanticRole role)
{
    const ColourRgba colour = RoleColour (role);
    return RGB (colour.red, colour.green, colour.blue);
}

void PaintFrameGdi (HDC hdc, const Frame& frame, const Transform2D& transform, const GdiPaintOptions& options)
{
    if (hdc == nullptr)
        return;
    const double dpiScale =
        options.dpiScale > 0.0 ? options.dpiScale : (std::max) (1, GetDeviceCaps (hdc, LOGPIXELSX)) / 96.0;
    for (const Primitive& primitive : frame.primitives) {
        if (!IsDrawable (primitive) || primitive.kind == PrimitiveKind::Element)
            continue;
        const COLORREF colour = SemanticColour (primitive.role);
        HPEN pen = CreatePen (PS_SOLID, (std::max) (1, int (std::lround (options.lineWidth * dpiScale))), colour);
        HBRUSH brush = CreateSolidBrush (colour);
        HGDIOBJ oldPen = SelectObject (hdc, pen);
        HGDIOBJ oldBrush = SelectObject (hdc, brush);
        std::vector<POINT> points;
        points.reserve (primitive.points.size ());
        for (const Point3& point : primitive.points)
            points.push_back (Project (point, transform, options));

        if (primitive.kind == PrimitiveKind::Point) {
            const LONG radius = static_cast<LONG> (std::lround (4.0 * dpiScale));
            Ellipse (hdc, points[0].x - radius, points[0].y - radius, points[0].x + radius + 1,
                     points[0].y + radius + 1);
            Text (hdc, primitive.text, points[0], colour, dpiScale);
        }
        else if (primitive.kind == PrimitiveKind::Label) {
            Text (hdc, primitive.text, points[0], colour, dpiScale);
        }
        else if (primitive.kind == PrimitiveKind::Angle) {
            ArchitecturalAngleGlyph glyph;
            const POINT firstRay = primitive.direction ? points[2] : points[1];
            const POINT secondRay = primitive.direction ? points[1] : points[2];
            if (BuildArchitecturalAngleGlyph ({ double (points[0].x), double (points[0].y) },
                                              { double (firstRay.x), double (firstRay.y) },
                                              { double (secondRay.x), double (secondRay.y) }, dpiScale, glyph)) {
                HPEN arcPen = CreatePen (PS_SOLID, static_cast<int> (std::lround (glyph.arcWidthPixels)), colour);
                HGDIOBJ oldArcPen = SelectObject (hdc, arcPen);
                for (size_t segment = 1; segment < glyph.arc.size (); ++segment)
                    Line (hdc, ToPoint (glyph.arc[segment - 1]), ToPoint (glyph.arc[segment]));
                FilledTriangle (hdc, glyph.arrowheads[0]);
                FilledTriangle (hdc, glyph.arrowheads[1]);
                SelectObject (hdc, oldArcPen);
                DeleteObject (arcPen);
                char measured[64];
                std::snprintf (measured, sizeof (measured), "%.1f\xC2\xB0", glyph.degrees);
                Text (hdc, primitive.text.empty () ? measured : primitive.text, ToPoint (glyph.labelAnchor), colour,
                      dpiScale, true, glyph.fontSizePixels);
            }
        }
        else if (primitive.kind == PrimitiveKind::Dimension) {
            const double dx = static_cast<double> (points[1].x - points[0].x);
            const double dy = static_cast<double> (points[1].y - points[0].y);
            const double length = std::hypot (dx, dy);
            const double offset = primitive.offset != 0.0 ? primitive.offset * std::fabs (transform.scaleX) : 14.0;
            const double nx = length > 0.0 ? -dy / length : 0.0, ny = length > 0.0 ? dx / length : 0.0;
            POINT a = { static_cast<LONG> (std::lround (points[0].x + nx * offset)),
                        static_cast<LONG> (std::lround (points[0].y + ny * offset)) };
            POINT b = { static_cast<LONG> (std::lround (points[1].x + nx * offset)),
                        static_cast<LONG> (std::lround (points[1].y + ny * offset)) };
            Line (hdc, points[0], a);
            Line (hdc, points[1], b);
            Line (hdc, a, b);
            Arrowhead (hdc, b, a, dpiScale);
            Arrowhead (hdc, a, b, dpiScale);
            char measured[64];
            const Point3 delta { primitive.points[1].x - primitive.points[0].x,
                                 primitive.points[1].y - primitive.points[0].y,
                                 primitive.points[1].z - primitive.points[0].z };
            std::snprintf (measured, sizeof (measured), "%.3f m",
                           std::sqrt (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
            Text (hdc, primitive.text.empty () ? measured : primitive.text, { (a.x + b.x) / 2, (a.y + b.y) / 2 },
                  colour, dpiScale, true);
        }
        else {
            for (size_t index = 1; index < points.size (); ++index)
                Line (hdc, points[index - 1], points[index]);
            if (primitive.closed && points.size () > 2)
                Polygon (hdc, points.data (), static_cast<int> (points.size ()));
            if (primitive.kind == PrimitiveKind::Arrow || primitive.direction)
                Arrowhead (hdc, points[points.size () - 2], points.back (), dpiScale);
            Text (hdc, primitive.text, points.back (), colour, dpiScale);
        }
        SelectObject (hdc, oldBrush);
        SelectObject (hdc, oldPen);
        DeleteObject (brush);
        DeleteObject (pen);
    }
}

} // namespace geomsrv::annotation
