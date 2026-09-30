// ArchViz/OverlayScene -- see the header.

#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/OverlaySceneBuilder.hpp"

#include "Annotation/DimensionGeometry.hpp"
#include "ArchViz/OverlayHud.hpp" // ComposePages: the HUD's atlas pages

#include <algorithm>
#include <chrono>
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
        if (layers::DrawnByGuest (mesh, layer))
            AddMesh (layer, mesh);
    for (const layers::Polyline& polyline : layer.polylines)
        if (layers::DrawnByGuest (polyline, layer))
            AddPolyline (layer, polyline);
    for (const layers::Dimension& dimension : layer.dimensions)
        AddDimension (layer, dimension);
    for (const layers::Text& text : layer.texts)
        AddText (layer, text);
    for (size_t i = 0; i < layer.legends.size (); ++i)
        AddLegend (layer, layer.legends[i], uint32_t (i));
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
        fill.draw.layer = LayerKey (layer.name);
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
            line.dashes = DashesOf ({}, nullptr, behind);
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
    const uint32_t dashes = DashesOf (polyline.dashMetres, &polyline.hidden, behind);
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
        line.hiddenRgba = polyline.hidden.rgba;
        line.hiddenWidth = polyline.hidden.widthPixels;
        line.dashes = dashes;
        line.arc = float (arc);
        line.behind = behind;
        if (!PushLine (line))
            return;
        arc += length;
    }
    if (polyline.closed || points < 2)
        return;
    // Each end's own segment, the first that has a length.
    auto end = [&] (bool start, Vec3& at, Vec3& along) {
        for (size_t k = 1; k < points; ++k) {
            const size_t tip = start ? 0 : points - 1, next = start ? k : points - 1 - k;
            at = At (polyline.points, uint32_t (tip));
            along = start ? Sub (At (polyline.points, uint32_t (next)), at)
                          : Sub (at, At (polyline.points, uint32_t (next)));
            if (Plan ())
                along.z = 0.0;
            if (Length (along) > 1e-9)
                return true;
        }
        return false;
    };
    const float width = (std::max) (polyline.widthPixels, 1.25f);
    Vec3 at {}, along {};
    if (polyline.startArrow != layers::Terminator::None && end (true, at, along))
        AddTerminator (at, along, polyline.startArrow, true, width, polyline.arrowSizePixels, polyline.rgba, behind);
    if (polyline.endArrow != layers::Terminator::None && end (false, at, along))
        AddTerminator (at, along, polyline.endArrow, false, width, polyline.arrowSizePixels, polyline.rgba, behind);
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
        l.dashes = DashesOf ({}, nullptr, behind);
        PushLine (l);
    };
    line (resolved->dimensionFirst, resolved->dimensionSecond);
    line (resolved->witnessFirstStart, resolved->witnessFirstEnd);
    line (resolved->witnessSecondStart, resolved->witnessSecondEnd);

    // The terminators turn with the dimension line's projection; the text also
    // keeps upright and gives way when the dimension is too short on screen to
    // hold it between its ends.
    const float width = (std::max) (dimension.widthPixels, 1.25f);
    if (dimension.terminator != layers::Terminator::None) {
        AddTerminator (first, span, dimension.terminator, true, width, dimension.terminatorSizePixels, dimension.rgba,
                       behind);
        AddTerminator (second, span, dimension.terminator, false, width, dimension.terminatorSizePixels, dimension.rgba,
                       behind);
    }

    const std::string text = !dimension.text.empty () ? dimension.text
                                                      : FormatLength (resolved->measurement, dimension.decimals,
                                                                      dimension.unit, dimension.showUnit);
    const Vec3 middle = { (first.x + second.x) * 0.5, (first.y + second.y) * 0.5, (first.z + second.z) * 0.5 };
    overlaytext::Label label;
    if (!LayOut (text, dimension.textSizePixels, layers::Align::Center, layers::Baseline::Bottom, label,
                 dimension.font))
        return;
    const float gap = 3.0f;
    const float minSpan = (label.right - label.left) + 2.0f * 10.0f;
    for (const overlaytext::Quad& quad : label.quads) {
        DraftGlyph glyph;
        Assign (glyph.anchor, middle);
        Assign (glyph.dir, span);
        glyph.rgba = (dimension.textRgba & 0xFFu) != 0 ? dimension.textRgba : dimension.rgba;
        glyph.halo = dimension.haloRgba;
        glyph.haloPixels = HaloOf (dimension.haloPixels, 1.0f);
        glyph.flags = kAlongDirection | kKeepUpright | kHideShortSpan;
        glyph.minSpan = minSpan;
        glyph.behind = behind;
        PushQuad (glyph, quad, 0.0f, -gap);
    }
}

// A tick, an arrowhead or a dot `size` pixels long at one end of a line, in the
// local frame whose +x is the line's direction on screen; `first` is its start.
void Builder::AddTerminator (const Vec3& at, const Vec3& span, layers::Terminator terminator, bool first, float width,
                             float size, uint32_t rgba, uint32_t behind)
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
        const float length = size, half = 0.35f * size, back = first ? length : -length;
        const float tri[3][2] = { { 0.0f, 0.0f }, { back, -half }, { back, half } };
        const int order[6] = { 0, 1, 2, 2, 2, 2 };
        for (int i = 0; i < 6; ++i) {
            corners[i][0] = tri[order[i]][0];
            corners[i][1] = tri[order[i]][1];
        }
    }
    else if (terminator == layers::Terminator::Dot) {
        const float r = (std::max) (0.25f * size, width * 1.5f);
        const float quad[4][2] = { { 0.0f, -r }, { r, 0.0f }, { 0.0f, r }, { -r, 0.0f } };
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; ++i) {
            corners[i][0] = quad[order[i]][0];
            corners[i][1] = quad[order[i]][1];
        }
    }
    else {
        // The architectural slash: 45 degrees through the end, 0.9 of `size` long.
        const float h = 0.45f * size * 0.70710678f, w = width * 0.5f * 0.70710678f;
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
    if (!LayOut (text.text, text.sizePixels, text.align, text.baseline, label, text.font))
        return;
    if (text.planar) {
        AddPlanarText (layer, text, label);
        return;
    }
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
    glyph.haloPixels = HaloOf (text.haloPixels, text.haloScale);
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

// The label's layout mapped onto its plane: x along `direction`, the layout's y (down)
// against the glyphs' up, `normal` x `direction`. Every corner is its own model point
// (`kModelQuad`), so the text is foreshortened and hidden like the model.
void Builder::AddPlanarText (const layers::Layer& layer, const layers::Text& text, const overlaytext::Label& label)
{
    const Vec3 normal = Unit ({ text.normal[0], text.normal[1], text.normal[2] });
    Vec3 along = { text.direction[0], text.direction[1], text.direction[2] };
    along = Unit (Sub (along, Scaled (normal, Dot (along, normal))));
    const Vec3 up = Cross (normal, along);
    const Vec3 at = { text.at[0], text.at[1], text.at[2] };
    const double metresPerPixel = text.sizeMetres / double (text.sizePixels);
    auto model = [&] (float x, float y) {
        const double a = (double (x) + text.offsetPixels[0]) * metresPerPixel;
        const double b = -(double (y) + text.offsetPixels[1]) * metresPerPixel;
        return Plus (at, Plus (Scaled (along, a), Scaled (up, b)));
    };
    DraftGlyph glyph;
    glyph.flags = kModelQuad;
    glyph.behind = BehindOf (text.behind, layer);
    glyph.halo = text.haloRgba;
    glyph.haloPixels = HaloOf (text.haloPixels, text.haloScale);
    const int order[6] = { 0, 1, 2, 0, 2, 3 };
    if ((text.backgroundRgba & 0xFFu) != 0 && !label.quads.empty ()) {
        const float pad = text.sizePixels * 0.2f;
        const float quad[4][2] = { { label.left - pad, label.top - pad },
                                   { label.right + pad, label.top - pad },
                                   { label.right + pad, label.bottom + pad },
                                   { label.left - pad, label.bottom + pad } };
        DraftGlyph panel = glyph;
        panel.rgba = text.backgroundRgba;
        panel.flags |= kSolid;
        for (int i = 0; i < 6; ++i) {
            Assign (panel.anchor, model (quad[order[i]][0], quad[order[i]][1]));
            PushGlyphVertex (panel, 0.0f, 0.0f, -1.0f, -1.0f);
        }
    }
    glyph.rgba = text.rgba;
    for (const overlaytext::Quad& quad : label.quads) {
        const float corners[4][4] = { { quad.left, quad.top, quad.u0, quad.v0 },
                                      { quad.right, quad.top, quad.u1, quad.v0 },
                                      { quad.right, quad.bottom, quad.u1, quad.v1 },
                                      { quad.left, quad.bottom, quad.u0, quad.v1 } };
        glyph.page = quad.page;
        for (int i = 0; i < 6; ++i) {
            Assign (glyph.anchor, model (corners[order[i]][0], corners[order[i]][1]));
            if (!PushGlyphVertex (glyph, 0.0f, 0.0f, corners[order[i]][2], corners[order[i]][3]))
                return;
        }
    }
}

overlaytext::Engine* Builder::EngineFor (const std::string& font)
{
    if (font.empty () || !fonts_)
        return text_;
    overlaytext::Engine* const engine = fonts_ (font);
    if (engine != nullptr && engine->Ready ())
        return engine;
    draft_.problems.lastError = "the font \"" + font + "\" could not be loaded: the bundled font is used";
    return text_;
}

bool Builder::LayOut (const std::string& text, float size, layers::Align align, layers::Baseline baseline,
                      overlaytext::Label& label, const std::string& font)
{
    overlaytext::Engine* const engine = EngineFor (font);
    if (engine == nullptr || !engine->Ready ()) {
        ++draft_.problems.textsNotLaidOut;
        draft_.problems.lastError = "the overlay text engine is not ready";
        return false;
    }
    std::string error;
    if (!engine->Layout (text, size, align, baseline, label, error)) {
        ++draft_.problems.textsNotLaidOut;
        draft_.problems.lastError = error;
        return false;
    }
    // Pages are the engine's own; the draft's list holds every engine's the glyphs use.
    for (overlaytext::Quad& quad : label.quads) {
        const auto key = std::make_pair (static_cast<const overlaytext::Engine*> (engine), quad.page);
        auto slot = pageSlots_.find (key);
        if (slot == pageSlots_.end ()) {
            slot = pageSlots_.emplace (key, uint32_t (draft_.textPages.size ())).first;
            draft_.textPages.push_back (engine->Pages ()[quad.page]);
        }
        quad.page = slot->second;
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

uint32_t Builder::DashOf (const float* lengths, size_t count)
{
    if (count == 0)
        return kSolidPattern;
    std::array<float, layers::kMaxDashEntries> pattern {};
    for (size_t i = 0; i < count && i < pattern.size (); ++i)
        pattern[i] = lengths[i];
    for (size_t i = 0; i < draft_.dashes.size (); ++i)
        if (draft_.dashes[i] == pattern)
            return uint32_t (i);
    if (draft_.dashes.size () >= kMaxDashPatterns) {
        draft_.problems.lastError = "more than 16 dash patterns: the rest are drawn solid";
        return kSolidPattern;
    }
    draft_.dashes.push_back (pattern);
    return uint32_t (draft_.dashes.size () - 1);
}

uint32_t Builder::DashesOf (const std::vector<float>& visible, const layers::HiddenLine* hidden, uint32_t behind)
{
    const uint32_t shown = DashOf (visible.data (), visible.size ());
    uint32_t behindBuilding = kSolidPattern;
    if (hidden != nullptr && !hidden->dashMetres.empty ())
        behindBuilding = DashOf (hidden->dashMetres.data (), hidden->dashMetres.size ());
    else if (behind == kBehindDash)
        behindBuilding = DashOf (layers::kDefaultHiddenDash, 2);
    return shown | (behindBuilding << 8);
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

using namespace build;

namespace {

// One layer's draft as it was built, and what it was built with.
struct CachedDraft {
    std::shared_ptr<const layers::Layer> layer;
    const overlaytext::Engine* text = nullptr;
    std::shared_ptr<const Draft> draft;
};

std::vector<CachedDraft> g_drafts[2]; // the plan's, the 3D window's -- MAIN THREAD

// `from` after what `into` holds, its text pages renumbered after `into`'s. False, and
// nothing appended, when the whole would pass a budget.
bool Append (Draft& into, const Draft& from)
{
    if (into.fillVertices + from.fillVertices > kMaxFillVertices ||
        into.lines.size () + from.lines.size () > kMaxLines ||
        into.glyphs.size () + from.glyphs.size () > kMaxGlyphVertices)
        return false;
    // A page two layers share is one page: same id, one slot, one draw.
    std::vector<uint32_t> slot (from.textPages.size ());
    for (size_t i = 0; i < from.textPages.size (); ++i) {
        size_t at = 0;
        while (at < into.textPages.size () && into.textPages[at]->id != from.textPages[i]->id)
            ++at;
        if (at == into.textPages.size ())
            into.textPages.push_back (from.textPages[i]);
        slot[i] = uint32_t (at);
    }
    // Dash patterns likewise, by content; past the table's room a line is drawn solid.
    std::vector<uint32_t> dash (from.dashes.size (), kSolidPattern);
    for (size_t i = 0; i < from.dashes.size (); ++i) {
        size_t at = 0;
        while (at < into.dashes.size () && into.dashes[at] != from.dashes[i])
            ++at;
        if (at == into.dashes.size () && into.dashes.size () < kMaxDashPatterns)
            into.dashes.push_back (from.dashes[i]);
        dash[i] = at < into.dashes.size () ? uint32_t (at) : kSolidPattern;
    }
    auto renumbered = [&dash] (uint32_t id) { return id < dash.size () ? dash[id] : kSolidPattern; };
    into.fills.insert (into.fills.end (), from.fills.begin (), from.fills.end ());
    into.lines.reserve (into.lines.size () + from.lines.size ());
    for (DraftLine line : from.lines) {
        line.dashes = renumbered (line.dashes & 0xFFu) | (renumbered ((line.dashes >> 8) & 0xFFu) << 8);
        into.lines.push_back (line);
    }
    into.glyphs.reserve (into.glyphs.size () + from.glyphs.size ());
    for (DraftGlyph glyph : from.glyphs) {
        if (glyph.page < kHudPageBase && glyph.page < slot.size ())
            glyph.page = slot[glyph.page];
        into.glyphs.push_back (glyph);
    }
    into.fillVertices += from.fillVertices;
    into.regions.insert (into.regions.end (), from.regions.begin (), from.regions.end ());
    into.problems.textsNotLaidOut += from.problems.textsNotLaidOut;
    into.problems.dimensionsNotResolved += from.problems.dimensionsNotResolved;
    into.problems.truncated += from.problems.truncated;
    if (!from.problems.lastError.empty ())
        into.problems.lastError = from.problems.lastError;
    return true;
}

Draft BuildDraft (const std::vector<std::shared_ptr<const layers::Layer>>& all, layers::Views view,
                  overlaytext::Engine* text, const FontResolver& fonts, Cost& cost)
{
    std::vector<CachedDraft>& cache = g_drafts[view == layers::Views::TwoD ? 0 : 1];
    std::vector<CachedDraft> kept;
    Draft out;
    for (const std::shared_ptr<const layers::Layer>& layer : all) {
        if (!layers::DrawnIn (layer->views, view))
            continue;
        std::shared_ptr<const Draft> draft;
        for (const CachedDraft& cached : cache)
            if (cached.layer == layer && cached.text == text)
                draft = cached.draft;
        if (draft != nullptr) {
            ++cost.layersReused;
        }
        else {
            Builder builder (view, text, nullptr, 1.0f, fonts);
            builder.AddLayer (*layer);
            draft = std::make_shared<const Draft> (builder.Take ());
            ++cost.layersBuilt;
        }
        kept.push_back ({ layer, text, draft });
        if (!Append (out, *draft)) {
            ++out.problems.truncated;
            out.problems.lastError = "layer \"" + layer->name + "\" would pass the overlay's budget: not drawn";
        }
    }
    cache = std::move (kept);
    return out;
}

// The panels of the layers drawn in `view`, laid out as one set (OverlayHud.hpp), with
// the scene's legends' bars where they are on the view, for their tooltips.
Draft HudDraft (const std::vector<std::shared_ptr<const layers::Layer>>& all, layers::Views view,
                overlayhud::Engine* hud, float scale, const overlayhud::Input& input,
                const std::vector<overlayinput::Region>* legends)
{
    const FontResolver none;
    Builder builder (view, nullptr, hud, scale, none);
    builder.AddHud (all, input, legends);
    return builder.Take ();
}

uint32_t MicrosecondsSince (std::chrono::steady_clock::time_point started)
{
    return uint32_t (
        std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now () - started).count ());
}

// The pages the glyphs sample: the text pages they use, from every font's engine
// (LayOut numbered them), then the HUD's, every glyph's page renumbered into that one list.
std::vector<std::shared_ptr<const overlaytext::Page>> ComposePages (Draft& draft, overlayhud::Engine* hud)
{
    std::vector<std::shared_ptr<const overlaytext::Page>> pages;
    if (draft.glyphs.empty ())
        return pages;
    pages = draft.textPages;
    const uint32_t base = uint32_t (pages.size ());
    bool panels = false;
    for (DraftGlyph& glyph : draft.glyphs)
        if (glyph.page >= kHudPageBase) {
            glyph.page = base + (glyph.page - kHudPageBase);
            panels = true;
        }
    if (panels && hud != nullptr)
        for (const auto& page : hud->Pages ())
            pages.push_back (page);
    return pages;
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

void ForgetDrafts ()
{
    for (std::vector<CachedDraft>& cache : g_drafts)
        cache.clear ();
}

void ForgetDrafts (layers::Views view)
{
    g_drafts[view == layers::Views::TwoD ? 0 : 1].clear ();
}

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

namespace {

// A draft made into the plan's arrays; `hud` supplies the pages of its panels' glyphs.
void FinishPlan (Draft& draft, overlayhud::Engine* hud, Plan& out)
{
    out.problems = draft.problems;
    out.regions = std::move (draft.regions);
    out.highlight = draft.highlight;
    out.hand = draft.hand;
    for (const auto& pattern : draft.dashes)
        out.dashes.insert (out.dashes.end (), pattern.begin (), pattern.end ());
    out.pages = ComposePages (draft, hud);

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
        l.arcStart = line.arc;
        l.dashes = line.dashes;
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
}

} // namespace

Plan PreparePlan (const std::vector<std::shared_ptr<const layers::Layer>>& all, overlaytext::Engine* text,
                  const FontResolver& fonts)
{
    const auto started = std::chrono::steady_clock::now ();
    Plan out;
    Draft draft = BuildDraft (all, layers::Views::TwoD, text, fonts, out.cost);
    FinishPlan (draft, nullptr, out);
    out.cost.microseconds = MicrosecondsSince (started);
    return out;
}

Plan PreparePlanHud (const std::vector<std::shared_ptr<const layers::Layer>>& all, overlayhud::Engine* hud, float scale,
                     const overlayhud::Input& input, const std::vector<overlayinput::Region>* legends)
{
    const auto started = std::chrono::steady_clock::now ();
    Plan out;
    Draft draft = HudDraft (all, layers::Views::TwoD, hud, scale, input, legends);
    FinishPlan (draft, hud, out);
    out.cost.microseconds = MicrosecondsSince (started);
    return out;
}

namespace {

// A draft made into the 3D window's arrays; `hud` supplies the pages of its panels' glyphs.
void FinishScene (Draft& draft, overlayhud::Engine* hud, Scene& out)
{
    out.problems = draft.problems;
    out.regions = std::move (draft.regions);
    out.highlight = draft.highlight;
    out.hand = draft.hand;
    for (const auto& pattern : draft.dashes)
        out.dashes.insert (out.dashes.end (), pattern.begin (), pattern.end ());
    out.pages = ComposePages (draft, hud);

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
        s.hiddenRgba = layers::ToUnorm (line.hiddenRgba);
        s.widthPixels = line.width;
        s.hiddenWidthPixels = line.hiddenWidth;
        s.arcStart = line.arc;
        s.dashes = line.dashes;
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
}

} // namespace

Scene PrepareScene (const std::vector<std::shared_ptr<const layers::Layer>>& all, overlaytext::Engine* text,
                    const FontResolver& fonts)
{
    const auto started = std::chrono::steady_clock::now ();
    Scene out;
    Draft draft = BuildDraft (all, layers::Views::ThreeD, text, fonts, out.cost);
    FinishScene (draft, nullptr, out);
    out.cost.microseconds = MicrosecondsSince (started);
    return out;
}

Scene PrepareSceneHud (const std::vector<std::shared_ptr<const layers::Layer>>& all, overlayhud::Engine* hud,
                       float scale, const overlayhud::Input& input, const std::vector<overlayinput::Region>* legends)
{
    const auto started = std::chrono::steady_clock::now ();
    Scene out;
    Draft draft = HudDraft (all, layers::Views::ThreeD, hud, scale, input, legends);
    FinishScene (draft, hud, out);
    out.cost.microseconds = MicrosecondsSince (started);
    return out;
}

} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv
