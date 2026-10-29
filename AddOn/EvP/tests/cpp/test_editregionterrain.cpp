#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditSession.hpp"
#include "Geometry/Mesh.hpp"
#include <gtest/gtest.h>

namespace lg = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace {
struct Fixture {
    edit::Document doc;
    edit::Id object = 0;
    Fixture ()
    {
        std::string error;
        EXPECT_TRUE (lg::StartLandscape (doc, { 1 }, error));
        const auto line = edit::AddPolyline (doc, { { 5, 5, 0 }, { 15, 5, 0 }, { 15, 15, 0 }, { 5, 15, 0 } }, true);
        EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error)) << error;
        object = doc.landscape->objects[0].id;
        lg::RegionSurface surface;
        surface.elevation = lg::RegionElevationMode::Constant;
        surface.first.z = 2;
        EXPECT_TRUE (lg::SetRegionSurface (doc, object, surface, {}, error));
        lg::RegionTerrain settings;
        settings.effect = lg::RegionTerrainEffect::Grade;
        EXPECT_TRUE (lg::SetRegionTerrain (doc, object, settings, error));
        edit::Id terrain = 0;
        EXPECT_TRUE (lg::SetLandscapeTerrain (doc, "00112233-4455-6677-8899-AABBCCDDEEFF", terrain, error)) << error;
    }
    std::shared_ptr<lg::TerrainCapture> Capture ()
    {
        geomsrv::Mesh mesh;
        mesh.guid = doc.landscape->sourceTerrainGuid;
        mesh.vertices = { 0, 0, 0, 20, 0, 0, 20, 20, 0, 0, 20, 0 };
        mesh.triangles = { 0, 1, 2, 0, 2, 3 };
        auto capture = std::make_shared<lg::TerrainCapture> ();
        std::string error;
        EXPECT_TRUE (lg::CaptureLandscapeTerrain (doc, mesh, { object, 2, 1 }, *capture, error)) << error;
        return capture;
    }
};
double Height (const lg::RegionTerrainResult& result, double x, double y)
{
    std::vector<lg::TerrainSample> samples;
    lg::TerrainFieldSummary summary;
    const std::array<double, 2> query { x, y };
    std::string error;
    const auto& mesh = result.evaluation.terrain;
    EXPECT_TRUE (lg::SampleTerrain ({ mesh.points, mesh.triangles }, query, 0.25, samples, summary, error)) << error;
    EXPECT_EQ (samples.size (), 1u);
    return samples.empty () ? -999 : samples[0].height;
}
} // namespace

TEST (EditRegionTerrain, TwoTriangleBaselineGetsAFlatTerraceAndLocalTransition)
{
    Fixture fixture;
    const auto capture = fixture.Capture ();
    lg::RegionTerrainResult result;
    std::string error;
    ASSERT_TRUE (lg::EvaluateRegionTerrain (fixture.doc, capture, result, error)) << error;
    EXPECT_NEAR (Height (result, 10, 10), 2, 1e-8);
    EXPECT_NEAR (Height (result, 7, 8), 2, 1e-8);
    EXPECT_NEAR (Height (result, 1, 1), 0, 1e-12);
    EXPECT_NEAR (Height (result, 19, 10), 0, 1e-12);
    ASSERT_TRUE (result.evaluation.terrain.volumes);
    EXPECT_GT (result.evaluation.terrain.volumes->site.fill, 199);
    EXPECT_NEAR (result.evaluation.terrain.volumes->site.cut, 0, 1e-10);
    EXPECT_GT (result.preparation.size (), 1u);
    lg::ProvenanceDiagnostic diagnostic;
    EXPECT_TRUE (lg::ValidateProvenance (result.evaluation.terrain.inputProvenance, diagnostic, error)) << error;
    EXPECT_EQ (capture->triangles.size (), 6u) << "The captured baseline is immutable.";
}

TEST (EditRegionTerrain, AddNeverCutsAndSubtractNeverFills)
{
    for (auto effect : { lg::RegionTerrainEffect::Add, lg::RegionTerrainEffect::Subtract })
        for (double level : { -2.0, 2.0 }) {
            Fixture fixture;
            std::string error;
            auto surface = *fixture.doc.landscape->objects[0].surface;
            surface.first.z = level;
            ASSERT_TRUE (lg::SetRegionSurface (fixture.doc, fixture.object, surface, {}, error));
            auto settings = fixture.doc.landscape->objects[0].terrain;
            settings.effect = effect;
            ASSERT_TRUE (lg::SetRegionTerrain (fixture.doc, fixture.object, settings, error));
            lg::RegionTerrainResult result;
            ASSERT_TRUE (lg::EvaluateRegionTerrain (fixture.doc, fixture.Capture (), result, error)) << error;
            const double expected =
                effect == lg::RegionTerrainEffect::Add ? std::max (0.0, level) : std::min (0.0, level);
            EXPECT_NEAR (Height (result, 10, 10), expected, 1e-8);
            const auto& volumes = *result.evaluation.terrain.volumes;
            EXPECT_NEAR (effect == lg::RegionTerrainEffect::Add ? volumes.site.cut : volumes.site.fill, 0, 1e-10);
        }
}

TEST (EditRegionTerrain, TwoPointAndContourPlanesActuallyGradeTheTerrain)
{
    for (auto mode : { lg::RegionSlopeMode::TwoPoints, lg::RegionSlopeMode::ContourLine }) {
        Fixture fixture;
        auto surface = *fixture.doc.landscape->objects[0].surface;
        surface.slope = mode;
        surface.first = { 5, 5, 2 };
        surface.second = { 15, 5, 3 };
        surface.gradePercent = 5;
        surface.fallsTo = lg::RegionFallSide::Left;
        std::string error;
        ASSERT_TRUE (lg::SetRegionSurface (fixture.doc, fixture.object, surface, {}, error));
        lg::RegionTerrainResult result;
        ASSERT_TRUE (lg::EvaluateRegionTerrain (fixture.doc, fixture.Capture (), result, error)) << error;
        EXPECT_NEAR (Height (result, 10, 10), *lg::RegionSurfaceElevation (surface, { 10, 10, 0 }), 1e-7);
        EXPECT_NEAR (Height (result, 8, 12), *lg::RegionSurfaceElevation (surface, { 8, 12, 0 }), 1e-7);
    }
}

TEST (EditRegionTerrain, EffectAndTransitionPersistWithUndoAndRejectExistingTargets)
{
    Fixture fixture;
    edit::Session session (fixture.doc);
    auto settings = fixture.doc.landscape->objects[0].terrain;
    settings.effect = lg::RegionTerrainEffect::Subtract;
    settings.transition = lg::RegionTerrainTransition::Daylight;
    ASSERT_TRUE (session.Apply (
        [&] (auto& doc, auto& error) { return lg::SetRegionTerrain (doc, fixture.object, settings, error); }));
    std::string json, error;
    ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error));
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, session.Doc ().landscape);
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().landscape, fixture.doc.landscape);
    ASSERT_TRUE (session.Redo ());
    auto surface = *loaded.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Existing;
    EXPECT_FALSE (lg::SetRegionSurface (loaded, fixture.object, surface, {}, error));
    EXPECT_NE (error.find ("Constant"), std::string::npos);
}

TEST (EditRegionTerrain, AuthoredPropertyAndProtectedMaskBoundariesKeepTheirBaselineFaces)
{
    for (bool protection : { false, true }) {
        Fixture fixture;
        std::string error;
        lg::LandscapeSource source;
        source.entityId = edit::AddPolyline (
            fixture.doc,
            protection ? std::vector<edit::Vec3> { { 8, 8, 0 }, { 12, 8, 0 }, { 12, 12, 0 }, { 8, 12, 0 } }
                       : std::vector<edit::Vec3> { { 0, 0, 0 }, { 12, 0, 0 }, { 12, 20, 0 }, { 0, 20, 0 } },
            true);
        edit::Id id = 0;
        ASSERT_TRUE (lg::AddLandscapeSource (fixture.doc, source, id, error));
        if (protection) {
            lg::LandscapeOperation mask;
            mask.kind = lg::OperationKind::ProtectedZone;
            mask.inputs = { fixture.doc.landscape->objects[0].footprintOperation, id };
            edit::Id operation = 0;
            ASSERT_TRUE (lg::AddLandscapeOperation (fixture.doc, mask, operation, error));
            fixture.doc.landscape->objects[0].footprintOperation = operation;
        }
        else
            ASSERT_TRUE (lg::SetLandscapePropertyLine (fixture.doc, id, error));
        lg::RegionTerrainResult result;
        ASSERT_TRUE (lg::EvaluateRegionTerrain (fixture.doc, fixture.Capture (), result, error)) << error;
        EXPECT_NEAR (Height (result, protection ? 10 : 13, 10), 0, 1e-12);
        EXPECT_NEAR (Height (result, 7, 7), 2, 1e-7);
    }
}

TEST (EditRegionTerrain, DaylightUsesTheAuthoredSideGrade)
{
    Fixture fixture;
    std::string error;
    auto settings = fixture.doc.landscape->objects[0].terrain;
    settings.transition = lg::RegionTerrainTransition::Daylight;
    settings.reach = 3;
    settings.cutPercent = settings.fillPercent = 100;
    ASSERT_TRUE (lg::SetRegionTerrain (fixture.doc, fixture.object, settings, error));
    lg::RegionTerrainResult result;
    ASSERT_TRUE (lg::EvaluateRegionTerrain (fixture.doc, fixture.Capture (), result, error)) << error;
    EXPECT_NEAR (Height (result, 10, 10), 2, 1e-7);
    EXPECT_NEAR (Height (result, 16, 10), 1, 1e-6);
    EXPECT_NEAR (Height (result, 19, 10), 0, 1e-12);
}
