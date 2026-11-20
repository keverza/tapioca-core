#include "ArchViz/EditLayers.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditPolyline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace el = geomsrv::archviz::editlayers;
namespace layers = geomsrv::archviz::overlaylayers;

geo::DerivedRegion WithHole ()
{
    geo::DerivedRegion region;
    region.rings = {
        { { 0, 0, 10 },
          { 4, 0, 11 },
          { 4, 4, 12 },
          { 3, 4, 13 },
          { 3, 1, 14 },
          { 1, 1, 15 },
          { 1, 4, 16 },
          { 0, 4, 17 } },
        { { 1.5, 0.25, 20 }, { 1.5, 0.75, 21 }, { 2.5, 0.75, 22 }, { 2.5, 0.25, 23 } },
    };
    region.seed = { 0.5, 0.5, 42 };
    region.area = 9.5;
    return region;
}

double TriangleArea (edit::Vec3 a, edit::Vec3 b, edit::Vec3 c)
{
    return ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5;
}

double ExpandedArea (const std::vector<edit::Vec3>& triangles)
{
    double area = 0.0;
    for (size_t i = 0; i + 2 < triangles.size (); i += 3)
        area += TriangleArea (triangles[i], triangles[i + 1], triangles[i + 2]);
    return area;
}

double IndexedArea (const geo::LandscapePreviewMesh& mesh)
{
    double area = 0.0;
    for (size_t i = 0; i + 2 < mesh.triangles.size (); i += 3)
        area += TriangleArea (mesh.vertices.at (mesh.triangles[i]), mesh.vertices.at (mesh.triangles[i + 1]),
                              mesh.vertices.at (mesh.triangles[i + 2]));
    return area;
}

void ExpectRawVertices (const geo::DerivedRegion& region, const geo::LandscapePreviewMesh& mesh)
{
    size_t index = 0;
    for (const auto& ring : region.rings) {
        for (const auto& point : ring) {
            ASSERT_LT (index, mesh.vertices.size ());
            EXPECT_DOUBLE_EQ (mesh.vertices[index].x, point.x);
            EXPECT_DOUBLE_EQ (mesh.vertices[index].y, point.y);
            EXPECT_DOUBLE_EQ (mesh.vertices[index].z, region.seed.z);
            ++index;
        }
    }
    EXPECT_EQ (index, mesh.vertices.size ());
    EXPECT_EQ (mesh.triangles.size () % 3, 0u);
    for (uint32_t vertex : mesh.triangles)
        ASSERT_LT (vertex, mesh.vertices.size ());
}
} // namespace

TEST (LandscapePreview, IndexedHoleFillMatchesLegacyAreaAndSharesRawVerticesInBothWindings)
{
    auto region = WithHole ();
    for (bool reverse : { false, true }) {
        if (reverse)
            for (auto& ring : region.rings)
                std::reverse (ring.begin (), ring.end ());
        geo::LandscapePreviewMesh mesh, again;
        std::vector<edit::Vec3> legacy;
        std::string error = "old failure";
        ASSERT_TRUE (geo::BuildLandscapePreviewMesh (region, mesh, error)) << error;
        EXPECT_TRUE (error.empty ());
        ASSERT_TRUE (geo::BuildLandscapePreviewMesh (region, again, error)) << error;
        ASSERT_TRUE (geo::LandscapePreviewTriangles (region, legacy, error)) << error;
        ExpectRawVertices (region, mesh);
        EXPECT_EQ (mesh.vertices, again.vertices);
        EXPECT_EQ (mesh.triangles, again.triangles);
        EXPECT_EQ (mesh.vertices.size (), 12u);
        EXPECT_EQ (mesh.triangles.size (), 36u);
        EXPECT_LT (mesh.vertices.size (), legacy.size ());
        EXPECT_NEAR (IndexedArea (mesh), 9.5, 1.0e-12);
        EXPECT_NEAR (IndexedArea (mesh), ExpandedArea (legacy), 1.0e-9);
        for (size_t index = 0; index < mesh.triangles.size (); index += 3) {
            const auto a = mesh.vertices[mesh.triangles[index]];
            const auto b = mesh.vertices[mesh.triangles[index + 1]];
            const auto c = mesh.vertices[mesh.triangles[index + 2]];
            EXPECT_GT (TriangleArea (a, b, c), 0);
            for (const auto& weights : { std::pair { 0.6, 0.2 }, { 0.2, 0.6 }, { 0.2, 0.2 } }) {
                const auto point = a * weights.first + b * weights.second + c * (1 - weights.first - weights.second);
                EXPECT_TRUE (point.y <= 1 || point.x <= 1 || point.x >= 3);
                EXPECT_FALSE (point.x > 1.5 && point.x < 2.5 && point.y > 0.25 && point.y < 0.75);
            }
        }
    }
}

TEST (LandscapePreview, ClosedCollinearSurveyRingsRetainRawIndexOffsets)
{
    geo::DerivedRegion region;
    region.rings = {
        { { 0, 0, 0 }, { 2, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 }, { 0, 0, 0 } },
        { { 1, 1, 0 }, { 1, 2, 0 }, { 2, 2, 0 }, { 2, 1, 0 }, { 1, 1, 0 } },
    };
    region.seed.z = -12;
    for (auto& ring : region.rings)
        for (auto& point : ring) {
            point.x += 2600000;
            point.y += 1200000;
        }
    geo::LandscapePreviewMesh mesh;
    std::string error;
    ASSERT_TRUE (geo::BuildLandscapePreviewMesh (region, mesh, error)) << error;
    ExpectRawVertices (region, mesh);
    EXPECT_EQ (mesh.vertices.size (), 11u);
    EXPECT_NEAR (IndexedArea (mesh), 15, 1.0e-12);
    EXPECT_TRUE (std::any_of (mesh.triangles.begin (), mesh.triangles.end (), [] (auto index) { return index >= 6; }));
}

TEST (LandscapePreview, ConcaveSingleOuterUsesSixVerticesInsteadOfTwelveTriangleCorners)
{
    geo::DerivedRegion region;
    region.rings = { { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 1, 0 }, { 1, 1, 0 }, { 1, 4, 0 }, { 0, 4, 0 } } };
    geo::LandscapePreviewMesh mesh;
    std::vector<edit::Vec3> legacy;
    std::string error;
    ASSERT_TRUE (geo::BuildLandscapePreviewMesh (region, mesh, error)) << error;
    ASSERT_TRUE (geo::LandscapePreviewTriangles (region, legacy, error)) << error;
    ExpectRawVertices (region, mesh);
    EXPECT_EQ (mesh.vertices.size (), 6u);
    EXPECT_EQ (mesh.triangles.size (), 12u);
    EXPECT_EQ (legacy.size (), 12u);
    EXPECT_NEAR (IndexedArea (mesh), 7, 1.0e-12);
    EXPECT_NEAR (IndexedArea (mesh), ExpandedArea (legacy), 1.0e-12);
}

TEST (LandscapePreview, InvalidHierarchyRepairAndNonfiniteInputsRefuseAtomically)
{
    std::vector<geo::DerivedRegion> invalid (7, WithHole ());
    invalid[0].rings.clear ();
    invalid[1].rings[1] = { { 20, 0, 0 }, { 21, 0, 0 }, { 20, 1, 0 } };
    invalid[2].rings = { { { 0, 0, 0 }, { 4, 4, 0 }, { 4, 0, 0 }, { 0, 3, 0 } } };
    invalid[3].rings[0][1].x = std::numeric_limits<double>::infinity ();
    invalid[4].seed.z = std::numeric_limits<double>::quiet_NaN ();
    invalid[5].rings[1] = { { 1, 1, 0 }, { 2, 2, 0 } };
    invalid[6].rings = {
        { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 }, { 0, 0, 0 }, { 4, 0, 0 }, { 4, 4, 0 }, { 0, 4, 0 } }
    };
    for (const auto& region : invalid) {
        geo::LandscapePreviewMesh mesh { { { 80, 81, 82 } }, { 7, 8, 9 } };
        const auto before = mesh;
        std::string error;
        EXPECT_FALSE (geo::BuildLandscapePreviewMesh (region, mesh, error));
        EXPECT_EQ (error.find ("Landscape preview triangulation failed; outlines only."), 0u);
        EXPECT_EQ (mesh.vertices, before.vertices);
        EXPECT_EQ (mesh.triangles, before.triangles);
    }
}

TEST (LandscapePreview, ManyClassifiedHolesKeepAreaCheckedConstrainedFill)
{
    geo::DerivedRegion region;
    region.rings = { { { 0, 0, 0 }, { 33, 0, 0 }, { 33, 17, 0 }, { 0, 17, 0 } } };
    for (size_t row = 0; row < 8; ++row) {
        for (size_t column = 0; column < 16; ++column) {
            const double x = 1.0 + column * 2.0, y = 1.0 + row * 2.0;
            region.rings.push_back ({ { x, y, 0 }, { x, y + 0.5, 0 }, { x + 0.5, y + 0.5, 0 }, { x + 0.5, y, 0 } });
        }
    }
    ASSERT_EQ (region.rings.size (), 129u);
    geo::LandscapePreviewMesh mesh;
    std::string error;
    ASSERT_TRUE (geo::BuildLandscapePreviewMesh (region, mesh, error)) << error;
    ExpectRawVertices (region, mesh);
    EXPECT_NEAR (IndexedArea (mesh), 529, 1.0e-9);
    std::vector<edit::Vec3> legacy;
    ASSERT_TRUE (geo::LandscapePreviewTriangles (region, legacy, error)) << error;
    EXPECT_NEAR (IndexedArea (mesh), ExpandedArea (legacy), 1.0e-9);
}

TEST (LandscapePreview, LegacyTerrainMaskStillAcceptsDisconnectedOutersAndAHole)
{
    geo::DerivedRegion mask;
    mask.rings = {
        { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } },
        { { 2, 2, 0 }, { 2, 4, 0 }, { 4, 4, 0 }, { 4, 2, 0 } },
        { { 20, 0, 0 }, { 22, 0, 0 }, { 22, 2, 0 }, { 20, 2, 0 } },
    };
    std::vector<edit::Vec3> legacy;
    std::string error;
    ASSERT_TRUE (geo::LandscapePreviewTriangles (mask, legacy, error)) << error;
    EXPECT_NEAR (ExpandedArea (legacy), 100.0, 1.0e-12);
    geo::LandscapePreviewMesh mesh;
    EXPECT_FALSE (geo::BuildLandscapePreviewMesh (mask, mesh, error));
}

TEST (LandscapePreviewLayers, PublishedMeshKeepsSharedIndicesAndFeedbackDatum)
{
    edit::Document doc;
    doc.landscape.emplace ();
    geo::LandscapeResult result;
    result.regions = { WithHole () };
    el::Feedback feedback;
    feedback.landscape = &result;
    feedback.landscapeZ = 125;
    layers::Layer layer;
    const el::Style style;
    el::AddLandscape (doc, feedback, style, layer);
    ASSERT_EQ (layer.meshes.size (), 1u);
    EXPECT_EQ (layer.polylines.size (), 2u);
    EXPECT_TRUE (layer.texts.empty ());
    const auto& mesh = layer.meshes[0];
    EXPECT_EQ (mesh.points.size (), 36u);
    EXPECT_EQ (mesh.indices.size (), 36u);
    EXPECT_LT (mesh.points.size (), mesh.indices.size () * 3);
    EXPECT_EQ (mesh.rgba, style.landscapeFill);
    EXPECT_EQ (mesh.style.shading, layers::Shading::Flat);
    geo::LandscapePreviewMesh preview;
    std::string error;
    ASSERT_TRUE (geo::BuildLandscapePreviewMesh (result.regions[0], preview, error)) << error;
    EXPECT_EQ (mesh.indices, preview.triangles);
    for (size_t i = 0; i < preview.vertices.size (); ++i) {
        EXPECT_DOUBLE_EQ (mesh.points[i * 3], preview.vertices[i].x);
        EXPECT_DOUBLE_EQ (mesh.points[i * 3 + 1], preview.vertices[i].y);
        EXPECT_DOUBLE_EQ (mesh.points[i * 3 + 2], 125);
    }
}

TEST (LandscapePreviewLayers, DisjointRegionsAreIndependentAndBadFillKeepsOutlinesAndWarning)
{
    edit::Document doc;
    doc.landscape.emplace ();
    geo::LandscapeResult result;
    result.regions = { WithHole (), WithHole () };
    for (auto& ring : result.regions[1].rings)
        for (auto& point : ring)
            point.x += 20;
    el::Feedback feedback;
    feedback.landscape = &result;
    feedback.landscapeZ = 6;
    layers::Layer layer;
    el::AddLandscape (doc, feedback, {}, layer);
    EXPECT_EQ (layer.meshes.size (), 2u);
    EXPECT_EQ (layer.polylines.size (), 4u);
    EXPECT_TRUE (layer.texts.empty ());

    result.regions[0].rings[1] = { { 20, 0, 0 }, { 21, 0, 0 }, { 20, 1, 0 } };
    layer = {};
    el::AddLandscape (doc, feedback, {}, layer);
    EXPECT_EQ (layer.meshes.size (), 1u);
    EXPECT_EQ (layer.polylines.size (), 4u);
    ASSERT_EQ (layer.texts.size (), 1u);
    EXPECT_EQ (layer.texts[0].text.find ("Landscape preview triangulation failed; outlines only."), 0u);
    EXPECT_DOUBLE_EQ (layer.texts[0].at[0], result.regions[0].seed.x);
    EXPECT_DOUBLE_EQ (layer.texts[0].at[1], result.regions[0].seed.y);
    EXPECT_DOUBLE_EQ (layer.texts[0].at[2], 6);
}

TEST (LandscapePreviewLayers, FaceSelectionAndHoverKeepExistingFillStyleAndOutlineOnlyIdle)
{
    edit::Document doc;
    doc.landscape.emplace ();
    geo::LandscapeResult result;
    result.revision = 8;
    result.faces = { WithHole () };
    result.faces[0].faceId = 77;
    edit::Selection selection;
    el::Feedback feedback;
    feedback.landscape = &result;
    feedback.selection = &selection;
    layers::Layer layer;
    const el::Style style;
    // #160: an idle face is outlined only while faces are being picked (Face mode).
    el::AddLandscape (doc, feedback, style, layer);
    EXPECT_TRUE (layer.polylines.empty ());
    EXPECT_TRUE (layer.meshes.empty ());
    feedback.landscapeFaces = true;
    layer = {};
    el::AddLandscape (doc, feedback, style, layer);
    EXPECT_EQ (layer.polylines.size (), 2u);
    EXPECT_TRUE (layer.meshes.empty ());
    feedback.landscapeFaces = false;
    const edit::Ref ref { edit::kNoId, edit::Part::LandscapeFace, 77, 8 };
    selection.Set ({ ref });
    layer = {};
    el::AddLandscape (doc, feedback, style, layer);
    ASSERT_EQ (layer.meshes.size (), 1u);
    EXPECT_EQ (layer.meshes[0].rgba, (style.landscapeSelected & 0xFFFFFF00u) | 0x28u);
    const auto indices = layer.meshes[0].indices;
    selection.Clear ();
    selection.hover = ref;
    layer = {};
    el::AddLandscape (doc, feedback, style, layer);
    ASSERT_EQ (layer.meshes.size (), 1u);
    EXPECT_EQ (layer.meshes[0].rgba, (style.landscapeHover & 0xFFFFFF00u) | 0x28u);
    EXPECT_EQ (layer.meshes[0].indices, indices);
}

// #189: a construction input is not drawn as a region. Only the region built from it gets the
// solid outline and the fill; the input keeps the dashed source line every source has.
TEST (LandscapePreviewLayers, AConstructionInputIsNotDrawnAsARegion189)
{
    edit::Document doc;
    doc.landscape.emplace ();
    geo::LandscapeResult result;
    result.regions = { WithHole (), WithHole () };
    for (auto& ring : result.regions[1].rings)
        for (auto& point : ring)
            point.x += 20;
    result.regions[0].objectId = 3;
    result.regions[0].constructionOf = 9;
    result.regions[1].objectId = 9;
    el::Feedback feedback;
    feedback.landscape = &result;
    feedback.landscapeZ = 6;
    layers::Layer layer;
    el::AddLandscape (doc, feedback, {}, layer);
    ASSERT_EQ (layer.meshes.size (), 1u) << "one fill: the result's";
    ASSERT_EQ (layer.polylines.size (), 2u) << "the result's outer ring and hole";
    for (const auto& line : layer.polylines) {
        EXPECT_TRUE (line.dashMetres.empty ());
        EXPECT_GE (line.points[0], 20.0) << "the input at x 0..4 has no solid outline";
    }
}

// #160/#189 live run 2026-11-19: outside the Boolean pick, the result and its inputs could not be
// told apart. Every planar face was outlined solid in the source colour, over every source line,
// so the inputs' dashes never showed. Now, from the default drawing workflow (two region lines,
// Union): the inputs' lines are grey and finely dashed, nothing solid is drawn over them, and only
// the result is outlined solid -- heavier than a plain region -- and filled.
TEST (LandscapePreviewLayers, BooleanInputsReadAsConstructionAndTheResultAsTheShape160)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    const auto square = [&] (double x) {
        const auto line =
            edit::AddPolyline (doc, { { x, 0, 0 }, { x + 10, 0, 0 }, { x + 10, 10, 0 }, { x, 10, 0 } }, true);
        EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
        return doc.landscape->objects.back ();
    };
    const auto a = square (0), b = square (5), plain = square (30);
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Union;
    command.name = "Union";
    command.subject.input = a.footprintOperation;
    command.operand.input = b.footprintOperation;
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    geo::LandscapeResult result;
    ASSERT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    ASSERT_FALSE (result.faces.empty ());

    el::Feedback feedback;
    feedback.landscape = &result;
    layers::Layer layer;
    const el::Style style;
    el::AddLandscape (doc, feedback, style, layer);

    const auto inside = [] (const layers::Polyline& line, double lo, double hi) {
        for (size_t i = 0; i < line.points.size (); i += 3)
            if (line.points[i] < lo - 1e-9 || line.points[i] > hi + 1e-9)
                return false;
        return true;
    };
    size_t construction = 0, resultOutlines = 0, plainOutlines = 0;
    for (const auto& line : layer.polylines) {
        if (line.rgba == style.landscapeConstruction) {
            ++construction;
            EXPECT_EQ (line.dashMetres, (std::vector<float> { 0.15f, 0.15f }));
            EXPECT_LT (line.widthPixels, style.lineWidth);
            EXPECT_TRUE (inside (line, 0, 15));
            continue;
        }
        if (!line.dashMetres.empty ()) {
            EXPECT_TRUE (inside (line, 30, 40)) << "only the plain region's line keeps the source dash";
            continue;
        }
        // Solid: a region's outline, never an idle face's.
        if (inside (line, 0, 15)) {
            ++resultOutlines;
            EXPECT_EQ (line.widthPixels, style.lineWidth + 1.0f) << "the result is drawn heavier";
            double lo = 1e9, hi = -1e9;
            for (size_t i = 0; i < line.points.size (); i += 3)
                lo = std::min (lo, line.points[i]), hi = std::max (hi, line.points[i]);
            EXPECT_NEAR (lo, 0.0, 1e-9) << "the union's one outline, not an input square";
            EXPECT_NEAR (hi, 15.0, 1e-9);
        }
        else {
            ++plainOutlines;
            EXPECT_EQ (line.widthPixels, style.lineWidth);
        }
    }
    EXPECT_EQ (construction, 2u) << "both input squares, grey and finely dashed";
    EXPECT_EQ (resultOutlines, 1u);
    EXPECT_EQ (plainOutlines, 1u);
    EXPECT_EQ (layer.meshes.size (), 2u) << "fills: the result and the plain region, no input";
    (void) plain;
}
