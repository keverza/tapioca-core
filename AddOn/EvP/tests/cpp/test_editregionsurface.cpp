#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditSession.hpp"
#include "ArchViz/EditLayers.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <cmath>
#include <numbers>

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

TEST (EditRegionSurface, LegacyCutPercentLoadsAndAmbiguousOrMalformedTypedCutRefusesAtomically)
{
    auto doc = Region ();
    const auto object = doc.landscape->objects[0].id;
    auto terrain = doc.landscape->objects[0].terrain;
    terrain.effect = lg::RegionTerrainEffect::Grade;
    std::string json, error;
    ASSERT_TRUE (lg::SetRegionTerrain (doc, object, terrain, error)) << error;
    ASSERT_TRUE (lg::WriteLandscape (doc, lg::LandscapeStorage::Virtual, json, error)) << error;
    const auto start = json.find ("\"cut\":");
    ASSERT_NE (start, std::string::npos);
    const auto end = json.find ('}', start);
    ASSERT_NE (end, std::string::npos);
    const auto replace = [&] (const std::string& field) {
        auto copy = json;
        copy.replace (start, end - start + 1, field);
        return copy;
    };
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (replace ("\"cutPercent\":50"), loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, doc.landscape);
    for (const auto& cut : { "\"cutPercent\":50,\"cut\":{\"unit\":2,\"a\":50,\"b\":0}",
                             "\"cut\":{\"unit\":0,\"a\":50,\"b\":0}", "\"cut\":{\"unit\":5,\"a\":50,\"b\":0}",
                             "\"cut\":{\"unit\":2,\"a\":50,\"b\":1}", "\"cut\":{\"unit\":2,\"a\":-1,\"b\":0}",
                             "\"cut\":{\"unit\":2,\"a\":1001,\"b\":0}", "\"cut\":{\"unit\":3,\"a\":0,\"b\":0}",
                             "\"cut\":{\"unit\":2,\"a\":50}", "\"cut\":{\"unit\":2,\"a\":50,\"b\":0,\"extra\":0}" }) {
        const auto before = loaded;
        EXPECT_FALSE (lg::ReadLandscape (replace (cut), loaded, error)) << cut;
        EXPECT_EQ (loaded.landscape, before.landscape);
        EXPECT_EQ (loaded.entities, before.entities);
    }
}

TEST (EditRegionSurface, FillCodecReadsEachLegacyFaceIndependentlyAndRejectsMissingOrAmbiguousFill)
{
    auto doc = Region ();
    const auto object = doc.landscape->objects[0].id;
    auto terrain = doc.landscape->objects[0].terrain;
    terrain.effect = lg::RegionTerrainEffect::Grade;
    std::string json, error;
    ASSERT_TRUE (lg::SetRegionTerrain (doc, object, terrain, error)) << error;
    ASSERT_TRUE (lg::WriteLandscape (doc, lg::LandscapeStorage::Virtual, json, error)) << error;
    const auto replace = [] (std::string text, const char* face, const std::string& field) {
        auto start = text.find (std::string ("\"") + face + "\":");
        EXPECT_NE (start, std::string::npos);
        const auto end = text.find ('}', start);
        EXPECT_NE (end, std::string::npos);
        if (start != std::string::npos && end != std::string::npos) {
            size_t length = end - start + 1;
            if (field.empty ()) {
                if (end + 1 < text.size () && text[end + 1] == ',')
                    ++length;
                else if (start > 0 && text[start - 1] == ',') {
                    --start;
                    ++length;
                }
            }
            text.replace (start, length, field);
        }
        return text;
    };
    for (bool oldCut : { false, true })
        for (bool oldFill : { false, true }) {
            auto input = oldCut ? replace (json, "cut", "\"cutPercent\":50") : json;
            if (oldFill)
                input = replace (input, "fill", "\"fillPercent\":33");
            edit::Document loaded;
            ASSERT_TRUE (lg::ReadLandscape (input, loaded, error)) << error;
            EXPECT_EQ (loaded.landscape, doc.landscape);
            EXPECT_EQ (loaded.entities, doc.entities);
        }
    for (const auto& fill :
         { "", "\"fillPercent\":33,\"fill\":{\"unit\":2,\"a\":33,\"b\":0}", "\"fill\":{\"unit\":0,\"a\":33,\"b\":0}",
           "\"fill\":{\"unit\":5,\"a\":33,\"b\":0}", "\"fill\":{\"unit\":2,\"a\":33,\"b\":1}",
           "\"fill\":{\"unit\":2,\"a\":0,\"b\":0}", "\"fill\":{\"unit\":2,\"a\":1001,\"b\":0}",
           "\"fill\":{\"unit\":3,\"a\":0,\"b\":0}", "\"fill\":{\"unit\":2,\"a\":33}",
           "\"fill\":{\"unit\":2,\"a\":33,\"b\":0,\"extra\":0}" }) {
        auto loaded = doc;
        EXPECT_FALSE (lg::ReadLandscape (replace (json, "fill", fill), loaded, error)) << fill;
        if (std::string (fill).empty ())
            EXPECT_NE (error.find ("exactly one of fill"), std::string::npos) << error;
        EXPECT_EQ (loaded.landscape, doc.landscape);
        EXPECT_EQ (loaded.entities, doc.entities);
    }
}

#ifdef TAPIOCA_GEOMETRY_RUST
TEST (EditRegionSurface, FillSlopeUnitsPreserveTheCutPairHistoryAndSurveyGeometry)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = Region ();
    for (auto& entity : doc.entities)
        for (auto& vertex : entity.vertices) {
            vertex.p.x += 1000000;
            vertex.p.y -= 2000000;
        }
    const auto object = doc.landscape->objects[0].id;
    auto initial = doc.landscape->objects[0].terrain;
    initial.cut = { Unit::RiseRun, 1.5, 3 };
    std::string error;
    ASSERT_TRUE (lg::SetRegionTerrain (doc, object, initial, error)) << error;
    for (const lg::TerrainSlope fill : { lg::TerrainSlope { Unit::Percent, 25, 0 },
                                         { Unit::Angle, std::atan (0.25), 0 },
                                         { Unit::RiseRun, 1.5, 6 },
                                         { Unit::Height, 2.5, 10 } }) {
        auto settings = initial;
        settings.fill = fill;
        edit::Session session (doc);
        ASSERT_TRUE (session.Apply ([object, settings] (auto& d, auto& e) {
            return lg::SetRegionTerrain (d, object, settings, e);
        })) << session.Message ();
        EXPECT_EQ (session.Doc ().landscape->objects[0].terrain.cut, initial.cut);
        EXPECT_EQ (session.Doc ().landscape->objects[0].terrain.fill, fill);
        EXPECT_EQ (session.Doc ().entities, doc.entities);
        std::string json;
        ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
        EXPECT_EQ (loaded.landscape, session.Doc ().landscape);
        EXPECT_EQ (session.UndoDepth (), 1u);
        ASSERT_TRUE (session.Undo ());
        EXPECT_EQ (session.Doc ().landscape, doc.landscape);
        ASSERT_TRUE (session.Redo ());
        EXPECT_EQ (session.Doc ().landscape->objects[0].terrain.fill, fill);
        EXPECT_EQ (session.Doc ().landscape->objects[0].terrain.cut, initial.cut);
    }
}

TEST (EditRegionSurface, FillPolicyRefusesInvalidAndVerticalValuesWithoutClampingEitherFace)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = Region ();
    const auto object = doc.landscape->objects[0].id;
    const auto before = doc.landscape;
    std::string error;
    for (const lg::TerrainSlope fill : { lg::TerrainSlope { Unit::Angle, std::numbers::pi / 2, 0 },
                                         { Unit::RiseRun, 1, 0 },
                                         { Unit::Height, 0, 0 },
                                         { Unit::Percent, std::numeric_limits<double>::infinity (), 0 },
                                         { Unit::Height, 1, std::numeric_limits<double>::quiet_NaN () },
                                         { Unit::Percent, 1000.000001, 0 },
                                         { Unit::RiseRun, 11, 1 },
                                         { Unit::Angle, -0.2, 0 } }) {
        auto settings = doc.landscape->objects[0].terrain;
        settings.fill = fill;
        EXPECT_FALSE (lg::SetRegionTerrain (doc, object, settings, error));
        EXPECT_EQ (doc.landscape, before);
    }
    for (auto unit : { Unit::Percent, Unit::Angle, Unit::RiseRun, Unit::Height }) {
        auto settings = doc.landscape->objects[0].terrain;
        ASSERT_TRUE (lg::ConvertTerrainSlope ({ Unit::Percent, 1000, 0 }, unit, 1, settings.fill, error)) << error;
        const auto authored = settings.fill;
        EXPECT_TRUE (lg::SetRegionTerrain (doc, object, settings, error)) << error;
        EXPECT_EQ (doc.landscape->objects[0].terrain.fill, authored);
        EXPECT_EQ (doc.landscape->objects[0].terrain.cut, before->objects[0].terrain.cut);
    }
}

TEST (EditRegionSurface, CutSlopeUnitsPreserveAuthoredPairsHistoryAndSurveyGeometry)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = Region ();
    for (auto& entity : doc.entities)
        for (auto& vertex : entity.vertices) {
            vertex.p.x += 1000000;
            vertex.p.y -= 2000000;
        }
    const auto object = doc.landscape->objects[0].id;
    std::string error;
    for (const lg::TerrainSlope cut : { lg::TerrainSlope { Unit::Percent, 50, 0 },
                                        { Unit::Angle, std::atan (0.5), 0 },
                                        { Unit::RiseRun, 1.5, 3 },
                                        { Unit::Height, 2.5, 5 } }) {
        auto terrain = doc.landscape->objects[0].terrain;
        terrain.cut = cut;
        terrain.effect = lg::RegionTerrainEffect::Grade;
        edit::Session session (doc);
        ASSERT_TRUE (session.Apply ([object, terrain] (auto& d, auto& e) {
            return lg::SetRegionTerrain (d, object, terrain, e);
        })) << session.Message ();
        EXPECT_EQ (session.Doc ().landscape->objects[0].terrain.cut, cut);
        EXPECT_EQ (session.Doc ().entities, doc.entities);
        std::string json;
        ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
        EXPECT_EQ (loaded.landscape, session.Doc ().landscape);
        EXPECT_EQ (session.UndoDepth (), 1u);
        ASSERT_TRUE (session.Undo ());
        EXPECT_EQ (session.Doc ().landscape, doc.landscape);
        ASSERT_TRUE (session.Redo ());
        EXPECT_EQ (session.Doc ().landscape->objects[0].terrain.cut, cut);
        lg::TerrainSlope percent;
        ASSERT_TRUE (lg::ConvertTerrainSlope (cut, Unit::Percent, 1, percent, error)) << error;
        EXPECT_NEAR (percent.a, 50, 1e-12);
    }
}

TEST (EditRegionSurface, CutSlopePolicyRefusesVerticalInvalidAndTooSteepWithoutClamping)
{
    using Unit = lg::TerrainSlope::Unit;
    auto doc = Region ();
    const auto object = doc.landscape->objects[0].id;
    const auto before = doc.landscape;
    std::string error;
    for (const lg::TerrainSlope cut : { lg::TerrainSlope { Unit::Angle, std::numbers::pi / 2, 0 },
                                        { Unit::RiseRun, 1, 0 },
                                        { Unit::Height, 0, 0 },
                                        { Unit::Percent, std::numeric_limits<double>::infinity (), 0 },
                                        { Unit::RiseRun, 11, 1 },
                                        { Unit::Angle, -0.2, 0 } }) {
        auto terrain = doc.landscape->objects[0].terrain;
        terrain.cut = cut;
        EXPECT_FALSE (lg::SetRegionTerrain (doc, object, terrain, error));
        EXPECT_EQ (doc.landscape, before);
    }
    for (auto unit : { Unit::Percent, Unit::Angle, Unit::RiseRun, Unit::Height }) {
        auto terrain = doc.landscape->objects[0].terrain;
        ASSERT_TRUE (lg::ConvertTerrainSlope ({ Unit::Percent, 1000, 0 }, unit, 1, terrain.cut, error)) << error;
        EXPECT_TRUE (lg::SetRegionTerrain (doc, object, terrain, error)) << error;
    }
}
#endif

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
