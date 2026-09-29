// ArchViz/OverlayScene -- see the header.

#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/OverlaySceneBuilder.hpp"

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

namespace build {

void Builder::AddLayer (const layers::Layer& layer)
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

bool Builder::Plan () const
{
    return view_ == layers::Views::TwoD;
}

// The 2D overlay has no depth (finding 13): everything there is drawn whole.
uint32_t Builder::BehindOf (layers::Behind behind, const layers::Layer& layer) const
{
    return Plan () ? kBehindShow : BehindCode (layers::Resolve (behind, layer));
}

void Builder::Truncated ()
{
    ++draft_.problems.truncated;
}

void Builder::SetRamp (FillDraw& draw, const layers::Colormap& colormap, double min, double max) const
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

void Builder::AddMesh (const layers::Layer& layer, const layers::Mesh& mesh)
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
    // The caller's normals, per vertex; otherwise, for a shaded mesh, per corner and
    // split at creases (CornerNormals). A flat mesh reads none.
    const bool shaded = mesh.style.shading != layers::Shading::Flat;
    const std::vector<double> corners = mesh.normals.empty () && shaded
                                            ? CornerNormals (mesh.points, mesh.indices, kSmoothingCreaseDegrees)
                                            : std::vector<double> ();
    fill.vertices.reserve (mesh.indices.size ());
    for (size_t c = 0; c < mesh.indices.size (); ++c) {
        const uint32_t index = mesh.indices[c];
        DraftVertex v;
        const size_t at = size_t (index) * 3;
        v.p[0] = mesh.points[at];
        v.p[1] = mesh.points[at + 1];
        v.p[2] = mesh.points[at + 2];
        const double* n = !mesh.normals.empty () ? &mesh.normals[at] : !corners.empty () ? &corners[c * 3] : nullptr;
        if (n != nullptr) {
            v.n[0] = float (n[0]);
            v.n[1] = float (n[1]);
            v.n[2] = float (n[2]);
        }
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

void Builder::AddPolyline (const layers::Layer& layer, const layers::Polyline& polyline)
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

void Builder::AddDimension (const layers::Layer& layer, const layers::Dimension& dimension)
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
void Builder::AddTerminator (const Vec3& at, const Vec3& span, layers::Terminator terminator, bool first, float width,
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

void Builder::AddText (const layers::Layer& layer, const layers::Text& text)
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

bool Builder::LayOut (const std::string& text, float size, layers::Align align, layers::Baseline baseline,
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
void Builder::PushQuad (DraftGlyph glyph, const overlaytext::Quad& quad, float dx, float dy)
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

bool Builder::PushGlyphVertex (const DraftGlyph& glyph, float x, float y, float u, float v)
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

bool Builder::PushLine (const DraftLine& line)
{
    if (draft_.lines.size () >= kMaxLines) {
        Truncated ();
        return false;
    }
    draft_.lines.push_back (line);
    return true;
}

void Builder::Assign (double (&out)[3], const Vec3& value)
{
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

} // namespace build

namespace {

using namespace build;

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

} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv
