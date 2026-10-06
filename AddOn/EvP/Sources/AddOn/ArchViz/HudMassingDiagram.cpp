#include "ArchViz/HudMassingDiagram.hpp"
#include "ArchViz/StorySliceGeometry.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::hudmassingrules {
void DrawDiagramProperty (ImDrawList& draw, ScreenPoint from, ScreenPoint to, float width, float fontSize, double& arc,
                          ProjectedDrawList* occupied)
{
    const double dx = double (to.x) - from.x, dy = double (to.y) - from.y, length = std::hypot (dx, dy);
    if (!std::isfinite (length) || length < 1e-6 || !std::isfinite (fontSize) || fontSize <= 0)
        return;
    const double scale = fontSize / 13, period = 26 * scale;
    if (length > 8192 * period)
        return;
    constexpr ImU32 red = IM_COL32 (170, 68, 101, 255);
    if (occupied)
        occupied->lines.push_back ({ from, to, red, width });
    const auto at = [&] (double position) {
        const double t = (position - arc) / length;
        return ImVec2 { float (from.x + t * dx), float (from.y + t * dy) };
    };
    for (double start = std::floor (arc / period) * period; start < arc + length; start += period) {
        const double low = (std::max) (arc, start), high = (std::min) (arc + length, start + 12 * scale);
        if (high > low)
            draw.AddLine (at (low), at (high), red, width);
        const double dot = start + 19 * scale;
        if (dot >= arc && dot < arc + length)
            draw.AddCircleFilled (at (dot), width / 2, red);
    }
    arc += length;
}

void DrawDiagramOffset (ImDrawList& draw, const std::vector<ScreenPoint>& points, float fontSize,
                        ProjectedDrawList* occupied)
{
    if (points.size () < 3 || !std::isfinite (fontSize) || fontSize <= 0)
        return;
    const double dash = 5 * fontSize / 13, period = 8 * fontSize / 13;
    double total = 0;
    for (size_t i = 0; i < points.size (); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size ()];
        if (!std::isfinite (a.x) || !std::isfinite (a.y))
            return;
        total += std::hypot (double (b.x) - a.x, double (b.y) - a.y);
    }
    if (!std::isfinite (total) || total > 8192 * period)
        return;
    double arc = 0;
    for (size_t i = 0; i < points.size (); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size ()];
        const double dx = double (b.x) - a.x, dy = double (b.y) - a.y, length = std::hypot (dx, dy);
        if (length < 1e-6)
            continue;
        if (occupied)
            occupied->lines.push_back ({ a, b, IM_COL32 (166, 98, 38, 255), 2 });
        for (double start = std::floor (arc / period) * period; start < arc + length; start += period) {
            const double low = (std::max) (arc, start), high = (std::min) (arc + length, start + dash);
            if (high <= low)
                continue;
            const auto at = [&] (double position) {
                const double t = (position - arc) / length;
                return ImVec2 { float (a.x + t * dx), float (a.y + t * dy) };
            };
            draw.AddLine (at (low), at (high), IM_COL32 (166, 98, 38, 255), 2);
        }
        arc += length;
    }
}

void DrawDiagramZeroOffset (ImDrawList& draw, const std::vector<ScreenPoint>& points, const std::vector<bool>& zero,
                            float fontSize)
{
    namespace cp = Clipper2Lib;
    if (points.size () < 3 || points.size () != zero.size () || points.size () > 2048 || !std::isfinite (fontSize) ||
        fontSize <= 0 || std::none_of (zero.begin (), zero.end (), [] (bool value) { return value; }) ||
        points.size () * size_t (std::count (zero.begin (), zero.end (), true)) > 1000000)
        return;
    const float depth = (std::min) (30.0f, 18 * fontSize / 13);
    const auto origin = points[0];
    cp::PathD polygon;
    for (const auto p : points) {
        if (!std::isfinite (p.x) || !std::isfinite (p.y))
            return;
        polygon.emplace_back (double (p.x) - origin.x, double (p.y) - origin.y);
    }
    const double area = cp::Area (polygon);
    if (std::abs (area) < 1e-6)
        return;
    std::vector<ScreenPoint> normals (points.size ());
    for (size_t i = 0; i < points.size (); ++i) {
        const auto a = points[i], b = points[(i + 1) % points.size ()];
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot (dx, dy);
        if (length > 1e-5f)
            normals[i] =
                area > 0 ? ScreenPoint { dy / length, -dx / length } : ScreenPoint { -dy / length, dx / length };
    }
    std::vector<ImDrawVert> vertices;
    const auto uv = ImGui::GetIO ().Fonts->TexUvWhitePixel;
    for (size_t i = 0; i < points.size (); ++i) {
        if (!zero[i])
            continue;
        const size_t next = (i + 1) % points.size (), previous = (i + points.size () - 1) % points.size ();
        const auto n = normals[i];
        if (std::hypot (n.x, n.y) < 0.5f)
            continue;
        const auto join = [&] (ScreenPoint other, bool enabled) {
            const float dot = n.x * other.x + n.y * other.y;
            if (!enabled || dot < -0.5f)
                return n;
            return ScreenPoint { (n.x + other.x) / (1 + dot), (n.y + other.y) / (1 + dot) };
        };
        const auto left = join (normals[previous], zero[previous]), right = join (normals[next], zero[next]);
        const auto a = polygon[i], b = polygon[next];
        const cp::PathD quad {
            a, b, { b.x + right.x * depth, b.y + right.y * depth }, { a.x + left.x * depth, a.y + left.y * depth }
        };
        // Local outward normals alone can cross the opposite wall of a narrow
        // concavity. Subtract the complete parcel so no yellow is painted inside it.
        const auto exterior = cp::Difference ({ quad }, { polygon }, cp::FillRule::EvenOdd, 3);
        std::vector<SliceChain> chains;
        for (const auto& path : exterior) {
            SliceChain chain;
            chain.closed = true;
            for (const auto& p : path)
                chain.xy.insert (chain.xy.end (), { p.x, p.y });
            chains.push_back (std::move (chain));
        }
        std::vector<StorySliceFillVertex> triangles;
        BuildSliceFill (chains, 0, triangles);
        for (const auto& p : triangles) {
            const double distance = (p.x - a.x) * n.x + (p.y - a.y) * n.y;
            const int alpha = int (std::lround (170 * (1 - std::clamp (distance / depth, 0.0, 1.0))));
            vertices.push_back ({ { p.x + origin.x, p.y + origin.y }, uv, IM_COL32 (255, 193, 70, alpha) });
        }
        if (vertices.size () > 24000)
            return;
    }
    draw.PrimReserve (int (vertices.size ()), int (vertices.size ()));
    for (const auto& vertex : vertices) {
        draw.PrimWriteIdx (ImDrawIdx (draw._VtxCurrentIdx));
        draw.PrimWriteVtx (vertex.pos, vertex.uv, vertex.col);
    }
}
} // namespace geomsrv::archviz::hudmassingrules
