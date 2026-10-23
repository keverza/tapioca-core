#include "Edit/EditTerrainControls.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Geometry/Mesh.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace {
using namespace tapioca::geometry;
namespace edit = geomsrv::edit;
constexpr char kTerrain[] = "12345678-1234-4234-9234-123456789ABC";
constexpr char kGuide[] = "22345678-1234-4234-9234-123456789ABC";
const std::array<double, 12> kRegion { -10, -15, 0, 50, -15, 0, 50, 15, 0, -10, 15, 0 };
struct Fixture {
    edit::Document doc;
    geomsrv::Mesh mesh;
    Id source = 0, path = 0, operation = 0, owner = 0;
    Fixture ()
    {
        std::string error;
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error));
        path = edit::AddPath (doc, { { 0, 0, 10 }, { 40, 0, 12 } }, 4);
        EXPECT_TRUE (edit::EditPath (
            doc, path,
            [] (edit::Path& p, std::string&) {
                for (auto& s : p.sections) {
                    s.crossfall = 0;
                    s.cutSlope = s.fillSlope = 50;
                }
                return true;
            },
            error));
        LandscapeSource centre;
        centre.pathId = path;
        centre.role = SourceRole::PathCenterline;
        EXPECT_TRUE (AddLandscapeSource (doc, centre, source, error));
        LandscapeOperation stroke;
        stroke.kind = OperationKind::Stroke;
        stroke.inputs = { source };
        EXPECT_TRUE (AddLandscapeOperation (doc, stroke, operation, error));
        HardscapeObject object;
        object.kind = ObjectKind::Path;
        object.pathId = path;
        object.footprintOperation = operation;
        EXPECT_TRUE (AddHardscapeObject (doc, object, owner, error));
        Id terrain = 0;
        EXPECT_TRUE (SetLandscapeTerrain (doc, kTerrain, terrain, error));
        mesh.guid = kTerrain;
        for (uint32_t row = 0; row < 13; ++row)
            for (uint32_t col = 0; col < 17; ++col) {
                const double x = -20.5 + col * 5, y = -30.5 + row * 5;
                mesh.vertices.insert (mesh.vertices.end (), { x, y, 10 + .05 * x - .2 * y });
            }
        for (uint32_t row = 0; row < 12; ++row)
            for (uint32_t col = 0; col < 16; ++col) {
                const auto a = row * 17 + col;
                mesh.triangles.insert (mesh.triangles.end (), { a, a + 1, a + 18, a, a + 18, a + 17 });
            }
    }
    std::pair<Id, Id> Guide (std::vector<edit::Vec3> points, const char* native = "")
    {
        LandscapeSource guide;
        guide.entityId = edit::AddPolyline (doc, points, false);
        guide.archicadGuid = native;
        Id id = 0;
        std::string error;
        EXPECT_TRUE (AddLandscapeSource (doc, guide, id, error)) << error;
        return { id, guide.entityId };
    }
    TerrainCapture Capture () const
    {
        TerrainCapture captured;
        std::string error;
        EXPECT_TRUE (CaptureLandscapeTerrain (doc, mesh, { owner, 2, 1 }, captured, error)) << error;
        return captured;
    }
    TerrainDesignOptions Options () const
    {
        TerrainDesignOptions o;
        o.source = source;
        o.operation = operation;
        o.object = owner;
        o.designTable = 3;
        o.meshTable = 4;
        o.region = kRegion;
        o.maxStep = 5;
        o.tolerance = 1e-6;
        return o;
    }
};
bool Contains (const std::vector<uint64_t>& pool, IdentityRange range, Id id)
{
    return std::find (pool.begin () + range.offset, pool.begin () + range.offset + range.count, id) !=
           pool.begin () + range.offset + range.count;
}
} // namespace

TEST (EditTerrainControls, CrossingGuidesAndPointsKeepAuthoredIdsNativeBindingsAndCompleteMeshAncestry)
{
    Fixture f;
    const auto [line, entity] = f.Guide ({ { 5, -1, 10.25 }, { 35, 1, 11.75 } }, kGuide);
    const auto [other, otherEntity] = f.Guide ({ { 5, 1, 10.25 }, { 35, -1, 11.75 } });
    const auto [point, pointEntity] = f.Guide ({ { 20, 0, 11 }, { 20, -1, 11 } });
    const auto vertex = edit::Find (f.doc, pointEntity)->vertices.front ().id;
    std::string error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, line, 0, true, error)) << error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, other, 0, true, error)) << error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, point, vertex, true, error)) << error;
    const auto captured = f.Capture ();
    TerrainDesignResult result;
    ASSERT_TRUE (EvaluateTerrainDesign (f.doc, captured.Input (), f.Options (), result, error)) << error;
    EXPECT_EQ (result.meshProvenance.table.bindings, captured.provenance.table.bindings);
    EXPECT_TRUE (
        std::any_of (result.meshProvenance.table.bindings.begin (), result.meshProvenance.table.bindings.end (),
                     [=] (const auto& b) { return b.logicalId == line && b.logicalKind == LogicalKind::Source; }));
    bool shared = false;
    for (const auto& fixed : result.locked) {
        EXPECT_FALSE (result.vertexOperations.at (fixed.vertex));
        const auto& rows = result.meshProvenance.table.mappings;
        const auto row = std::find_if (rows.begin (), rows.end (), [&] (const auto& r) {
            return r.domain == IdentityDomain::Vertex && r.outputIndex == fixed.vertex;
        });
        ASSERT_NE (row, rows.end ());
        for (const auto& ref : fixed.supports) {
            EXPECT_TRUE (Contains (result.meshProvenance.table.sourceIds, row->sources, ref.source));
            if (ref.source == line) {
                EXPECT_EQ (ref.segment, edit::Find (f.doc, entity)->segments.front ().id);
                EXPECT_EQ (ref.vertices[0], edit::Find (f.doc, entity)->vertices.front ().id);
            }
            if (ref.source == point)
                EXPECT_EQ (ref.vertices[0], vertex);
        }
        shared |= fixed.supports.size () == 3;
    }
    EXPECT_TRUE (shared);
    EXPECT_EQ (result.build.context.cut, 0);
    EXPECT_EQ (result.build.context.fill, 0);
    TerrainDesignResult again;
    ASSERT_TRUE (EvaluateTerrainDesign (f.doc, captured.Input (), f.Options (), again, error)) << error;
    EXPECT_EQ (again, result);
}

TEST (EditTerrainControls, ExistingMeshVertexRetainsBothItsBaseAndFixedGuidePredecessors)
{
    Fixture f;
    const auto [source, entity] = f.Guide ({ { 9.5, -.5, 10.475 }, { 10, -.5, 10.5 } });
    const auto vertex = edit::Find (f.doc, entity)->vertices.front ().id;
    std::string error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, source, vertex, true, error));
    const auto capture = f.Capture ();
    TerrainDesignResult result;
    ASSERT_TRUE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error)) << error;
    ASSERT_EQ (result.locked.size (), 1U);
    const auto output = result.locked.front ().vertex;
    EXPECT_LT (output, f.mesh.vertices.size () / 3);
    const auto& rows = result.meshProvenance.table.mappings;
    const auto row = std::find_if (rows.begin (), rows.end (), [=] (const auto& r) {
        return r.domain == IdentityDomain::Vertex && r.outputIndex == output;
    });
    ASSERT_NE (row, rows.end ());
    EXPECT_TRUE (Contains (result.meshProvenance.table.sourceIds, row->sources, source));
    EXPECT_GE (row->predecessors.count, 2U);
    EXPECT_FALSE (result.vertexOperations.at (output));
}

TEST (EditTerrainControls, ConflictsResolveToAuthoredReferencesAndDisabledControlsReleaseTheBuild)
{
    Fixture f;
    const auto [source, entity] = f.Guide ({ { 20, 0, 12 }, { 21, 0, 12 } });
    const auto vertex = edit::Find (f.doc, entity)->vertices.front ().id;
    std::string error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, source, vertex, true, error));
    auto capture = f.Capture ();
    TerrainDesignResult result;
    result.stations = { 77 };
    const auto before = result;
    EXPECT_FALSE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error));
    EXPECT_EQ (result, before);
    EXPECT_EQ (error.find ("LockedConflict:"), 0U);
    EXPECT_NE (error.find ("at fixed source " + std::to_string (source)), std::string::npos);
    std::vector<TerrainDesignConflict> conflicts;
    ASSERT_TRUE (AssessTerrainDesign (f.doc, capture.Input (), f.Options (), conflicts, error)) << error;
    ASSERT_EQ (conflicts.size (), 1U);
    ASSERT_TRUE (conflicts[0].control);
    EXPECT_EQ (conflicts[0].control->source, source);
    EXPECT_EQ (conflicts[0].control->vertices[0], vertex);
    EXPECT_NEAR (conflicts[0].magnitude, -1, 1e-12);
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, source, vertex, false, error));
    capture = f.Capture ();
    ASSERT_TRUE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error)) << error;
    EXPECT_TRUE (result.locked.empty ());
}

TEST (EditTerrainControls, OrphanedCurvedAndUncapturedControlsRefuseRatherThanFlattenOrGuess)
{
    Fixture f;
    const auto [source, entity] = f.Guide ({ { 5, 0, 10.25 }, { 35, 0, 11.75 } });
    std::string error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, source, 0, true, error));
    auto capture = f.Capture ();
    const auto sourceRow =
        std::find_if (capture.provenance.table.sources.begin (), capture.provenance.table.sources.end (),
                      [=] (const auto& s) { return s.id == source; });
    capture.provenance.table.sources.erase (sourceRow);
    TerrainDesignResult result;
    EXPECT_FALSE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error));
    EXPECT_NE (error.find ("ProvenanceUnattributable"), std::string::npos);
    edit::Find (f.doc, entity)->vertices.front ().radius = 1;
    ++f.doc.revision;
    capture = f.Capture ();
    EXPECT_FALSE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error));
    EXPECT_EQ (error.find ("TerrainControlUnsupported:"), 0U);
    edit::Find (f.doc, entity)->vertices.front ().radius = 0;
    ASSERT_TRUE (DeleteLandscapeSource (f.doc, source, error));
    capture = f.Capture ();
    EXPECT_FALSE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error));
    EXPECT_EQ (error.find ("TerrainControlOrphaned:"), 0U);
}

TEST (EditTerrainControls, ConflictingControlSourcesAndCorruptDuplicateReferencesAreNotSilentlyMerged)
{
    Fixture f;
    const auto [first, firstEntity] = f.Guide ({ { 20, 0, 11 }, { 21, 0, 11 } });
    const auto [second, secondEntity] = f.Guide ({ { 20, 0, 12 }, { 21, 1, 12 } });
    const auto firstVertex = edit::Find (f.doc, firstEntity)->vertices.front ().id;
    const auto secondVertex = edit::Find (f.doc, secondEntity)->vertices.front ().id;
    std::string error;
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, first, firstVertex, true, error));
    ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, second, secondVertex, true, error));
    auto capture = f.Capture ();
    std::vector<TerrainDesignConflict> conflicts;
    ASSERT_TRUE (AssessTerrainDesign (f.doc, capture.Input (), f.Options (), conflicts, error)) << error;
    ASSERT_EQ (conflicts.size (), 1U);
    ASSERT_TRUE (conflicts[0].control);
    ASSERT_TRUE (conflicts[0].other);
    EXPECT_EQ (conflicts[0].control->source, first);
    EXPECT_EQ (conflicts[0].control->vertices[0], firstVertex);
    EXPECT_EQ (conflicts[0].other->source, second);
    EXPECT_EQ (conflicts[0].other->vertices[0], secondVertex);
    f.doc.landscape->terrainControls.push_back (f.doc.landscape->terrainControls.front ());
    terrain_detail::TerrainControls result;
    EXPECT_FALSE (terrain_detail::CompileTerrainControls (f.doc, capture.provenance, result, error));
    EXPECT_NE (error.find ("duplicate control"), std::string::npos);
}

TEST (EditTerrainControls, AllFixedConflictsResolveAuthoredVerticesWithoutPublishingGeometry)
{
    Fixture f;
    std::vector<Id> sources, vertices;
    std::string error;
    for (const auto point : { edit::Vec3 { 10, 0, 10.5 }, edit::Vec3 { 10, 0, 11.5 }, edit::Vec3 { 10, 0, 12.5 },
                              edit::Vec3 { 20, 0, 12 }, edit::Vec3 { 30, 0, 12.5 } }) {
        const auto [source, entity] = f.Guide ({ point, { point.x + 1, point.y, point.z } });
        const auto vertex = edit::Find (f.doc, entity)->vertices.front ().id;
        ASSERT_TRUE (SetLandscapeTerrainControl (f.doc, source, vertex, true, error)) << error;
        sources.push_back (source);
        vertices.push_back (vertex);
    }
    const auto capture = f.Capture ();
    std::vector<TerrainDesignConflict> conflicts;
    ASSERT_TRUE (AssessTerrainDesign (f.doc, capture.Input (), f.Options (), conflicts, error)) << error;
    ASSERT_EQ (conflicts.size (), 4U);
    for (size_t i = 0; i < conflicts.size (); ++i) {
        ASSERT_TRUE (conflicts[i].control);
        const auto input = i < 2 ? 0 : i + 1;
        EXPECT_EQ (conflicts[i].control->source, sources[input]);
        EXPECT_EQ (conflicts[i].control->vertices[0], vertices[input]);
        EXPECT_EQ (conflicts[i].control->segment, 0U);
        if (i < 2) {
            ASSERT_TRUE (conflicts[i].other);
            EXPECT_EQ (conflicts[i].other->source, sources[i + 1]);
            EXPECT_EQ (conflicts[i].other->vertices[0], vertices[i + 1]);
        }
        else
            EXPECT_FALSE (conflicts[i].other);
    }
    TerrainDesignResult result;
    result.stations = { 77 };
    const auto before = result;
    EXPECT_FALSE (EvaluateTerrainDesign (f.doc, capture.Input (), f.Options (), result, error));
    EXPECT_EQ (result, before);
    EXPECT_NE (error.find ("at fixed source " + std::to_string (sources[0])), std::string::npos);
}
