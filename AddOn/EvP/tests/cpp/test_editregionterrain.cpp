#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditSession.hpp"
#include "Geometry/Mesh.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "virtual_editor_debug_replay.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

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

TEST (EditRegionTerrain, AuthoredBoundaryElevationsGradeTheSurfaceAndFollowVertexEditsWithUndo)
{
    Fixture fixture;
    std::string error;
    auto surface = *fixture.doc.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Boundary;
    ASSERT_TRUE (lg::SetRegionSurface (fixture.doc, fixture.object, surface, {}, error)) << error;
    const auto entity = fixture.doc.entities[0].id;
    // A non-planar quad: each point's height must survive; a single separate
    // surface.first.z cannot describe this target.
    for (size_t i = 0; i < 4; ++i)
        fixture.doc.entities[0].vertices[i].p.z = double (i + 1);
    ++fixture.doc.revision;
    edit::Session session (fixture.doc);
    const auto run = [&] (const edit::Document& doc) {
        auto copy = fixture;
        copy.doc = doc;
        lg::RegionTerrainResult result;
        EXPECT_TRUE (lg::EvaluateRegionTerrain (doc, copy.Capture (), result, error)) << error;
        return result;
    };
    auto result = run (session.Doc ());
    ASSERT_FALSE (result.evaluation.terrain.points.empty ());
    for (const auto& vertex : session.Doc ().entities[0].vertices)
        EXPECT_NEAR (Height (result, vertex.p.x, vertex.p.y), vertex.p.z, 1e-6);
    const auto vertex = session.Doc ().entities[0].vertices[0];
    ASSERT_TRUE (session.Apply ([&] (auto& doc, auto& e) {
        return edit::MoveVertex (doc, entity, vertex.id, { vertex.p.x, vertex.p.y, 5 }, e);
    })) << session.Message ();
    result = run (session.Doc ());
    EXPECT_NEAR (Height (result, vertex.p.x, vertex.p.y), 5, 1e-6);
    ASSERT_TRUE (session.Undo ());
    result = run (session.Doc ());
    EXPECT_NEAR (Height (result, vertex.p.x, vertex.p.y), 1, 1e-6);
    std::string json;
    ASSERT_TRUE (lg::WriteLandscape (session.Doc (), lg::LandscapeStorage::Virtual, json, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (lg::ReadLandscape (json, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, session.Doc ().landscape);
}

TEST (EditRegionTerrain, CurvedBoundaryUsesTheActualSampledCurveHeights)
{
    Fixture fixture;
    std::string error;
    fixture.doc.landscape->sources[0].arcAngles.assign (4, 1.5707963267948966);
    for (size_t i = 0; i < 4; ++i)
        fixture.doc.entities[0].vertices[i].p.z = 1 + double (i) * 0.1;
    auto surface = *fixture.doc.landscape->objects[0].surface;
    surface.elevation = lg::RegionElevationMode::Boundary;
    ASSERT_TRUE (lg::SetRegionSurface (fixture.doc, fixture.object, surface, {}, error)) << error;
    std::vector<edit::Vec3> boundary;
    bool closed = false;
    ASSERT_TRUE (lg::LandscapeSourcePreview (fixture.doc, fixture.doc.landscape->sources[0], boundary, closed, error))
        << error;
    ASSERT_TRUE (closed);
    ASSERT_GT (boundary.size (), 4u);
    lg::RegionTerrainResult result;
    ASSERT_TRUE (lg::EvaluateRegionTerrain (fixture.doc, fixture.Capture (), result, error)) << error;
    for (size_t i = 0; i < boundary.size (); i += std::max (size_t (1), boundary.size () / 8))
        EXPECT_NEAR (Height (result, boundary[i].x, boundary[i].y), boundary[i].z, 1e-6);
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

TEST (EditRegionTerrain, MixedPreparedSceneRefusesAtomicallyUntilSplitterEndpointGapIsResolved)
{
    Fixture fixture;
    const auto path = edit::AddPath (fixture.doc, { { 2, 2, 0.5 }, { 2, 18, 0.5 } }, 1);
    lg::LandscapeSource source;
    source.pathId = path;
    source.role = lg::SourceRole::PathCenterline;
    std::string error;
    edit::Id id = 0;
    ASSERT_TRUE (lg::AddLandscapeSource (fixture.doc, source, id, error));
    lg::TerrainDesignOptions options;
    options.source = id;
    options.object = fixture.object;
    options.operation = fixture.doc.landscape->objects[0].footprintOperation;
    options.outline = true;
    lg::TerrainDesignPublisher publisher;
    lg::TerrainDesignJob job;
    ASSERT_TRUE (publisher.Prepare (fixture.doc, fixture.Capture (), options, {}, job, error));
    auto result = lg::RunLandscapeTerrainDesign (job);
    ASSERT_FALSE (result.built);
    EXPECT_NE (result.error.find ("RegionPathComposition (#82)"), std::string::npos) << result.error;
    EXPECT_TRUE (result.result.build.points.empty ()) << "Never publish a region-only or path-only partial result.";
    ASSERT_TRUE (result.regions);
    EXPECT_EQ (result.regions->input->points[2], 0);
    EXPECT_NEAR (Height (*result.regions, 10, 10), 2, 1e-7);
    EXPECT_TRUE (lg::RunTerrainDesign (job).built) << "The unchanged path lane still works on the original capture.";
    ASSERT_TRUE (publisher.Publish (fixture.doc, std::move (result), error));
    auto changed = fixture.doc;
    ++changed.revision;
    EXPECT_EQ (publisher.Current (changed), nullptr);
}

TEST (EditRegionTerrain, CapturedNativeTerrainWithIndependentBoundaryBuildsBothRegionAndPath)
{
    const auto file = std::filesystem::path (__FILE__).parent_path () / "fixtures/region-boundary-20261029.debug.json";
    std::ifstream input (file, std::ios::binary);
    ASSERT_TRUE (input);
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    auto scene = debugreplay::Read (parsed.value);
    const auto originalPoints = scene.baseline.vertices;
    const auto originalTriangles = scene.baseline.triangles;
    const auto originalLandscape = scene.doc.landscape;
    for (bool includePath : { false, true }) {
        SCOPED_TRACE (includePath);
        scene.options.source = includePath ? 8 : 0;
        const auto result = debugreplay::Run (scene);
        ASSERT_TRUE (result.built) << result.error;
        EXPECT_TRUE (result.error.empty ());
        ASSERT_TRUE (result.regions);
        ASSERT_TRUE (result.regions->input->native);
        EXPECT_EQ (result.regions->input->native->points, originalPoints);
        EXPECT_EQ (result.regions->input->native->triangles, originalTriangles);
        EXPECT_EQ (result.regions->input->points.size (), 12u) << "The native solid compacts to four upward vertices.";
        EXPECT_GT (result.result.build.site.fill, 0);
        EXPECT_FALSE (result.result.build.triangles.empty ());
        lg::ProvenanceDiagnostic diagnostic;
        std::string error;
        // The outline lane is preview-only, not attributed final path faces.
        // Validate its actual retained stage input and the complete area input.
        ASSERT_TRUE (result.capture);
        EXPECT_TRUE (lg::ValidateProvenance (result.capture->provenance, diagnostic, error)) << error;
        EXPECT_EQ (diagnostic, lg::ProvenanceDiagnostic::None);
        EXPECT_TRUE (lg::ValidateProvenance (result.regions->evaluation.terrain.inputProvenance, diagnostic, error))
            << error;
        EXPECT_EQ (diagnostic, lg::ProvenanceDiagnostic::None);
        const std::array<double, 6> query { 6.5, 6, 10.5921960923966, 14.2999855870725, 0.5, 0.5 };
        std::vector<lg::TerrainSample> samples;
        lg::TerrainFieldSummary summary;
        ASSERT_TRUE (lg::SampleTerrain ({ result.result.build.points, result.result.build.triangles }, query, 0.25,
                                        samples, summary, error))
            << error;
        ASSERT_EQ (samples.size (), 3u);
        for (const auto& sample : samples)
            EXPECT_TRUE (sample.onSurface);
        EXPECT_NEAR (samples[0].height, 0, 1e-6) << "The region actually grades the baseline.";
        EXPECT_NEAR (samples[1].height, includePath ? 0.07 : -0.2, 1e-6);
        EXPECT_NEAR (samples[2].height, -0.2, 1e-12) << "Remote terrain stays at its captured level.";
        const auto repeated = debugreplay::Run (scene);
        ASSERT_TRUE (repeated.built) << repeated.error;
        EXPECT_EQ (repeated.result.build.points, result.result.build.points);
        EXPECT_EQ (repeated.result.build.triangles, result.result.build.triangles);
    }
    EXPECT_EQ (scene.doc.landscape, originalLandscape);
    EXPECT_EQ (scene.baseline.vertices, originalPoints);
    EXPECT_EQ (scene.baseline.triangles, originalTriangles);
}

TEST (EditRegionTerrain, CoincidentRegionResplittingRefusesAtomicallyUntilEndpointGapIsResolved)
{
    Fixture fixture;
    std::string error;
    const auto line = edit::AddPolyline (fixture.doc, { { 5, 5, 0 }, { 15, 5, 0 }, { 15, 15, 0 }, { 5, 15, 0 } }, true);
    ASSERT_TRUE (lg::SetLandscapeSourceRole (fixture.doc, line, lg::SourceRole::RegionBoundary, error)) << error;
    const auto second = fixture.doc.landscape->objects.back ().id;
    auto surface = *fixture.doc.landscape->objects[0].surface;
    surface.first.z = 3;
    ASSERT_TRUE (lg::SetRegionSurface (fixture.doc, second, surface, {}, error)) << error;
    ASSERT_TRUE (lg::SetRegionTerrain (fixture.doc, second, fixture.doc.landscape->objects[0].terrain, error)) << error;
    const auto before = fixture.doc.landscape;
    const auto capture = fixture.Capture ();
    const auto originalPoints = capture->points;
    const auto originalTriangles = capture->triangles;
    lg::RegionTerrainResult result;
    // A separate #82 case: repeating the already split footprint/transition
    // rings can still collapse a constraint. Ownership attribution is repaired,
    // but this test must not be mistaken for multi-region success acceptance.
    ASSERT_FALSE (lg::EvaluateRegionTerrain (fixture.doc, capture, result, error));
    EXPECT_NE (error.find ("TriangulationDegenerate"), std::string::npos) << error;
    EXPECT_TRUE (result.evaluation.terrain.points.empty ());
    EXPECT_TRUE (result.preparation.empty ()) << "No partial region stage escapes a failed preparation.";
    EXPECT_EQ (capture->points, originalPoints);
    EXPECT_EQ (capture->triangles, originalTriangles);
    EXPECT_EQ (fixture.doc.landscape, before);
}

TEST (EditRegionTerrain, CapturedRectangleHasCleanContoursAndStillBuildsAfterPathAndTransitionEdits)
{
    // Geometry/profile values from the user's 10:17:45 and 10:19:30 UTC dumps.
    // The native Mesh and options are identical to the earlier captured fixture;
    // rebuild the authored geometry through public APIs, not its cached outcome.
    const auto file = std::filesystem::path (__FILE__).parent_path () / "fixtures/region-boundary-20261029.debug.json";
    std::ifstream input (file, std::ios::binary);
    ASSERT_TRUE (input);
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    for (bool changed : { false, true }) {
        SCOPED_TRACE (changed);
        auto scene = debugreplay::Read (parsed.value);
        scene.doc = {};
        std::string error;
        ASSERT_TRUE (lg::StartLandscape (scene.doc, { 1 }, error)) << error;
        const auto path = edit::AddPath (scene.doc,
                                         { changed ? edit::Vec3 { 2.3958113406403756, 8.920939441050294, 0 }
                                                   : edit::Vec3 { 5.276001592382269, 11.225354576484476, 0 },
                                           { 8.900047123605457, 13.317698239491644, 0 },
                                           changed ? edit::Vec3 { 13.364166121962585, 11.203991004939164, 0 }
                                                   : edit::Vec3 { 14.33789767803898, 11.524028649243991, 0 } },
                                         1.5);
        ASSERT_TRUE (edit::EditPath (
            scene.doc, path,
            [changed] (edit::Path& p, std::string&) {
                p.profile.front ().z = changed ? 0.11000000000000001 : 0.04;
                p.profile.back ().z = changed ? 0.19 : 0.04;
                p.profile.back ().station = changed ? 12.790138717591025 : 9.910721665007785;
                return true;
            },
            error))
            << error;
        lg::LandscapeSource source;
        source.pathId = path;
        source.role = lg::SourceRole::PathCenterline;
        lg::Id pathSource = 0, operation = 0, owner = 0, terrain = 0;
        ASSERT_TRUE (lg::AddLandscapeSource (scene.doc, source, pathSource, error)) << error;
        lg::LandscapeOperation stroke;
        stroke.kind = lg::OperationKind::Stroke;
        stroke.inputs = { pathSource };
        stroke.width = 1.5;
        ASSERT_TRUE (lg::AddLandscapeOperation (scene.doc, stroke, operation, error)) << error;
        lg::HardscapeObject object;
        object.pathId = path;
        object.kind = lg::ObjectKind::Path;
        object.footprintOperation = operation;
        ASSERT_TRUE (lg::AddHardscapeObject (scene.doc, object, owner, error)) << error;
        ASSERT_TRUE (lg::SetLandscapeTerrain (scene.doc, scene.baseline.guid, terrain, error)) << error;
        const auto rectangle = edit::AddPolyline (scene.doc,
                                                  { { 6.316217305653341, 7.874297764609372, 0 },
                                                    { 6.316217305653341, 4.524116466780111, 0 },
                                                    { 10.728651210111392, 4.524116466780111, 0 },
                                                    { 10.728651210111392, 7.874297764609372, 0 } },
                                                  true);
        ASSERT_TRUE (lg::SetLandscapeSourceRole (scene.doc, rectangle, lg::SourceRole::RegionBoundary, error)) << error;
        const auto region = scene.doc.landscape->objects.back ().id;
        lg::RegionSurface surface;
        surface.elevation = lg::RegionElevationMode::Constant;
        ASSERT_TRUE (lg::SetRegionSurface (scene.doc, region, surface, {}, error)) << error;
        lg::RegionTerrain settings;
        settings.effect = lg::RegionTerrainEffect::Grade;
        settings.transition = changed ? lg::RegionTerrainTransition::Daylight : lg::RegionTerrainTransition::Falloff;
        ASSERT_TRUE (lg::SetRegionTerrain (scene.doc, region, settings, error)) << error;
        scene.options.source = pathSource;
        scene.options.operation = operation;
        scene.options.object = owner;
        const auto result = debugreplay::Run (scene);
        ASSERT_TRUE (result.built) << result.error;
        edit::PavedArea mesh;
        const auto& build = result.result.build;
        for (size_t i = 0; i < build.points.size (); i += 3)
            mesh.vertices.push_back ({ build.points[i], build.points[i + 1], build.points[i + 2] });
        mesh.triangles = build.triangles;
        const auto contours = edit::SurfaceContours (mesh, 0.1);
        EXPECT_FALSE (contours.empty ()) << "Real graded contour levels remain visible.";
        for (const auto& contour : contours)
            EXPECT_GT (contour.z, -0.2 + 1e-12) << "A flat baseline must not draw scattered triangle-edge contours.";
        const std::array<double, 6> query { 8, 6, 0.5, 0.5, 8.900047123605457, 13.317698239491644 };
        std::vector<lg::TerrainSample> samples;
        lg::TerrainFieldSummary summary;
        ASSERT_TRUE (lg::SampleTerrain ({ build.points, build.triangles }, query, 0.25, samples, summary, error))
            << error;
        ASSERT_EQ (samples.size (), 3u);
        for (const auto& sample : samples)
            EXPECT_TRUE (sample.onSurface);
        EXPECT_NEAR (samples[0].height, 0, 1e-6);
        EXPECT_NEAR (samples[1].height, -0.2, 1e-10);
        EXPECT_GT (samples[2].height, changed ? 0.1 : 0.039);
    }
}

TEST (EditRegionTerrain, RegionOnlyHudStartsFromHeldTerrainAndRebuildsAfterUndo)
{
    namespace eh = geomsrv::archviz::edithud;
    const auto await = [] (eh::Session& session) {
        const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (10);
        while (std::chrono::steady_clock::now () < end) {
            eh::PollTerrainDesign (session);
            if (const auto* current = eh::CurrentTerrainDesign (session))
                return current->built;
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        return false;
    };
    Fixture fixture;
    const auto captured = fixture.Capture ();
    auto mesh = std::make_shared<geomsrv::Mesh> ();
    mesh->guid = captured->sourceTerrainGuid;
    mesh->vertices = captured->points;
    mesh->triangles = captured->triangles;
    eh::Session session (fixture.doc);
    ASSERT_TRUE (eh::SetTerrainBaseline (session, { mesh, false, {} }, 0));
    ASSERT_TRUE (await (session)) << eh::GetTerrainDesignStatus (session).error;
    EXPECT_TRUE (eh::GetTerrainDesignStatus (session).built);
    EXPECT_GT (eh::GetTerrainDesignStatus (session).fill, 199);
    auto target = *fixture.doc.landscape->objects[0].surface;
    target.first.z = 1;
    ASSERT_TRUE (session.edits.Apply (
        [&] (auto& doc, auto& error) { return lg::SetRegionSurface (doc, fixture.object, target, {}, error); }));
    EXPECT_EQ (eh::CurrentTerrainDesign (session), nullptr);
    ASSERT_TRUE (await (session)) << eh::GetTerrainDesignStatus (session).error;
    const auto lower = eh::GetTerrainDesignStatus (session).fill;
    ASSERT_TRUE (session.edits.Undo ());
    ASSERT_TRUE (await (session)) << eh::GetTerrainDesignStatus (session).error;
    EXPECT_GT (eh::GetTerrainDesignStatus (session).fill, lower * 1.9);
    EXPECT_EQ (mesh->vertices, captured->points);
    ASSERT_TRUE (session.edits.Apply ([] (auto& doc, auto& error) {
        const auto path = edit::AddPath (doc, { { 2, 2, 0.5 }, { 2, 18, 0.5 } }, 1);
        lg::LandscapeSource source;
        source.pathId = path;
        source.role = lg::SourceRole::PathCenterline;
        edit::Id id = 0;
        return lg::AddLandscapeSource (doc, source, id, error);
    }));
    ASSERT_TRUE (eh::SetTerrainPaths (session, false));
    ASSERT_TRUE (await (session)) << eh::GetTerrainDesignStatus (session).error;
    EXPECT_EQ (session.terrainPath, 0u);
    EXPECT_FALSE (session.terrainPaths);
    EXPECT_EQ (session.edits.Doc ().paths.size (), 1u) << "Region-only mode does not delete authored paths.";
    EXPECT_GT (eh::GetTerrainDesignStatus (session).fill, 199);
}
