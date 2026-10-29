#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditSession.hpp"
#include "ArchViz/EditLayers.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace {
edit::Document Region ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error));
    const auto line = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error)) << error;
    return doc;
}
} // namespace

TEST (EditRegionSurface, ExistingHasNoVerticalTargetAndConstantIsAnAbsoluteLevel)
{
    lg::RegionSurface surface;
    EXPECT_FALSE (lg::RegionSurfaceElevation (surface, { 3, 4, -100 }));
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.first.z = 12.345;
    EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { 3, 4, -100 }), 12.345);
    EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { -300, 400, 100 }), 12.345);
}

TEST (EditRegionSurface, TwoPointsSetTheirExactElevationsAndTheirDirectionalGrade)
{
    for (double offset : { 0.0, 1000000.0, -1000000.0 }) {
        lg::RegionSurface surface;
        surface.elevation = lg::RegionElevationMode::Constant;
        surface.slope = lg::RegionSlopeMode::TwoPoints;
        surface.first = { offset + 2, offset + 3, 10 };
        surface.second = { offset + 8, offset + 11, 10.5 };
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, surface.first), 10);
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, surface.second), 10.5);
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { offset + 5, offset + 7, 0 }), 10.25);
        EXPECT_NEAR (*lg::RegionSurfaceElevation (surface, { offset - 3, offset + 13, 0 }), 10.25, 1e-12)
            << "perpendicular to the grade direction is level";
    }
}

TEST (EditRegionSurface, ContourLineFallsPerpendicularlyOnTheExplicitSide)
{
    lg::RegionSurface surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::ContourLine;
    surface.first = { 10, 20, 15 };
    surface.second = { 20, 20, -100 }; // the second point's Z is not a second elevation target
    surface.gradePercent = 5;
    for (const auto side : { lg::RegionFallSide::Left, lg::RegionFallSide::Right }) {
        surface.fallsTo = side;
        const double sign = side == lg::RegionFallSide::Left ? -1 : 1;
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { 100, 20, 0 }), 15);
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { 15, 30, 0 }), 15 + sign * 0.5);
        EXPECT_DOUBLE_EQ (*lg::RegionSurfaceElevation (surface, { 15, 10, 0 }), 15 - sign * 0.5);
    }
}

TEST (EditRegionSurface, MetadataAndExistingMaterialSpecRoundTripWithExactUndoAndUnchangedBoundary)
{
    const auto doc = Region ();
    edit::Session session (doc);
    const auto object = doc.landscape->objects[0].id;
    lg::RegionSurface surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::ContourLine;
    surface.first = { 0, 0, 12 };
    surface.second = { 10, 0, 0 };
    lg::MaterialSpec material;
    material.preset = lg::MaterialPreset::HardPaving;
    material.fillGuid = "00112233-4455-6677-8899-AABBCCDDEEFF";
    material.fillName = "Named fill";
    material.surfaceGuid = "11223344-5566-7788-99AA-BBCCDDEEFF00";
    material.surfaceName = "Named surface";
    ASSERT_TRUE (session.Apply ([&] (auto& d, auto& error) {
        return lg::SetRegionSurface (d, object, surface, material, error);
    })) << session.Message ();
    EXPECT_EQ (session.UndoDepth (), 1u);
    const auto after = session.Doc ();
    EXPECT_EQ (after.entities, doc.entities);
    EXPECT_EQ (after.landscape->operations, doc.landscape->operations);
    EXPECT_EQ (after.landscape->regions, doc.landscape->regions);
    EXPECT_EQ (after.landscape->objects[0].baseMaterial, material);
    std::string json, error;
    ASSERT_TRUE (lg::WriteLandscape (after, lg::LandscapeStorage::Virtual, json, error));
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, after.landscape);
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().landscape, doc.landscape);
    ASSERT_TRUE (session.Redo ());
    EXPECT_EQ (session.Doc ().landscape, after.landscape);
    surface.second = surface.first;
    EXPECT_FALSE (lg::SetRegionSurface (loaded, object, surface, material, error));
    EXPECT_EQ (loaded.landscape, after.landscape);
    surface = *after.landscape->objects[0].surface;
    material.fillGuid = "missing-not-a-guid";
    EXPECT_FALSE (lg::SetRegionSurface (loaded, object, surface, material, error));
    EXPECT_EQ (loaded.landscape, after.landscape);
}

TEST (EditRegionSurface, SurfaceValidationRefusesContradictoryAndDegenerateDefinitions)
{
    lg::RegionSurface surface;
    std::string error;
    surface.slope = lg::RegionSlopeMode::TwoPoints;
    EXPECT_FALSE (lg::ValidateRegionSurface (surface, error));
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.second = surface.first;
    EXPECT_FALSE (lg::ValidateRegionSurface (surface, error));
    surface.second.x = 1;
    surface.gradePercent = -1;
    EXPECT_FALSE (lg::ValidateRegionSurface (surface, error));
    surface.gradePercent = 2;
    surface.first.z = std::numeric_limits<double>::infinity ();
    EXPECT_FALSE (lg::ValidateRegionSurface (surface, error));
    EXPECT_FALSE (lg::RegionSurfaceElevation (surface, {}));
}

TEST (EditRegionSurface, PreviewUsesTheTargetPlaneWithoutMovingAuthoredLinework)
{
    auto doc = Region ();
    const auto lines = doc.entities;
    auto surface = *doc.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Constant;
    surface.slope = lg::RegionSlopeMode::ContourLine;
    surface.first.z = 12;
    surface.gradePercent = 5;
    surface.fallsTo = lg::RegionFallSide::Left;
    std::string error;
    ASSERT_TRUE (lg::SetRegionSurface (doc, doc.landscape->objects[0].id, surface, {}, error));
    lg::LandscapeResult landscape;
    ASSERT_TRUE (lg::EvaluateLandscape (doc, landscape));
    namespace el = geomsrv::archviz::editlayers;
    el::Feedback feedback;
    feedback.landscape = &landscape;
    feedback.landscapeZ = -100;
    geomsrv::archviz::overlaylayers::Layer layer;
    el::AddLandscape (doc, feedback, {}, layer);
    ASSERT_EQ (layer.meshes.size (), 1u);
    for (size_t i = 0; i < layer.meshes[0].points.size (); i += 3) {
        const auto& p = layer.meshes[0].points;
        EXPECT_NEAR (p[i + 2], 12 - p[i + 1] * 0.05, 1e-12);
    }
    EXPECT_EQ (doc.entities, lines);
}

TEST (EditRegionSurface, NewRegionUsesItsAuthoredBoundaryHeightsInTheTargetPreview)
{
    auto doc = Region ();
    ASSERT_EQ (doc.landscape->objects[0].surface->elevation, lg::RegionElevationMode::Boundary);
    for (auto& v : doc.entities[0].vertices)
        v.p.z = 2 + v.p.x * 0.1;
    ++doc.revision;
    lg::LandscapeResult landscape;
    ASSERT_TRUE (lg::EvaluateLandscape (doc, landscape)) << landscape.error;
    lg::LandscapePreviewMesh target;
    std::string error;
    ASSERT_TRUE (lg::BuildRegionSurfaceMesh (doc, doc.landscape->objects[0], landscape.regions[0], target, error))
        << error;
    for (const auto& p : target.vertices)
        EXPECT_NEAR (p.z, 2 + p.x * 0.1, 1e-9);
    namespace el = geomsrv::archviz::editlayers;
    el::Feedback feedback;
    feedback.landscape = &landscape;
    geomsrv::archviz::overlaylayers::Layer layer;
    el::AddLandscape (doc, feedback, {}, layer);
    ASSERT_EQ (layer.meshes.size (), 1u);
    for (size_t i = 0; i < layer.meshes[0].points.size (); i += 3)
        EXPECT_NEAR (layer.meshes[0].points[i + 2], 2 + layer.meshes[0].points[i] * 0.1, 1e-9);
}
