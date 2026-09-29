// ArchViz/OverlayScene: what the Diligent guest draws, prepared from the layers.
// Every way of getting this wrong is a picture over Archicad's view -- a dimension on
// the wrong side, a label on the wrong anchor, a crease where a seam is -- so each
// rule is pinned here, offline, against the real source.

#include "ArchViz/OverlayScene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace scene = geomsrv::archviz::overlayscene;
namespace layers = geomsrv::archviz::overlaylayers;
namespace text = geomsrv::archviz::overlaytext;

namespace {

text::Engine& Engine ()
{
    static text::Engine engine;
    if (!engine.Ready ()) {
        std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
        std::vector<uint8_t> font { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
        std::string error;
        EXPECT_TRUE (engine.Init (std::move (font), error)) << error;
    }
    return engine;
}

std::vector<std::shared_ptr<const layers::Layer>> One (layers::Layer layer)
{
    return { std::make_shared<const layers::Layer> (std::move (layer)) };
}

double Rejoin (float hi, float lo, double origin)
{
    return double (hi) + double (lo) + origin;
}

layers::Mesh Cube (bool splitFaces)
{
    // Eight corners of the unit cube, and its twelve triangles wound outwards.
    const double corners[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
                                   { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } };
    const uint32_t faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
                                   { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    layers::Mesh mesh;
    for (const auto& corner : corners)
        if (!splitFaces)
            mesh.points.insert (mesh.points.end (), { corner[0], corner[1], corner[2] });
    for (const auto& face : faces) {
        uint32_t index[4];
        for (int k = 0; k < 4; ++k) {
            if (splitFaces) {
                index[k] = uint32_t (mesh.points.size () / 3);
                const auto& corner = corners[face[k]];
                mesh.points.insert (mesh.points.end (), { corner[0], corner[1], corner[2] });
            }
            else {
                index[k] = face[k];
            }
        }
        mesh.indices.insert (mesh.indices.end (), { index[0], index[1], index[2], index[0], index[2], index[3] });
    }
    return mesh;
}

} // namespace

TEST (OverlayScene, APlainPolylineStaysRawAndADashedOneGoesToTheGuest)
{
    layers::Layer layer;
    layer.name = "lines";
    layers::Polyline plain;
    plain.points = { 0, 0, 0, 10, 0, 0 };
    layers::Polyline dashed = plain;
    dashed.dashPixels = 8.0f;
    layer.polylines = { plain, dashed };
    const auto all = One (layer);

    EXPECT_EQ (layers::Prepare2D (all).strokes.size (), 1u);
    EXPECT_EQ (layers::Prepare3D (all).occludedLines.size (), 2u); // one segment, two vertices
    const scene::Plan plan = scene::PreparePlan (all, nullptr);
    ASSERT_EQ (plan.lines.size (), 1u);
    EXPECT_FLOAT_EQ (plan.lines[0].dashPixels, 8.0f);
    EXPECT_EQ (scene::PrepareScene (all, nullptr).lines.size (), 1u);
    EXPECT_TRUE (layers::NeedsGuest (layer));
}

// ⚠️ THE PLAN'S HALVES REJOIN TO THE MODEL COORDINATE -- the walls' precision, which
// is what keeps a georeferenced project's millimetres.
TEST (OverlayScene, PlanLinesRejoinToTheirModelCoordinatesFarFromTheOrigin)
{
    layers::Layer layer;
    layer.name = "far";
    layers::Polyline line;
    line.points = { 581234.5678, 6061234.4321, 0, 581244.5679, 6061234.4322, 0 };
    line.dashPixels = 6.0f;
    layer.polylines = { line };
    const scene::Plan plan = scene::PreparePlan (One (layer), nullptr);
    ASSERT_EQ (plan.lines.size (), 1u);
    const scene::PlanLine& l = plan.lines[0];
    EXPECT_NEAR (Rejoin (l.hiA[0], l.loA[0], plan.originX), 581234.5678, 1e-6);
    EXPECT_NEAR (Rejoin (l.hiA[1], l.loA[1], plan.originY), 6061234.4321, 1e-6);
    EXPECT_NEAR (Rejoin (l.hiB[0], l.loB[0], plan.originX), 581244.5679, 1e-6);
    EXPECT_NEAR (Rejoin (l.hiB[1], l.loB[1], plan.originY), 6061234.4322, 1e-6);
}

TEST (OverlayScene, ADashCarriesItsDistanceAlongThePolyline)
{
    layers::Layer layer;
    layer.name = "dash";
    layers::Polyline line;
    line.points = { 0, 0, 0, 3, 0, 0, 3, 4, 0 };
    line.dashPixels = 8.0f;
    layer.polylines = { line };
    const scene::Scene scene = scene::PrepareScene (One (layer), nullptr);
    ASSERT_EQ (scene.lines.size (), 2u);
    EXPECT_FLOAT_EQ (scene.lines[0].arcStart, 0.0f);
    EXPECT_FLOAT_EQ (scene.lines[1].arcStart, 3.0f);
}

TEST (OverlayScene, BehindFollowsTheLayerUnlessTheItemSaysOtherwise)
{
    layers::Layer layer;
    layer.name = "behind";
    layers::Polyline line;
    line.points = { 0, 0, 0, 1, 0, 0 };
    line.behind = layers::Behind::Layer;
    line.dashPixels = 4.0f;
    layers::Polyline faded = line;
    faded.behind = layers::Behind::Fade;
    layer.polylines = { line, faded };
    layer.occlusion = layers::Behind::Hide;
    scene::Scene scene = scene::PrepareScene (One (layer), nullptr);
    ASSERT_EQ (scene.lines.size (), 2u);
    EXPECT_EQ (scene.lines[0].behind, scene::kBehindHide);
    EXPECT_EQ (scene.lines[1].behind, scene::kBehindFade);
    layer.occlusion = layers::Behind::Show;
    scene = scene::PrepareScene (One (layer), nullptr);
    EXPECT_EQ (scene.lines[0].behind, scene::kBehindShow);
}

TEST (OverlayScene, ADimensionIsItsLineTwoWitnessesTwoTicksAndItsLength)
{
    layers::Layer layer;
    layer.name = "dimension";
    layers::Dimension dimension;
    const double from[3] = { 0, 0, 0 }, to[3] = { 4, 0, 0 };
    std::copy (from, from + 3, dimension.from);
    std::copy (to, to + 3, dimension.to);
    dimension.offsetMetres = 0.5;
    dimension.direction[1] = 1.0; // offset towards +y
    layer.dimensions = { dimension };
    const scene::Plan plan = scene::PreparePlan (One (layer), &Engine ());
    ASSERT_EQ (plan.lines.size (), 3u);
    // The dimension line, half a metre towards +y.
    EXPECT_NEAR (Rejoin (plan.lines[0].hiA[1], plan.lines[0].loA[1], plan.originY), 0.5, 1e-9);
    EXPECT_NEAR (Rejoin (plan.lines[0].hiB[0], plan.lines[0].loB[0], plan.originX), 4.0, 1e-9);
    // Ticks: two solid quads that turn with the line; the text keeps upright and
    // gives way when the dimension is too short on screen.
    size_t ticks = 0, text = 0;
    for (const scene::PlanGlyph& glyph : plan.glyphs) {
        EXPECT_NE (glyph.flags & scene::kAlongDirection, 0u);
        if ((glyph.flags & scene::kSolid) != 0)
            ++ticks;
        else {
            ++text;
            EXPECT_NE (glyph.flags & scene::kKeepUpright, 0u);
            EXPECT_NE (glyph.flags & scene::kHideShortSpan, 0u);
            EXPECT_GT (glyph.minSpan, 20.0f);
            EXPECT_FLOAT_EQ (glyph.dir[0], 4.0f);
        }
    }
    EXPECT_EQ (ticks, 12u);
    EXPECT_EQ (text, 4u * 6u); // "4.00" -- four glyphs, the point included
    EXPECT_EQ (plan.problems.textsNotLaidOut, 0u);
}

TEST (OverlayScene, AVerticalDimensionIsNothingInPlanAndADimensionIn3D)
{
    layers::Layer layer;
    layer.name = "height";
    layers::Dimension dimension;
    dimension.to[2] = 3.0;
    layer.dimensions = { dimension };
    EXPECT_TRUE (scene::PreparePlan (One (layer), &Engine ()).Empty ());
    const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
    EXPECT_EQ (scene.lines.size (), 3u);
    EXPECT_EQ (scene.problems.dimensionsNotResolved, 0u);
}

TEST (OverlayScene, WithoutTextTheLabelsAreCountedAndTheRestIsDrawn)
{
    layers::Layer layer;
    layer.name = "no text";
    layers::Dimension dimension;
    dimension.to[0] = 2.0;
    layer.dimensions = { dimension };
    layers::Text label;
    label.text = "A";
    layer.texts = { label };
    const scene::Plan plan = scene::PreparePlan (One (layer), nullptr);
    EXPECT_EQ (plan.lines.size (), 3u);
    EXPECT_EQ (plan.problems.textsNotLaidOut, 2u);
    EXPECT_EQ (plan.glyphs.size (), 12u); // the two ticks still
}

TEST (OverlayScene, ScreenTextIsAnchoredToTheViewAndShownIn3D)
{
    layers::Layer layer;
    layer.name = "hud";
    layers::Text label;
    label.text = "Sun hours";
    label.screen = true;
    label.at[0] = 1.0;
    label.at[1] = 0.0;
    label.offsetPixels[0] = -12.0f;
    label.offsetPixels[1] = 12.0f;
    label.align = layers::Align::Right;
    label.baseline = layers::Baseline::Top;
    layer.texts = { label };
    layer.occlusion = layers::Behind::Hide;
    const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
    ASSERT_FALSE (scene.glyphs.empty ());
    for (const scene::SceneGlyph& glyph : scene.glyphs) {
        EXPECT_NE (glyph.flags & scene::kScreenAnchored, 0u);
        EXPECT_FLOAT_EQ (glyph.position[0], 1.0f);
        EXPECT_FLOAT_EQ (glyph.position[1], 0.0f);
        // Right-aligned on the advance: the last glyph's quad overhangs by its bearing and
        // by the distance field's padding -- half the atlas range, 12 atlas px at 40 px to
        // the em, about 2 px at this size (OverlayText.cpp says why the range is wide).
        EXPECT_LE (glyph.offset[0], -12.0f + 3.0f);
        EXPECT_GE (glyph.offset[1], 12.0f - 1e-3f);
    }
    for (const scene::GlyphDraw& draw : scene.glyphDraws)
        EXPECT_EQ (draw.behind, scene::kBehindShow);
}

TEST (OverlayScene, ABackgroundPanelComesBeforeItsGlyphs)
{
    layers::Layer layer;
    layer.name = "panel";
    layers::Text label;
    label.text = "WC";
    label.backgroundRgba = 0xFFFFFFC0u;
    layer.texts = { label };
    const scene::Plan plan = scene::PreparePlan (One (layer), &Engine ());
    ASSERT_GE (plan.glyphs.size (), 18u);
    for (size_t i = 0; i < 6; ++i)
        EXPECT_NE (plan.glyphs[i].flags & scene::kSolid, 0u);
    EXPECT_EQ (plan.glyphs[6].flags & scene::kSolid, 0u);
}

TEST (OverlayScene, AHeatmapTakesItsRangeFromItsValues)
{
    layers::Layer layer;
    layer.name = "sun";
    layers::Mesh mesh;
    mesh.points = { 0, 0, 0, 1, 0, 0, 1, 1, 0 };
    mesh.indices = { 0, 1, 2 };
    mesh.values = { 2.0, 8.0, 5.0 };
    ASSERT_TRUE (layers::PresetStops ("sunhours", mesh.colormap.stops));
    mesh.colormap.bands = 6;
    layer.meshes = { mesh };
    EXPECT_EQ (layers::Validate (layer), "");
    const scene::Scene scene = scene::PrepareScene (One (layer), nullptr);
    ASSERT_EQ (scene.fillDraws.size (), 1u);
    const scene::FillDraw& draw = scene.fillDraws[0];
    EXPECT_TRUE (draw.heatmap);
    EXPECT_FLOAT_EQ (draw.min, 2.0f);
    EXPECT_FLOAT_EQ (draw.max, 8.0f);
    EXPECT_EQ (draw.stopCount, 9u);
    EXPECT_EQ (draw.bands, 6u);
    EXPECT_FLOAT_EQ (scene.fills[1].value, 8.0f);
    EXPECT_TRUE (layers::Prepare3D (One (layer)).occludedFills.empty ());
}

TEST (OverlayScene, TheRampInterpolatesBetweenItsStops)
{
    const std::vector<layers::ColourStop> stops = { { 0.0f, 0x000000FFu }, { 1.0f, 0xFF8000FFu } };
    EXPECT_EQ (scene::RampAt (stops, -1.0f), 0x000000FFu);
    EXPECT_EQ (scene::RampAt (stops, 2.0f), 0xFF8000FFu);
    EXPECT_EQ (scene::RampAt (stops, 0.5f), 0x804000FFu);
    std::vector<layers::ColourStop> viridis;
    ASSERT_TRUE (layers::PresetStops ("viridis", viridis));
    EXPECT_EQ (viridis.front ().rgba, 0x440154FFu);
    EXPECT_EQ (viridis.back ().rgba, 0xFDE725FFu);
    EXPECT_FALSE (layers::PresetStops ("rainbow", viridis));
}

// ⚠️ A SEAM IS NOT AN EDGE. A mesh that repeats its vertices per face -- how a brep's
// faces arrive -- has boundaries everywhere by index; welding by position is what
// keeps the edges to the cube's twelve.
TEST (OverlayScene, ACubeHasTwelveFeatureEdgesSharedOrSplit)
{
    const layers::Mesh shared = Cube (false);
    const layers::Mesh split = Cube (true);
    EXPECT_EQ (scene::FeatureEdges (shared.points, shared.indices, 30.0f).size (), 12u);
    EXPECT_EQ (scene::FeatureEdges (split.points, split.indices, 30.0f).size (), 12u);
    // Past 90 degrees nothing on a cube is a crease.
    EXPECT_EQ (scene::FeatureEdges (shared.points, shared.indices, 100.0f).size (), 0u);
}

TEST (OverlayScene, AFlatQuadHasItsFourBoundariesAndNotItsDiagonal)
{
    const std::vector<double> points = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
    const std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
    const auto edges = scene::FeatureEdges (points, indices, 30.0f);
    EXPECT_EQ (edges.size (), 4u);
    for (const auto& edge : edges)
        EXPECT_FALSE ((edge.first == 0 && edge.second == 2) || (edge.first == 2 && edge.second == 0));
    const std::vector<double> normals = scene::VertexNormals (points, indices);
    ASSERT_EQ (normals.size (), 12u);
    for (size_t v = 0; v < 4; ++v)
        EXPECT_NEAR (normals[v * 3 + 2], 1.0, 1e-12);
}

TEST (OverlayScene, AStyledMeshDrawsItsEdgesWithItsDepthPolicy)
{
    layers::Layer layer;
    layer.name = "ghost";
    layers::Mesh mesh = Cube (true);
    mesh.styled = true;
    mesh.style.shading = layers::Shading::Ghost;
    mesh.style.edgeRgba = 0x202020FFu;
    mesh.style.behind = layers::Behind::Fade;
    layer.meshes = { mesh };
    const scene::Scene scene = scene::PrepareScene (One (layer), nullptr);
    ASSERT_EQ (scene.fillDraws.size (), 1u);
    EXPECT_EQ (scene.fillDraws[0].shading, uint32_t (layers::Shading::Ghost));
    EXPECT_EQ (scene.fillDraws[0].behind, scene::kBehindFade);
    EXPECT_EQ (scene.fills.size (), 36u);
    EXPECT_EQ (scene.lines.size (), 12u);
    for (const scene::SceneLine& line : scene.lines)
        EXPECT_EQ (line.behind, scene::kBehindFade);
}

TEST (OverlayScene, LengthsReadInTheUnitAsked)
{
    EXPECT_EQ (scene::FormatLength (5.0, 2, layers::LengthUnit::Metres, false), "5.00");
    EXPECT_EQ (scene::FormatLength (5.0, 2, layers::LengthUnit::Metres, true), "5.00 m");
    EXPECT_EQ (scene::FormatLength (1.2346, 1, layers::LengthUnit::Centimetres, true), "123.5 cm");
    EXPECT_EQ (scene::FormatLength (0.9, 0, layers::LengthUnit::Millimetres, false), "900");
}

TEST (OverlayScene, ALegendIsARampBarFixedToTheViewWithItsValues)
{
    layers::Layer layer;
    layer.name = "legend";
    layers::Legend legend;
    legend.title = "Sun hours";
    legend.unit = "h";
    ASSERT_TRUE (layers::PresetStops ("sunhours", legend.colormap.stops));
    legend.colormap.autoRange = false;
    legend.colormap.min = 0.0;
    legend.colormap.max = 8.0;
    legend.ticks = 5;
    layer.legends = { legend };
    EXPECT_EQ (layers::Validate (layer), "");
    const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
    // Its panel first, then the bar: fills are drawn in order, so the bar is over it.
    ASSERT_EQ (scene.fillDraws.size (), 2u);
    EXPECT_TRUE (scene.fillDraws[0].screen);
    EXPECT_FALSE (scene.fillDraws[0].heatmap);
    const scene::FillDraw& bar = scene.fillDraws[1];
    EXPECT_TRUE (bar.screen);
    EXPECT_TRUE (bar.heatmap);
    EXPECT_FLOAT_EQ (bar.max, 8.0f);
    // The bar's value runs up it: the top corners hold the maximum.
    float topValue = 0.0f, bottomValue = 0.0f, top = 1e9f, bottom = -1e9f;
    for (uint32_t i = bar.first; i < bar.first + bar.count; ++i) {
        const scene::SceneFillVertex& v = scene.fills[i];
        if (v.offset[1] < top) {
            top = v.offset[1];
            topValue = v.value;
        }
        if (v.offset[1] > bottom) {
            bottom = v.offset[1];
            bottomValue = v.value;
        }
    }
    EXPECT_FLOAT_EQ (topValue, 8.0f);
    EXPECT_FLOAT_EQ (bottomValue, 0.0f);
    EXPECT_NEAR (bottom - top, legend.lengthPixels, 1e-3f);
    // Five values and a title, all fixed to the view.
    std::set<uint32_t> anchored;
    for (const scene::SceneGlyph& glyph : scene.glyphs)
        anchored.insert (glyph.flags & scene::kScreenAnchored);
    EXPECT_EQ (anchored, std::set<uint32_t> { scene::kScreenAnchored });
    EXPECT_GT (scene.glyphs.size (), 5u * 6u);

    // A legend has no values of its own to take a range from.
    layers::Layer unranged;
    unranged.name = "unranged";
    layers::Legend bare;
    ASSERT_TRUE (layers::PresetStops ("greys", bare.colormap.stops));
    unranged.legends = { bare };
    EXPECT_NE (layers::Validate (unranged).find ("needs min and max"), std::string::npos);
}

TEST (OverlayScene, ValidationNamesWhatIsWrong)
{
    layers::Layer layer;
    layer.name = "bad";
    layers::Mesh mesh;
    mesh.points = { 0, 0, 0, 1, 0, 0, 1, 1, 0 };
    mesh.indices = { 0, 1, 2 };
    mesh.values = { 1.0, 2.0 };
    layer.meshes = { mesh };
    EXPECT_NE (layers::Validate (layer).find ("one number per vertex"), std::string::npos);
    layer.meshes.clear ();
    layers::Dimension dimension;
    layer.dimensions = { dimension };
    EXPECT_NE (layers::Validate (layer).find ("two different points"), std::string::npos);
    layer.dimensions.clear ();
    layers::Text label;
    layer.texts = { label };
    EXPECT_NE (layers::Validate (layer).find ("1 to 512 bytes"), std::string::npos);
}

// ⚠️ A BOX'S FACES ARE FLAT. Per-vertex normals averaged three faces into each corner
// and the ghost's view-angle shading bulged every face (the first live run); corner
// normals split at the crease keep each face's own.
TEST (OverlayScene, CornerNormalsKeepABoxFlatAndASeamSmooth)
{
    for (const bool split : { false, true }) {
        const layers::Mesh cube = Cube (split);
        const std::vector<double> normals = scene::CornerNormals (cube.points, cube.indices, 45.0f);
        ASSERT_EQ (normals.size (), cube.indices.size () * 3);
        for (size_t t = 0; t < cube.indices.size (); t += 3) {
            for (size_t k = 0; k < 3; ++k) {
                const double* n = &normals[(t + k) * 3];
                // Axis-aligned: one component is +-1, the others 0.
                const double largest = (std::max) ({ std::fabs (n[0]), std::fabs (n[1]), std::fabs (n[2]) });
                EXPECT_NEAR (largest, 1.0, 1e-9) << (split ? "split" : "shared") << " corner " << t + k;
            }
        }
    }
    // Two triangles meeting at 20 degrees are one smooth surface: their shared corners agree.
    const double lift = std::tan (20.0 * 3.14159265358979 / 180.0);
    const std::vector<double> points = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, lift };
    const std::vector<uint32_t> indices = { 0, 1, 2, 1, 3, 2 };
    const std::vector<double> normals = scene::CornerNormals (points, indices, 45.0f);
    // Vertex 1 is corner 1 of the first triangle and corner 0 of the second.
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR (normals[1 * 3 + c], normals[3 * 3 + c], 1e-12);
}

TEST (OverlayScene, AShadedMeshWithoutNormalsIsShadedFlatPerFace)
{
    layers::Layer layer;
    layer.name = "ghost";
    layers::Mesh mesh = Cube (false);
    mesh.styled = true;
    mesh.style.shading = layers::Shading::Ghost;
    layer.meshes = { mesh };
    const scene::Scene scene = scene::PrepareScene (One (layer), nullptr);
    ASSERT_EQ (scene.fills.size (), 36u);
    for (size_t t = 0; t < 36; t += 3)
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ (scene.fills[t].normal[c], scene.fills[t + 1].normal[c]);
            EXPECT_FLOAT_EQ (scene.fills[t].normal[c], scene.fills[t + 2].normal[c]);
        }
}

// A text on a plane: every corner its own model point on that plane, the text as high
// as it was asked, running along its direction.
TEST (OverlayScene, APlanarTextLiesOnItsPlaneAtItsSize)
{
    layers::Layer layer;
    layer.name = "floor";
    layers::Text label;
    label.text = "Hall";
    label.planar = true;
    label.at[0] = 10.0;
    label.at[1] = 20.0;
    label.at[2] = 3.0;
    label.direction[0] = 0.0;
    label.direction[1] = 1.0; // along +y
    label.sizeMetres = 0.5;
    label.sizePixels = 32.0f;
    label.align = layers::Align::Left;
    label.baseline = layers::Baseline::Bottom;
    layer.texts = { label };
    EXPECT_EQ (layers::Validate (layer), "");
    const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
    ASSERT_FALSE (scene.glyphs.empty ());
    float lowX = 1e9f, highX = -1e9f, lowY = 1e9f, highY = -1e9f;
    for (const scene::SceneGlyph& glyph : scene.glyphs) {
        EXPECT_NE (glyph.flags & scene::kModelQuad, 0u);
        EXPECT_EQ (glyph.flags & scene::kScreenAnchored, 0u);
        EXPECT_FLOAT_EQ (glyph.position[2], 3.0f);
        EXPECT_FLOAT_EQ (glyph.offset[0], 0.0f);
        lowX = (std::min) (lowX, glyph.position[0]);
        highX = (std::max) (highX, glyph.position[0]);
        lowY = (std::min) (lowY, glyph.position[1]);
        highY = (std::max) (highY, glyph.position[1]);
    }
    // Along +y it runs from the anchor; it rises towards normal x direction, -x.
    EXPECT_GT (highY - lowY, 0.8f);  // four letters at half a metre
    EXPECT_LT (highX - lowX, 0.8f);  // one line high
    EXPECT_LE (highX, 10.0f + 0.1f); // the baseline on the anchor, the glyphs rising to -x
    EXPECT_GE (lowY, 20.0f - 0.1f);  // left aligned: from the anchor onwards
    // A plane needs a direction across its normal.
    layer.texts[0].direction[0] = 0.0;
    layer.texts[0].direction[1] = 0.0;
    layer.texts[0].direction[2] = 2.0;
    EXPECT_NE (layers::Validate (layer).find ("across its normal"), std::string::npos);
}

TEST (OverlayScene, AHorizontalLegendPlacedAnywhereSaysItsOwnTicks)
{
    layers::Layer layer;
    layer.name = "legend";
    layers::Legend legend;
    ASSERT_TRUE (layers::PresetStops ("slope", legend.colormap.stops));
    legend.colormap.autoRange = false;
    legend.colormap.min = 0.0;
    legend.colormap.max = 30.0;
    legend.horizontal = true;
    legend.placed = true;
    legend.screen[0] = 0.5f;
    legend.screen[1] = 1.0f;
    legend.corner = layers::Corner::BottomLeft;
    legend.tickValues = { 0.0, 10.0, 30.0 };
    legend.tickLabels = { "low", "mid", "high" }; // no fl, ff ligatures to count around
    legend.backgroundRgba = 0;
    layer.legends = { legend };
    EXPECT_EQ (layers::Validate (layer), "");
    const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
    ASSERT_EQ (scene.fillDraws.size (), 1u); // no panel asked for
    const scene::FillDraw& bar = scene.fillDraws[0];
    float leftValue = 0.0f, rightValue = 0.0f, left = 1e9f, right = -1e9f;
    for (uint32_t i = bar.first; i < bar.first + bar.count; ++i) {
        const scene::SceneFillVertex& v = scene.fills[i];
        EXPECT_FLOAT_EQ (v.position[0], 0.5f);
        EXPECT_FLOAT_EQ (v.position[1], 1.0f);
        if (v.offset[0] < left) {
            left = v.offset[0];
            leftValue = v.value;
        }
        if (v.offset[0] > right) {
            right = v.offset[0];
            rightValue = v.value;
        }
    }
    EXPECT_FLOAT_EQ (leftValue, 0.0f);
    EXPECT_FLOAT_EQ (rightValue, 30.0f);
    EXPECT_NEAR (right - left, legend.lengthPixels, 1e-3f);
    // Three ticks and "low" "mid" "high": 3 + 3 + 4 glyphs.
    size_t text = 0;
    for (const scene::SceneGlyph& glyph : scene.glyphs)
        text += (glyph.flags & scene::kSolid) == 0 ? 1 : 0;
    EXPECT_EQ (text, (3u + 3u + 4u) * 6u);
}

// A text's halo grows with it as drawn unless the caller fixes it: the vertex carries
// the fixed pixels, or -- negative -- the automatic halo's scale (HaloReach sizes it).
TEST (OverlayScene, AHaloIsAutomaticUnlessFixed)
{
    layers::Layer layer;
    layer.name = "halo";
    layers::Text automatic;
    automatic.text = "auto";
    layers::Text scaled = automatic;
    scaled.haloScale = 0.5f;
    layers::Text fixed = automatic;
    fixed.haloPixels = 2.0f;
    for (layers::Text* text : { &automatic, &scaled, &fixed }) {
        text->screen = true;
        text->at[0] = 0.5;
        text->at[1] = 0.5;
    }
    EXPECT_EQ (automatic.haloPixels, layers::kAutoHalo);
    for (const auto& [text, expected] :
         { std::make_pair (automatic, -1.0f), std::make_pair (scaled, -0.5f), std::make_pair (fixed, 2.0f) }) {
        layer.texts = { text };
        EXPECT_EQ (layers::Validate (layer), "");
        const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
        ASSERT_FALSE (scene.glyphs.empty ());
        for (const scene::SceneGlyph& glyph : scene.glyphs)
            EXPECT_FLOAT_EQ (glyph.haloPixels, expected);
    }
    fixed.haloPixels = 9.0f;
    layer.texts = { fixed };
    EXPECT_NE (layers::Validate (layer), "");
}

// An open polyline's ends: an arrowhead and a dot, turned with their own segments, as
// solid glyph triangles; the line itself goes to the guest because of them.
TEST (OverlayScene, APolylineEndsInItsArrows)
{
    layers::Layer layer;
    layer.name = "flow";
    layers::Polyline flow;
    flow.points = { 0, 0, 0, 2, 0, 0, 2, 3, 0 };
    flow.startArrow = layers::Terminator::Dot;
    flow.endArrow = layers::Terminator::Arrow;
    flow.arrowSizePixels = 14.0f;
    layer.polylines = { flow };
    EXPECT_TRUE (layers::DrawnByGuest (layer.polylines[0], layer));
    EXPECT_EQ (layers::Validate (layer), "");
    const scene::Scene scene = scene::PrepareScene (One (layer), nullptr);
    EXPECT_EQ (scene.lines.size (), 2u);
    ASSERT_EQ (scene.glyphs.size (), 12u); // two terminators, six vertices each
    // The dot at the start, along the first segment; the arrow at the end, along the last.
    EXPECT_FLOAT_EQ (scene.glyphs[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ (scene.glyphs[0].dir[0], 2.0f);
    EXPECT_FLOAT_EQ (scene.glyphs[6].position[1], 3.0f);
    EXPECT_FLOAT_EQ (scene.glyphs[6].dir[1], 3.0f);
    float back = 0.0f;
    for (size_t i = 6; i < 12; ++i) {
        EXPECT_NE (scene.glyphs[i].flags & scene::kSolid, 0u);
        back = (std::min) (back, scene.glyphs[i].offset[0]);
    }
    EXPECT_FLOAT_EQ (back, -14.0f); // the head's body lies back along the line, its size long

    flow.closed = true; // a closed polyline has no ends
    layer.polylines = { flow };
    EXPECT_TRUE (scene::PrepareScene (One (layer), nullptr).glyphs.empty ());
}

// A dimension's text in its own colour and halo, its terminators sized, or none.
TEST (OverlayScene, ADimensionTakesItsTextStyleAndTerminatorSize)
{
    layers::Layer layer;
    layer.name = "dimension";
    layers::Dimension dimension;
    dimension.to[0] = 4.0;
    dimension.rgba = 0xFF5000FFu;
    dimension.textRgba = 0x202020FFu;
    dimension.haloRgba = 0xFFFFFFC0u;
    dimension.haloPixels = 1.0f;
    dimension.terminator = layers::Terminator::Arrow;
    dimension.terminatorSizePixels = 16.0f;
    layer.dimensions = { dimension };
    EXPECT_EQ (layers::Validate (layer), "");
    const scene::Scene scene = scene::PrepareScene (One (layer), &Engine ());
    size_t text = 0;
    float reach = 0.0f;
    for (const scene::SceneGlyph& glyph : scene.glyphs) {
        if ((glyph.flags & scene::kSolid) != 0) {
            reach = (std::max) (reach, std::fabs (glyph.offset[0]));
            continue;
        }
        ++text;
        EXPECT_EQ (glyph.rgba, layers::ToUnorm (0x202020FFu)); // as the GPU reads it
        EXPECT_EQ (glyph.halo, layers::ToUnorm (0xFFFFFFC0u));
        EXPECT_FLOAT_EQ (glyph.haloPixels, 1.0f);
    }
    EXPECT_GT (text, 0u);
    EXPECT_FLOAT_EQ (reach, 16.0f);
    dimension.terminator = layers::Terminator::None;
    layer.dimensions = { dimension };
    for (const scene::SceneGlyph& glyph : scene::PrepareScene (One (layer), &Engine ()).glyphs)
        EXPECT_EQ (glyph.flags & scene::kSolid, 0u);
}

// Only what changed is built again: a layer that stands is reused as it was built, a
// replaced one is built anew, and the whole is what building everything would give.
TEST (OverlayScene, ALayerThatStandsIsReusedAndOnlyTheChangedOneIsBuilt)
{
    scene::ForgetDrafts ();
    layers::Layer boxes;
    boxes.name = "boxes";
    layers::Mesh cube = Cube (true);
    cube.styled = true;
    cube.style.shading = layers::Shading::Ghost;
    cube.style.edgeRgba = 0x103060FFu;
    boxes.meshes = { cube };
    const auto standing = std::make_shared<const layers::Layer> (boxes);
    layers::Layer labels;
    labels.name = "labels";
    layers::Text label;
    label.text = "A";
    labels.texts = { label };

    const scene::Scene first =
        scene::PrepareScene ({ standing, std::make_shared<const layers::Layer> (labels) }, &Engine ());
    EXPECT_EQ (first.cost.layersBuilt, 2u);
    EXPECT_EQ (first.cost.layersReused, 0u);

    labels.texts[0].text = "B";
    const auto changed = std::make_shared<const layers::Layer> (labels);
    const scene::Scene second = scene::PrepareScene ({ standing, changed }, &Engine ());
    EXPECT_EQ (second.cost.layersBuilt, 1u);
    EXPECT_EQ (second.cost.layersReused, 1u);

    scene::ForgetDrafts ();
    const scene::Scene fresh = scene::PrepareScene ({ standing, changed }, &Engine ());
    EXPECT_EQ (fresh.cost.layersBuilt, 2u);
    ASSERT_EQ (second.fills.size (), fresh.fills.size ());
    ASSERT_EQ (second.lines.size (), fresh.lines.size ());
    ASSERT_EQ (second.glyphs.size (), fresh.glyphs.size ());
    for (size_t i = 0; i < fresh.fills.size (); ++i)
        EXPECT_EQ (std::memcmp (&second.fills[i], &fresh.fills[i], sizeof (scene::SceneFillVertex)), 0) << i;
    for (size_t i = 0; i < fresh.glyphs.size (); ++i)
        EXPECT_EQ (std::memcmp (&second.glyphs[i], &fresh.glyphs[i], sizeof (scene::SceneGlyph)), 0) << i;
    EXPECT_EQ (second.pages.size (), fresh.pages.size ());
}

// Two layers whose text sits on the same atlas page share it: one page, one draw.
TEST (OverlayScene, LayersSharingAnAtlasPageShareOneSlot)
{
    scene::ForgetDrafts ();
    std::vector<std::shared_ptr<const layers::Layer>> all;
    for (const char* name : { "one", "two" }) {
        layers::Layer layer;
        layer.name = name;
        layers::Text label;
        label.text = "12";
        label.screen = true;
        layer.texts = { label };
        all.push_back (std::make_shared<const layers::Layer> (layer));
    }
    const scene::Scene drawn = scene::PrepareScene (all, &Engine ());
    EXPECT_EQ (drawn.pages.size (), 1u);
    EXPECT_EQ (drawn.glyphDraws.size (), 1u);
}
