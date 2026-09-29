// ArchViz/OverlayScene -- see the header.

#include "ArchViz/OverlayScene.hpp"

#include "Annotation/DimensionGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>

namespace geomsrv {
namespace archviz {
namespace overlayscene {

namespace layers = overlaylayers;

namespace {

// Budgets on what one set of layers may turn into. A caller past them sees the
// excess counted in `truncated`, never an allocation that takes Archicad with it.
constexpr size_t kMaxFillVertices = 6000000;
constexpr size_t kMaxLines = 2000000;
constexpr size_t kMaxGlyphVertices = 1200000;

constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 Sub (const Vec3& a, const Vec3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
Vec3 Cross (const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
double Dot (const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double Length (const Vec3& a)
{
    return std::sqrt (Dot (a, a));
}
Vec3 At (const std::vector<double>& points, uint32_t index)
{
    const size_t i = size_t (index) * 3;
    return { points[i], points[i + 1], points[i + 2] };
}

// ---- the draft: one builder, two writers -------------------------------------

struct DraftVertex {
    double p[3] = {};
    float n[3] = { 0.0f, 0.0f, 1.0f };
    float offset[2] = {};
    uint32_t rgba = 0;
    float value = 0.0f;
};

struct DraftFill {
    FillDraw draw;
    std::vector<DraftVertex> vertices;
};

struct DraftLine {
    double a[3] = {}, b[3] = {};
    uint32_t rgba = 0;
    float width = 1.0f, dash = 0.0f, duty = 0.5f, arc = 0.0f;
    uint32_t behind = kBehindShow;
};

struct DraftGlyph {
    double anchor[3] = {};
    double dir[3] = {};
    float offset[2] = {};
    float uv[2] = {};
    uint32_t rgba = 0, halo = 0;
    float haloPixels = 0.0f;
    uint32_t flags = 0;
    float minSpan = 0.0f;
    uint32_t page = 0;
    uint32_t behind = kBehindShow;
};

struct Draft {
    std::vector<DraftFill> fills;
    std::vector<DraftLine> lines;
    std::vector<DraftGlyph> glyphs;
    size_t fillVertices = 0;
    Problems problems;
};

class Builder {
  public:
    Builder (layers::Views view, overlaytext::Engine* text) : view_ (view), text_ (text)
    {
    }

    Draft Take ()
    {
        return std::move (draft_);
    }

    void AddLayer (const layers::Layer& layer)
    {
        for (const layers::Mesh& mesh : layer.meshes)
            if (layers::DrawnByGuest (mesh))
                AddMesh (layer, mesh);
        for (const layers::Polyline& polyline : layer.polylines)
            if (layers::DrawnByGuest (polyline))
                AddPolyline (layer, polyline);
        for (const layers::Dimension& dimension : layer.dimensions)
            AddDimension (layer, dimension);
        for (const layers::Text& text : layer.texts)
            AddText (layer, text);
        for (const layers::Legend& legend : layer.legends)
            AddLegend (legend);
    }

  private:
    bool Plan () const
    {
        return view_ == layers::Views::TwoD;
    }

    // The 2D overlay has no depth (finding 13): everything there is drawn whole.
    uint32_t BehindOf (layers::Behind behind, const layers::Layer& layer) const
    {
        return Plan () ? kBehindShow : BehindCode (layers::Resolve (behind, layer));
    }

    void Truncated ()
    {
        ++draft_.problems.truncated;
    }

    void SetRamp (FillDraw& draw, const layers::Colormap& colormap, double min, double max) const
    {
        draw.heatmap = true;
        draw.stopCount = uint32_t ((std::min) (colormap.stops.size (), layers::kMaxStops));
        for (uint32_t i = 0; i < draw.stopCount; ++i) {
            draw.stopAt[i] = colormap.stops[i].at;
            draw.stopRgba[i] = colormap.stops[i].rgba;
        }
        draw.min = float (min);
        draw.max = float (max);
        draw.bands = colormap.bands;
        draw.isolineStep = float (colormap.isolineStep);
        draw.isolineRgba = colormap.isolineRgba;
        draw.isolineWidthPixels = colormap.isolineWidthPixels;
    }

    void AddMesh (const layers::Layer& layer, const layers::Mesh& mesh)
    {
        const size_t vertices = mesh.points.size () / 3;
        if (draft_.fillVertices + mesh.indices.size () > kMaxFillVertices) {
            Truncated ();
            return;
        }
        DraftFill fill;
        fill.draw.shading = uint32_t (mesh.style.shading);
        fill.draw.opacity = mesh.style.opacity;
        fill.draw.behind = BehindOf (mesh.style.behind, layer);
        fill.draw.cullBack = mesh.style.cullBack;
        if (!mesh.values.empty ()) {
            double min = mesh.colormap.min, max = mesh.colormap.max;
            if (mesh.colormap.autoRange) {
                const auto range = std::minmax_element (mesh.values.begin (), mesh.values.end ());
                min = *range.first;
                max = *range.second;
            }
            if (!(max > min))
                max = min + 1.0;
            SetRamp (fill.draw, mesh.colormap, min, max);
        }
        const std::vector<double> computed =
            mesh.normals.empty () ? VertexNormals (mesh.points, mesh.indices) : std::vector<double> ();
        const std::vector<double>& normals = mesh.normals.empty () ? computed : mesh.normals;
        fill.vertices.reserve (mesh.indices.size ());
        for (const uint32_t index : mesh.indices) {
            DraftVertex v;
            const size_t at = size_t (index) * 3;
            v.p[0] = mesh.points[at];
            v.p[1] = mesh.points[at + 1];
            v.p[2] = mesh.points[at + 2];
            v.n[0] = float (normals[at]);
            v.n[1] = float (normals[at + 1]);
            v.n[2] = float (normals[at + 2]);
            v.rgba = mesh.vertexRgba.empty () ? mesh.rgba : mesh.vertexRgba[index];
            v.value = mesh.values.empty () ? 0.0f : float (mesh.values[index]);
            fill.vertices.push_back (v);
        }
        draft_.fillVertices += fill.vertices.size ();
        draft_.fills.push_back (std::move (fill));

        if ((mesh.style.edgeRgba & 0xFFu) != 0 && vertices >= 3) {
            const uint32_t behind = BehindOf (mesh.style.behind, layer);
            for (const auto& edge : FeatureEdges (mesh.points, mesh.indices, mesh.style.edgeAngleDegrees)) {
                DraftLine line;
                const Vec3 a = At (mesh.points, edge.first), b = At (mesh.points, edge.second);
                Assign (line.a, a);
                Assign (line.b, b);
                line.rgba = mesh.style.edgeRgba;
                line.width = mesh.style.edgeWidthPixels;
                line.behind = behind;
                if (!PushLine (line))
                    break;
            }
        }
    }

    void AddPolyline (const layers::Layer& layer, const layers::Polyline& polyline)
    {
        const size_t points = polyline.points.size () / 3;
        const size_t segments = polyline.closed ? points : points - 1;
        const uint32_t behind = BehindOf (polyline.behind, layer);
        double arc = 0.0;
        for (size_t i = 0; i < segments; ++i) {
            const Vec3 a = At (polyline.points, uint32_t (i));
            const Vec3 b = At (polyline.points, uint32_t ((i + 1) % points));
            Vec3 d = Sub (b, a);
            if (Plan ())
                d.z = 0.0;
            const double length = Length (d);
            if (length < 1e-9)
                continue;
            DraftLine line;
            Assign (line.a, a);
            Assign (line.b, b);
            line.rgba = polyline.rgba;
            line.width = polyline.widthPixels;
            line.dash = polyline.dashPixels;
            line.duty = polyline.dashDuty;
            line.arc = float (arc);
            line.behind = behind;
            if (!PushLine (line))
                return;
            arc += length;
        }
    }

    void AddDimension (const layers::Layer& layer, const layers::Dimension& dimension)
    {
        annotation::AlignedDimensionInput input;
        input.first = { dimension.from[0], dimension.from[1], dimension.from[2] };
        input.second = { dimension.to[0], dimension.to[1], dimension.to[2] };
        const Vec3 normal = { dimension.normal[0], dimension.normal[1], dimension.normal[2] };
        if (Length (normal) > 1e-12)
            input.planes.explicitNormal = annotation::Point3 { normal.x, normal.y, normal.z };
        // A horizontal dimension measures in plan; one the plan cannot tell from a
        // point -- vertical -- stands upright, facing x.
        input.planes.declaredNormal = annotation::Point3 { 0.0, 0.0, 1.0 };
        input.planes.cameraFacingNormal = annotation::Point3 { 1.0, 0.0, 0.0 };
        const Vec3 direction = { dimension.direction[0], dimension.direction[1], dimension.direction[2] };
        if (Length (direction) > 1e-12)
            input.planes.preferredOffsetDirection = annotation::Point3 { direction.x, direction.y, direction.z };
        input.explicitOffset = dimension.offsetMetres;
        const annotation::DimensionStyle style;
        const std::optional<annotation::ResolvedDimensionGeometry> resolved =
            annotation::ResolveAlignedDimensionGeometry (input, style);
        if (!resolved.has_value ()) {
            ++draft_.problems.dimensionsNotResolved;
            draft_.problems.lastError = "a dimension's geometry could not be resolved";
            return;
        }
        const Vec3 first = { resolved->dimensionFirst.x, resolved->dimensionFirst.y, resolved->dimensionFirst.z };
        const Vec3 second = { resolved->dimensionSecond.x, resolved->dimensionSecond.y, resolved->dimensionSecond.z };
        Vec3 span = Sub (second, first);
        if (Plan ()) {
            span.z = 0.0;
            // Seen from above, a vertical dimension is a point: nothing to draw.
            if (Length (span) < 1e-9)
                return;
        }
        const uint32_t behind = BehindOf (dimension.behind, layer);

        auto line = [&] (const annotation::Point3& a, const annotation::Point3& b) {
            DraftLine l;
            Assign (l.a, Vec3 { a.x, a.y, a.z });
            Assign (l.b, Vec3 { b.x, b.y, b.z });
            l.rgba = dimension.rgba;
            l.width = dimension.widthPixels;
            l.behind = behind;
            PushLine (l);
        };
        line (resolved->dimensionFirst, resolved->dimensionSecond);
        line (resolved->witnessFirstStart, resolved->witnessFirstEnd);
        line (resolved->witnessSecondStart, resolved->witnessSecondEnd);

        // The terminators turn with the dimension line's projection; the text also
        // keeps upright and gives way when the dimension is too short on screen to
        // hold it between its ends.
        const float width = (std::max) (dimension.widthPixels, 1.25f);
        AddTerminator (first, span, dimension.terminator, true, width, dimension.rgba, behind);
        AddTerminator (second, span, dimension.terminator, false, width, dimension.rgba, behind);

        const std::string text = !dimension.text.empty () ? dimension.text
                                                          : FormatLength (resolved->measurement, dimension.decimals,
                                                                          dimension.unit, dimension.showUnit);
        const Vec3 middle = { (first.x + second.x) * 0.5, (first.y + second.y) * 0.5, (first.z + second.z) * 0.5 };
        overlaytext::Label label;
        if (!LayOut (text, dimension.textSizePixels, layers::Align::Center, layers::Baseline::Bottom, label))
            return;
        const float gap = 3.0f;
        const float minSpan = (label.right - label.left) + 2.0f * 10.0f;
        for (const overlaytext::Quad& quad : label.quads) {
            DraftGlyph glyph;
            Assign (glyph.anchor, middle);
            Assign (glyph.dir, span);
            glyph.rgba = dimension.rgba;
            glyph.halo = 0x000000A0u;
            glyph.haloPixels = 1.25f;
            glyph.flags = kAlongDirection | kKeepUpright | kHideShortSpan;
            glyph.minSpan = minSpan;
            glyph.behind = behind;
            PushQuad (glyph, quad, 0.0f, -gap);
        }
    }

    // A tick, an arrowhead or a dot at one end of a dimension line, in the local
    // frame whose +x is the dimension's direction on screen.
    void AddTerminator (const Vec3& at, const Vec3& span, layers::Terminator terminator, bool first, float width,
                        uint32_t rgba, uint32_t behind)
    {
        DraftGlyph glyph;
        Assign (glyph.anchor, at);
        Assign (glyph.dir, span);
        glyph.rgba = rgba;
        glyph.flags = kAlongDirection | kSolid;
        glyph.behind = behind;
        float corners[6][2];
        if (terminator == layers::Terminator::Arrow) {
            // The tip on the end, the body inside the line: outward arrows.
            const float length = 10.0f, half = 3.5f, back = first ? length : -length;
            const float tri[3][2] = { { 0.0f, 0.0f }, { back, -half }, { back, half } };
            const int order[6] = { 0, 1, 2, 2, 2, 2 };
            for (int i = 0; i < 6; ++i) {
                corners[i][0] = tri[order[i]][0];
                corners[i][1] = tri[order[i]][1];
            }
        }
        else if (terminator == layers::Terminator::Dot) {
            const float r = (std::max) (2.5f, width * 1.5f);
            const float quad[4][2] = { { 0.0f, -r }, { r, 0.0f }, { 0.0f, r }, { -r, 0.0f } };
            const int order[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; ++i) {
                corners[i][0] = quad[order[i]][0];
                corners[i][1] = quad[order[i]][1];
            }
        }
        else {
            // The architectural slash: 45 degrees through the end, 9 px long.
            const float h = 4.5f * 0.70710678f, w = width * 0.5f * 0.70710678f;
            // From lower left to upper right (screen y is down), widened across.
            const float quad[4][2] = { { -h - w, h - w }, { h - w, -h - w }, { h + w, -h + w }, { -h + w, h + w } };
            const int order[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; ++i) {
                corners[i][0] = quad[order[i]][0];
                corners[i][1] = quad[order[i]][1];
            }
        }
        for (int i = 0; i < 6; ++i) {
            if (!PushGlyphVertex (glyph, corners[i][0], corners[i][1], -1.0f, -1.0f))
                return;
        }
    }

    void AddText (const layers::Layer& layer, const layers::Text& text)
    {
        overlaytext::Label label;
        if (!LayOut (text.text, text.sizePixels, text.align, text.baseline, label))
            return;
        DraftGlyph glyph;
        if (text.screen) {
            glyph.anchor[0] = text.at[0];
            glyph.anchor[1] = text.at[1];
            glyph.flags = kScreenAnchored;
            glyph.behind = kBehindShow;
        }
        else {
            glyph.anchor[0] = text.at[0];
            glyph.anchor[1] = text.at[1];
            glyph.anchor[2] = text.at[2];
            glyph.behind = BehindOf (text.behind, layer);
        }
        glyph.halo = text.haloRgba;
        glyph.haloPixels = text.haloPixels;
        const double radians = double (text.rotationDegrees) * kPi / 180.0;
        const float c = float (std::cos (radians)), s = float (std::sin (radians));
        auto place = [&] (float x, float y, float& ox, float& oy) {
            // Counter-clockwise on a y-down screen, then the caller's offset.
            ox = x * c + y * s + text.offsetPixels[0];
            oy = -x * s + y * c + text.offsetPixels[1];
        };
        if ((text.backgroundRgba & 0xFFu) != 0 && !label.quads.empty ()) {
            const float scale = text.sizePixels / 18.0f;
            const float left = label.left - 4.0f * scale, right = label.right + 4.0f * scale;
            const float top = label.top - 3.0f * scale, bottom = label.bottom + 3.0f * scale;
            DraftGlyph panel = glyph;
            panel.rgba = text.backgroundRgba;
            panel.flags |= kSolid;
            const float quad[4][2] = { { left, top }, { right, top }, { right, bottom }, { left, bottom } };
            const int order[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; ++i) {
                float ox = 0.0f, oy = 0.0f;
                place (quad[order[i]][0], quad[order[i]][1], ox, oy);
                PushGlyphVertex (panel, ox, oy, -1.0f, -1.0f);
            }
        }
        glyph.rgba = text.rgba;
        for (const overlaytext::Quad& quad : label.quads) {
            const float corners[4][4] = { { quad.left, quad.top, quad.u0, quad.v0 },
                                          { quad.right, quad.top, quad.u1, quad.v0 },
                                          { quad.right, quad.bottom, quad.u1, quad.v1 },
                                          { quad.left, quad.bottom, quad.u0, quad.v1 } };
            const int order[6] = { 0, 1, 2, 0, 2, 3 };
            glyph.page = quad.page;
            for (int i = 0; i < 6; ++i) {
                float ox = 0.0f, oy = 0.0f;
                place (corners[order[i]][0], corners[order[i]][1], ox, oy);
                if (!PushGlyphVertex (glyph, ox, oy, corners[order[i]][2], corners[order[i]][3]))
                    return;
            }
        }
    }

    void AddLegend (const layers::Legend& legend)
    {
        const bool right = legend.corner == layers::Corner::TopRight || legend.corner == layers::Corner::BottomRight;
        const bool bottom = legend.corner == layers::Corner::BottomLeft || legend.corner == layers::Corner::BottomRight;
        const double fx = right ? 1.0 : 0.0, fy = bottom ? 1.0 : 0.0;
        const float sx = right ? -1.0f : 1.0f, sy = bottom ? -1.0f : 1.0f;
        const float titleRoom = legend.title.empty () ? 0.0f : legend.sizePixels * 1.6f;
        // The bar's rectangle in pixels from the corner: its outer edge is the offset.
        const float barOuterX = sx * legend.offsetPixels[0];
        const float barInnerX = barOuterX + sx * legend.widthPixels;
        const float nearY = sy * legend.offsetPixels[1];
        const float farY = nearY + sy * (legend.lengthPixels + titleRoom);
        // The title sits at the legend's far end from a bottom corner and at its near
        // end from a top one -- above the bar either way -- and the value runs up it.
        const float barTop = (std::min) (nearY, farY) + titleRoom;
        const float barBottom = barTop + legend.lengthPixels;
        const float barLeft = (std::min) (barOuterX, barInnerX), barRight = (std::max) (barOuterX, barInnerX);

        DraftFill fill;
        fill.draw.screen = true;
        fill.draw.behind = kBehindShow;
        SetRamp (fill.draw, legend.colormap, legend.colormap.min, legend.colormap.max);
        auto corner = [&] (float x, float y, double value) {
            DraftVertex v;
            v.p[0] = fx;
            v.p[1] = fy;
            v.offset[0] = x;
            v.offset[1] = y;
            v.rgba = 0xFFFFFFFFu;
            v.value = float (value);
            fill.vertices.push_back (v);
        };
        const double low = legend.colormap.min, high = legend.colormap.max;
        corner (barLeft, barTop, high);
        corner (barRight, barTop, high);
        corner (barRight, barBottom, low);
        corner (barLeft, barTop, high);
        corner (barRight, barBottom, low);
        corner (barLeft, barBottom, low);
        draft_.fillVertices += fill.vertices.size ();
        draft_.fills.push_back (std::move (fill));

        // The ticks and their values on the bar's inner side; the title above it.
        const layers::Align textAlign = right ? layers::Align::Right : layers::Align::Left;
        const float textX = barInnerX + sx * 6.0f;
        const uint32_t ticks = (std::max) (legend.ticks, 2u);
        for (uint32_t k = 0; k < ticks; ++k) {
            const double t = double (k) / double (ticks - 1);
            const double value = low + (high - low) * t;
            const float y = barBottom - float (t) * legend.lengthPixels;
            char buffer[64] = {};
            std::snprintf (buffer, sizeof (buffer), "%.*f", int (legend.decimals), value);
            std::string label = buffer;
            if (!legend.unit.empty () && k + 1 == ticks)
                label += " " + legend.unit;
            ScreenText (label, fx, fy, textX, y, legend.sizePixels, textAlign, layers::Baseline::Middle, legend.rgba,
                        legend.haloRgba);
            // A 4 px tick from the bar's inner edge.
            DraftGlyph mark;
            mark.anchor[0] = fx;
            mark.anchor[1] = fy;
            mark.flags = kScreenAnchored | kSolid;
            mark.rgba = legend.rgba;
            const float x0 = barInnerX, x1 = barInnerX + sx * 4.0f;
            const float quad[4][2] = { { (std::min) (x0, x1), y - 0.5f },
                                       { (std::max) (x0, x1), y - 0.5f },
                                       { (std::max) (x0, x1), y + 0.5f },
                                       { (std::min) (x0, x1), y + 0.5f } };
            const int order[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; ++i)
                PushGlyphVertex (mark, quad[order[i]][0], quad[order[i]][1], -1.0f, -1.0f);
        }
        if (!legend.title.empty ())
            ScreenText (legend.title, fx, fy, right ? barRight : barLeft, barTop - legend.sizePixels * 0.6f,
                        legend.sizePixels, right ? layers::Align::Right : layers::Align::Left, layers::Baseline::Bottom,
                        legend.rgba, legend.haloRgba);
    }

    void ScreenText (const std::string& text, double fx, double fy, float x, float y, float size, layers::Align align,
                     layers::Baseline baseline, uint32_t rgba, uint32_t halo)
    {
        overlaytext::Label label;
        if (!LayOut (text, size, align, baseline, label))
            return;
        DraftGlyph glyph;
        glyph.anchor[0] = fx;
        glyph.anchor[1] = fy;
        glyph.flags = kScreenAnchored;
        glyph.rgba = rgba;
        glyph.halo = halo;
        glyph.haloPixels = 1.25f;
        for (const overlaytext::Quad& quad : label.quads)
            PushQuad (glyph, quad, x, y);
    }

    bool LayOut (const std::string& text, float size, layers::Align align, layers::Baseline baseline,
                 overlaytext::Label& label)
    {
        if (text_ == nullptr || !text_->Ready ()) {
            ++draft_.problems.textsNotLaidOut;
            draft_.problems.lastError = "the overlay text engine is not ready";
            return false;
        }
        std::string error;
        if (!text_->Layout (text, size, align, baseline, label, error)) {
            ++draft_.problems.textsNotLaidOut;
            draft_.problems.lastError = error;
            return false;
        }
        return true;
    }

    // One glyph's two triangles, moved by (dx, dy) in the label's frame.
    void PushQuad (DraftGlyph glyph, const overlaytext::Quad& quad, float dx, float dy)
    {
        glyph.page = quad.page;
        const float corners[4][4] = { { quad.left, quad.top, quad.u0, quad.v0 },
                                      { quad.right, quad.top, quad.u1, quad.v0 },
                                      { quad.right, quad.bottom, quad.u1, quad.v1 },
                                      { quad.left, quad.bottom, quad.u0, quad.v1 } };
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; ++i)
            if (!PushGlyphVertex (glyph, corners[order[i]][0] + dx, corners[order[i]][1] + dy, corners[order[i]][2],
                                  corners[order[i]][3]))
                return;
    }

    bool PushGlyphVertex (const DraftGlyph& glyph, float x, float y, float u, float v)
    {
        if (draft_.glyphs.size () >= kMaxGlyphVertices) {
            Truncated ();
            return false;
        }
        DraftGlyph vertex = glyph;
        vertex.offset[0] = x;
        vertex.offset[1] = y;
        vertex.uv[0] = u;
        vertex.uv[1] = v;
        draft_.glyphs.push_back (vertex);
        return true;
    }

    bool PushLine (const DraftLine& line)
    {
        if (draft_.lines.size () >= kMaxLines) {
            Truncated ();
            return false;
        }
        draft_.lines.push_back (line);
        return true;
    }

    static void Assign (double (&out)[3], const Vec3& value)
    {
        out[0] = value.x;
        out[1] = value.y;
        out[2] = value.z;
    }

    layers::Views view_;
    overlaytext::Engine* text_;
    Draft draft_;
};

Draft BuildDraft (const std::vector<std::shared_ptr<const layers::Layer>>& all, layers::Views view,
                  overlaytext::Engine* text)
{
    Builder builder (view, text);
    for (const std::shared_ptr<const layers::Layer>& layer : all)
        if (layers::DrawnIn (layer->views, view))
            builder.AddLayer (*layer);
    return builder.Take ();
}

void SplitAt (double value, double origin, float& hi, float& lo)
{
    plancontent::Split (value - origin, hi, lo);
}

// Glyph vertices grouped by what they are drawn with, in first-seen order, so each
// group is one draw call. Stable, so a label's panel stays under its glyphs.
template <typename Key> std::vector<std::pair<Key, std::vector<size_t>>> Group (const std::vector<Key>& keys)
{
    std::vector<std::pair<Key, std::vector<size_t>>> groups;
    std::map<Key, size_t> where;
    for (size_t i = 0; i < keys.size (); ++i) {
        auto found = where.find (keys[i]);
        if (found == where.end ()) {
            found = where.emplace (keys[i], groups.size ()).first;
            groups.push_back ({ keys[i], {} });
        }
        groups[found->second].second.push_back (i);
    }
    return groups;
}

} // namespace

uint32_t BehindCode (layers::Behind resolved)
{
    switch (resolved) {
        case layers::Behind::Hide:
            return kBehindHide;
        case layers::Behind::Fade:
            return kBehindFade;
        case layers::Behind::Dash:
            return kBehindDash;
        case layers::Behind::Show:
        case layers::Behind::Layer:
            break;
    }
    return kBehindShow;
}

Plan PreparePlan (const std::vector<std::shared_ptr<const layers::Layer>>& all, overlaytext::Engine* text)
{
    Draft draft = BuildDraft (all, layers::Views::TwoD, text);
    Plan out;
    out.problems = draft.problems;

    // The centre every half is relative to: the model anchors' mean.
    double sumX = 0.0, sumY = 0.0;
    size_t count = 0;
    for (const DraftFill& fill : draft.fills)
        if (!fill.draw.screen)
            for (const DraftVertex& v : fill.vertices) {
                sumX += v.p[0];
                sumY += v.p[1];
                ++count;
            }
    for (const DraftLine& line : draft.lines) {
        sumX += line.a[0] + line.b[0];
        sumY += line.a[1] + line.b[1];
        count += 2;
    }
    for (const DraftGlyph& glyph : draft.glyphs)
        if ((glyph.flags & kScreenAnchored) == 0) {
            sumX += glyph.anchor[0];
            sumY += glyph.anchor[1];
            ++count;
        }
    if (count > 0) {
        out.originX = sumX / double (count);
        out.originY = sumY / double (count);
    }

    for (const DraftFill& fill : draft.fills) {
        FillDraw draw = fill.draw;
        draw.first = uint32_t (out.fills.size ());
        for (const DraftVertex& v : fill.vertices) {
            PlanFillVertex p = {};
            if (fill.draw.screen) {
                p.hi[0] = float (v.p[0]);
                p.hi[1] = float (v.p[1]);
            }
            else {
                SplitAt (v.p[0], out.originX, p.hi[0], p.lo[0]);
                SplitAt (v.p[1], out.originY, p.hi[1], p.lo[1]);
            }
            p.offset[0] = v.offset[0];
            p.offset[1] = v.offset[1];
            p.rgba = layers::ToUnorm (v.rgba);
            p.value = v.value;
            out.fills.push_back (p);
        }
        draw.count = uint32_t (out.fills.size ()) - draw.first;
        out.fillDraws.push_back (draw);
    }

    for (const DraftLine& line : draft.lines) {
        PlanLine l = {};
        SplitAt (line.a[0], out.originX, l.hiA[0], l.loA[0]);
        SplitAt (line.a[1], out.originY, l.hiA[1], l.loA[1]);
        SplitAt (line.b[0], out.originX, l.hiB[0], l.loB[0]);
        SplitAt (line.b[1], out.originY, l.hiB[1], l.loB[1]);
        l.rgba = layers::ToUnorm (line.rgba);
        l.widthPixels = line.width;
        l.dashPixels = line.dash;
        l.dashDuty = line.duty;
        l.arcStart = line.arc;
        out.lines.push_back (l);
    }

    std::vector<uint32_t> pages;
    pages.reserve (draft.glyphs.size ());
    for (const DraftGlyph& glyph : draft.glyphs)
        pages.push_back (glyph.page);
    for (const auto& group : Group (pages)) {
        GlyphDraw draw;
        draw.first = uint32_t (out.glyphs.size ());
        draw.page = group.first;
        for (const size_t i : group.second) {
            const DraftGlyph& glyph = draft.glyphs[i];
            PlanGlyph g = {};
            if ((glyph.flags & kScreenAnchored) != 0) {
                g.hi[0] = float (glyph.anchor[0]);
                g.hi[1] = float (glyph.anchor[1]);
            }
            else {
                SplitAt (glyph.anchor[0], out.originX, g.hi[0], g.lo[0]);
                SplitAt (glyph.anchor[1], out.originY, g.hi[1], g.lo[1]);
            }
            g.offset[0] = glyph.offset[0];
            g.offset[1] = glyph.offset[1];
            g.uv[0] = glyph.uv[0];
            g.uv[1] = glyph.uv[1];
            g.dir[0] = float (glyph.dir[0]);
            g.dir[1] = float (glyph.dir[1]);
            g.rgba = layers::ToUnorm (glyph.rgba);
            g.halo = layers::ToUnorm (glyph.halo);
            g.haloPixels = glyph.haloPixels;
            g.flags = glyph.flags;
            g.minSpan = glyph.minSpan;
            out.glyphs.push_back (g);
        }
        draw.count = uint32_t (out.glyphs.size ()) - draw.first;
        out.glyphDraws.push_back (draw);
    }
    if (text != nullptr && text->Ready () && !out.glyphs.empty ())
        out.pages = text->Pages ();
    return out;
}

Scene PrepareScene (const std::vector<std::shared_ptr<const layers::Layer>>& all, overlaytext::Engine* text)
{
    Draft draft = BuildDraft (all, layers::Views::ThreeD, text);
    Scene out;
    out.problems = draft.problems;

    for (const DraftFill& fill : draft.fills) {
        FillDraw draw = fill.draw;
        draw.first = uint32_t (out.fills.size ());
        for (const DraftVertex& v : fill.vertices) {
            SceneFillVertex s = {};
            for (int k = 0; k < 3; ++k) {
                s.position[k] = float (v.p[k]);
                s.normal[k] = v.n[k];
            }
            s.offset[0] = v.offset[0];
            s.offset[1] = v.offset[1];
            s.rgba = layers::ToUnorm (v.rgba);
            s.value = v.value;
            out.fills.push_back (s);
        }
        draw.count = uint32_t (out.fills.size ()) - draw.first;
        out.fillDraws.push_back (draw);
    }

    for (const DraftLine& line : draft.lines) {
        SceneLine s = {};
        for (int k = 0; k < 3; ++k) {
            s.a[k] = float (line.a[k]);
            s.b[k] = float (line.b[k]);
        }
        s.rgba = layers::ToUnorm (line.rgba);
        s.widthPixels = line.width;
        s.dashPixels = line.dash;
        s.dashDuty = line.duty;
        s.arcStart = line.arc;
        s.behind = line.behind;
        out.lines.push_back (s);
    }

    // Grouped by (depth policy, page): each group is one pipeline and one texture.
    std::vector<std::pair<uint32_t, uint32_t>> keys;
    keys.reserve (draft.glyphs.size ());
    for (const DraftGlyph& glyph : draft.glyphs)
        keys.push_back ({ (glyph.flags & kScreenAnchored) != 0 ? kBehindShow : glyph.behind, glyph.page });
    for (const auto& group : Group (keys)) {
        GlyphDraw draw;
        draw.first = uint32_t (out.glyphs.size ());
        draw.behind = group.first.first;
        draw.page = group.first.second;
        for (const size_t i : group.second) {
            const DraftGlyph& glyph = draft.glyphs[i];
            SceneGlyph g = {};
            for (int k = 0; k < 3; ++k) {
                g.position[k] = float (glyph.anchor[k]);
                g.dir[k] = float (glyph.dir[k]);
            }
            g.offset[0] = glyph.offset[0];
            g.offset[1] = glyph.offset[1];
            g.uv[0] = glyph.uv[0];
            g.uv[1] = glyph.uv[1];
            g.rgba = layers::ToUnorm (glyph.rgba);
            g.halo = layers::ToUnorm (glyph.halo);
            g.haloPixels = glyph.haloPixels;
            g.flags = glyph.flags;
            g.minSpan = glyph.minSpan;
            g.behind = draw.behind;
            out.glyphs.push_back (g);
        }
        draw.count = uint32_t (out.glyphs.size ()) - draw.first;
        out.glyphDraws.push_back (draw);
    }
    if (text != nullptr && text->Ready () && !out.glyphs.empty ())
        out.pages = text->Pages ();
    return out;
}

std::vector<double> VertexNormals (const std::vector<double>& points, const std::vector<uint32_t>& indices)
{
    const size_t vertices = points.size () / 3;
    std::vector<double> normals (vertices * 3, 0.0);
    for (size_t t = 0; t + 2 < indices.size (); t += 3) {
        const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= vertices || i1 >= vertices || i2 >= vertices)
            continue;
        // Unnormalised: its length is twice the triangle's area, which is the weight.
        const Vec3 n = Cross (Sub (At (points, i1), At (points, i0)), Sub (At (points, i2), At (points, i0)));
        for (const uint32_t i : { i0, i1, i2 }) {
            normals[size_t (i) * 3] += n.x;
            normals[size_t (i) * 3 + 1] += n.y;
            normals[size_t (i) * 3 + 2] += n.z;
        }
    }
    for (size_t v = 0; v < vertices; ++v) {
        const Vec3 n = { normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2] };
        const double length = Length (n);
        if (length > 1e-30) {
            normals[v * 3] = n.x / length;
            normals[v * 3 + 1] = n.y / length;
            normals[v * 3 + 2] = n.z / length;
        }
        else {
            normals[v * 3] = 0.0;
            normals[v * 3 + 1] = 0.0;
            normals[v * 3 + 2] = 1.0;
        }
    }
    return normals;
}

std::vector<std::pair<uint32_t, uint32_t>> FeatureEdges (const std::vector<double>& points,
                                                         const std::vector<uint32_t>& indices, float angleDegrees)
{
    const size_t vertices = points.size () / 3;
    // Weld: every vertex to the first one at its position, within a micrometre.
    std::vector<uint32_t> canonical (vertices);
    {
        struct KeyHash {
            size_t operator() (const std::tuple<int64_t, int64_t, int64_t>& key) const
            {
                const uint64_t a = uint64_t (std::get<0> (key)), b = uint64_t (std::get<1> (key)),
                               c = uint64_t (std::get<2> (key));
                return size_t (a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0x94D049BB133111EBull ^ c);
            }
        };
        std::unordered_map<std::tuple<int64_t, int64_t, int64_t>, uint32_t, KeyHash> seen;
        seen.reserve (vertices);
        for (size_t v = 0; v < vertices; ++v) {
            const auto key = std::make_tuple (int64_t (std::llround (points[v * 3] * 1e6)),
                                              int64_t (std::llround (points[v * 3 + 1] * 1e6)),
                                              int64_t (std::llround (points[v * 3 + 2] * 1e6)));
            canonical[v] = seen.emplace (key, uint32_t (v)).first->second;
        }
    }
    struct EdgeFaces {
        uint32_t a = 0, b = 0; // original indices, for the coordinates
        Vec3 normals[2];
        uint32_t faces = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, EdgeFaces> edges;
    for (size_t t = 0; t + 2 < indices.size (); t += 3) {
        const uint32_t original[3] = { indices[t], indices[t + 1], indices[t + 2] };
        if (original[0] >= vertices || original[1] >= vertices || original[2] >= vertices)
            continue;
        const Vec3 n = Cross (Sub (At (points, original[1]), At (points, original[0])),
                              Sub (At (points, original[2]), At (points, original[0])));
        const double area = Length (n);
        if (area < 1e-18)
            continue; // a degenerate triangle has no face to crease against
        const Vec3 unit = { n.x / area, n.y / area, n.z / area };
        for (int e = 0; e < 3; ++e) {
            const uint32_t i = original[e], j = original[(e + 1) % 3];
            const uint32_t ci = canonical[i], cj = canonical[j];
            if (ci == cj)
                continue;
            EdgeFaces& edge = edges[{ (std::min) (ci, cj), (std::max) (ci, cj) }];
            if (edge.faces == 0) {
                edge.a = i;
                edge.b = j;
            }
            if (edge.faces < 2)
                edge.normals[edge.faces] = unit;
            ++edge.faces;
        }
    }
    const double creaseCosine = std::cos (double (angleDegrees) * kPi / 180.0);
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& entry : edges) {
        const EdgeFaces& edge = entry.second;
        // One face: a boundary. More than two: not a surface, so drawn. Two: a crease
        // when they turn by more than the angle.
        const bool feature = edge.faces != 2 || Dot (edge.normals[0], edge.normals[1]) < creaseCosine - 1e-12;
        if (feature)
            out.push_back ({ edge.a, edge.b });
    }
    return out;
}

std::string FormatLength (double metres, uint32_t decimals, layers::LengthUnit unit, bool showUnit)
{
    double value = metres;
    const char* suffix = "m";
    if (unit == layers::LengthUnit::Centimetres) {
        value = metres * 100.0;
        suffix = "cm";
    }
    else if (unit == layers::LengthUnit::Millimetres) {
        value = metres * 1000.0;
        suffix = "mm";
    }
    char buffer[64] = {};
    std::snprintf (buffer, sizeof (buffer), "%.*f", int ((std::min) (decimals, 6u)), value);
    std::string text = buffer;
    if (showUnit)
        text += std::string (" ") + suffix;
    return text;
}

uint32_t RampAt (const std::vector<layers::ColourStop>& stops, float t)
{
    if (stops.empty ())
        return 0;
    if (t <= stops.front ().at)
        return stops.front ().rgba;
    if (t >= stops.back ().at)
        return stops.back ().rgba;
    for (size_t i = 1; i < stops.size (); ++i) {
        if (t > stops[i].at)
            continue;
        const float span = stops[i].at - stops[i - 1].at;
        const float f = span > 1e-6f ? (t - stops[i - 1].at) / span : 1.0f;
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float a = float ((stops[i - 1].rgba >> shift) & 0xFFu);
            const float b = float ((stops[i].rgba >> shift) & 0xFFu);
            out |= uint32_t (std::lround (a + (b - a) * f)) << shift;
        }
        return out;
    }
    return stops.back ().rgba;
}

} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv
