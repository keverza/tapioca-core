// #112 P4a: the neutral PublishedTerrain. These tests pin the neutral result
// contract: unified-lane conversion with explicit rebuild versus measurement
// scopes, the domains=5 mesh projection with native-prefix preservation,
// authoritative triangle-exact quantities (footprint attribution with holes and
// overlap resolution, zero-contour cut/fill split), the legacy boundary
// conversion as parity oracle, the daylight sampled-error grid tolerance, and
// refusal/reload behavior at the build boundary. No production dispatch.
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditLandscapeRecipe.hpp"
#include "Edit/EditRegionTerrain.hpp"

#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditSession.hpp"
#include "Geometry/Mesh.hpp"
#include "legacy_region_composition.hpp"
#include "virtual_editor_debug_replay.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
using namespace tapioca::geometry;

// 9 x 9 vertices over [0, 8] x [0, 8] with z = slope * x + offset, so a graded
// level crosses the baseline and the zero-height-difference contour runs
// through domains.
struct Base {
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    Base (double slope = 0.0, double offset = 0.0)
    {
        for (uint32_t row = 0; row < 9; ++row)
            for (uint32_t column = 0; column < 9; ++column)
                points.insert (points.end (), { double (column), double (row), slope * column + offset });
        for (uint32_t row = 0; row < 8; ++row)
            for (uint32_t column = 0; column < 8; ++column) {
                const uint32_t a = row * 9 + column;
                triangles.insert (triangles.end (), { a, a + 1, a + 10, a, a + 10, a + 9 });
            }
    }
};

ProvenanceRequest Provenance (geo::EntityCounts counts)
{
    ProvenanceRequest request;
    request.expectedSession = { 1 };
    request.counts = counts;
    request.domains = 7;
    auto& table = request.table;
    table.context = { 1, request.expectedSession, 0, 100 };
    table.sources = { { 0x20000000000001ULL, {} } };
    table.operations = { { 12, {} } };
    table.objects = { { 13, 12 } };
    table.sourceIds = { 0x20000000000001ULL };
    table.operationIds = { 12 };
    for (auto [domain, count] : { std::pair { geo::IdentityDomain::Vertex, counts.vertices },
                                  { geo::IdentityDomain::Edge, counts.edges },
                                  { geo::IdentityDomain::Face, counts.faces } }) {
        for (uint64_t index = 0; index < count; ++index) {
            EntityMapping row;
            row.domain = domain;
            row.kind = MappingKind::Generated;
            row.outputIndex = index;
            row.sources = { 0, 1 };
            row.operations = { 0, 1 };
            row.object = 13;
            table.mappings.push_back (row);
        }
    }
    return request;
}

std::vector<double> Ring (std::initializer_list<std::array<double, 2>> corners, double z = 0.0)
{
    std::vector<double> ring;
    for (const auto& [x, y] : corners)
        ring.insert (ring.end (), { x, y, z });
    return ring;
}

geo::FieldPlanPart Part (std::vector<std::vector<double>> rings)
{
    geo::FieldPlanPart part;
    for (const auto& ring : rings) {
        std::vector<edit::Vec3> points;
        for (size_t at = 0; at + 2 < ring.size (); at += 3)
            points.push_back ({ ring[at], ring[at + 1], ring[at + 2] });
        part.rings.push_back (points);
    }
    return part;
}

geo::FieldPlanEntry PlanEntry (uint32_t order, uint64_t object, uint64_t operation,
                               std::vector<geo::FieldPlanPart> parts)
{
    geo::FieldPlanEntry entry;
    entry.order = order;
    entry.object = object;
    entry.rootOperation = operation;
    entry.parts = std::move (parts);
    return entry;
}

lg::UnifiedEntry ConstantEntry (uint32_t order, uint64_t object, uint64_t operation, double level,
                                std::span<const lg::UnifiedDomainPart> parts)
{
    lg::UnifiedEntry entry;
    entry.order = order;
    entry.object = object;
    entry.rootOperation = operation;
    entry.surface = { lg::UnifiedSurfaceKind::Constant, level };
    // A small positive falloff: grid vertices sit 1 m apart, so only the ring
    // vertices (distance 0) take the target and the result is the sharp case.
    entry.transition = { lg::UnifiedTransitionKind::Falloff, 0.5, {}, {}, 0.0 };
    entry.parts = parts;
    return entry;
}

debugreplay::Scene Issue82Scene ()
{
    const auto file = std::filesystem::path (__FILE__).parent_path () / "fixtures/region-boundary-20261029.debug.json";
    std::ifstream input (file, std::ios::binary);
    if (!input)
        throw std::runtime_error ("missing original #82 fixture");
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = evp::nodegraph::json::Parse (text);
    if (!parsed.ok)
        throw std::runtime_error (parsed.error);
    auto scene = debugreplay::Read (parsed.value);
    // The 2026-10-29 capture asked for outline grading, which the mixed lane graded as the
    // selected path's band until #149 T1. These tests pin that band contract, so they ask for
    // the band; the outline over the same capture has its own tests.
    scene.options.outline = false;
    return scene;
}

geo::FieldPlan CompiledPlan (const debugreplay::Scene& scene, const geo::TerrainCapture& capture, bool includePath)
{
    geo::LandscapeSnapshot snapshot;
    snapshot.doc = &scene.doc;
    snapshot.revision = scene.doc.revision;
    snapshot.captureRevision = capture.revision;
    snapshot.session = scene.doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = scene.doc.landscape->sourceTerrainGuid;
    if (includePath) {
        geo::PathDesignRequest path;
        path.source = scene.options.source;
        path.operation = scene.options.operation;
        path.object = scene.options.object;
        path.maxStep = scene.options.maxStep;
        path.taper = scene.options.taper;
        path.margin = scene.options.margin;
        path.arcTolerance = scene.options.arcTolerance;
        path.cutLimit = scene.options.cutLimit;
        path.fillLimit = scene.options.fillLimit;
        path.effect = scene.options.effect;
        path.tolerance = scene.options.tolerance;
        path.outline = scene.options.outline;
        snapshot.path = path;
    }
    return geo::CompileLandscapeFieldPlan (snapshot);
}

const geo::PublishedDomainQuantity* Row (const geo::PublishedTerrain& published, uint32_t order)
{
    const auto found = std::find_if (published.domainQuantities.begin (), published.domainQuantities.end (),
                                     [=] (const auto& row) { return row.order == order; });
    return found == published.domainQuantities.end () ? nullptr : &*found;
}

double Sum (const geo::PublishedTerrain& published, double (geo::PublishedDomainQuantity::* field))
{
    double total = 0.0;
    for (const auto& row : published.domainQuantities)
        total += row.*field;
    return total;
}

bool InsideRing (const std::vector<double>& ring, double x, double y)
{
    bool inside = false;
    const size_t count = ring.size () / 3;
    for (size_t a = 0; a < count; ++a) {
        const size_t b = (a + 1) % count;
        const double ay = ring[a * 3 + 1], by = ring[b * 3 + 1];
        if ((ay > y) != (by > y)) {
            const double cross = ring[b * 3] - ring[a * 3];
            if (x < ring[a * 3] + (y - ay) / (by - ay) * cross)
                inside = !inside;
        }
    }
    return inside;
}

bool InsidePart (const std::vector<std::vector<double>>& rings, double x, double y)
{
    if (rings.empty () || !InsideRing (rings.front (), x, y))
        return false;
    for (size_t ring = 1; ring < rings.size (); ++ring)
        if (InsideRing (rings[ring], x, y))
            return false;
    return true;
}

// Samples both published surfaces on a regular grid over the rebuild region's
// bounds, in visit order.
std::pair<std::vector<lg::TerrainSample>, std::vector<lg::TerrainSample>>
SampleGrid (const geo::PublishedTerrain& unified, const geo::PublishedTerrain& legacy, double step, std::string& error)
{
    std::array<double, 4> box { 1e300, 1e300, -1e300, -1e300 };
    for (size_t at = 0; at + 2 < unified.rebuildRegion.size (); at += 3) {
        box[0] = std::min (box[0], unified.rebuildRegion[at]);
        box[1] = std::min (box[1], unified.rebuildRegion[at + 1]);
        box[2] = std::max (box[2], unified.rebuildRegion[at]);
        box[3] = std::max (box[3], unified.rebuildRegion[at + 1]);
    }
    std::vector<double> queries;
    for (double y = box[1] + step / 2; y < box[3]; y += step)
        for (double x = box[0] + step / 2; x < box[2]; x += step)
            queries.insert (queries.end (), { x, y });
    std::vector<lg::TerrainSample> a, b;
    lg::TerrainFieldSummary summaryA, summaryB;
    if (!lg::SampleTerrain ({ unified.points, unified.triangles }, queries, step, a, summaryA, error) ||
        !lg::SampleTerrain ({ legacy.points, legacy.triangles }, queries, step, b, summaryB, error))
        return {};
    return { std::move (a), std::move (b) };
}

} // namespace

// Neutral-result invariants on the real mixed #82 scene: geometry and
// attribution copy, both scopes reported separately, identity stamps, the
// domains=5 projection preserving the native baseline prefix, display totals
// measured against the measurement region, and kernel-exact volumes carried
// alongside rather than replacing them.
TEST (EditPublishedTerrain, UnifiedPublishCarriesScopesIdentityAndProvenance)
{
    auto scene = Issue82Scene ();
    scene.options.source = 0;
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    scene.options.source = 8;
    const auto plan = CompiledPlan (scene, *regionOnly.capture, true);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.entries.size (), 1u);
    ASSERT_EQ (plan.paths.size (), 1u);

    lg::UnifiedTerrainResult unified;
    lg::UnifiedLandscapeBuildScope scope;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error))
        << error;
    ASSERT_FALSE (scope.rebuildRegion.empty ());
    ASSERT_FALSE (scope.measurementRegion.empty ());

    geo::PublishedTerrain published;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, *regionOnly.capture, scope, published, error)) << error;
    EXPECT_EQ (published.points, unified.points);
    EXPECT_EQ (published.triangles, unified.triangles);
    EXPECT_EQ (published.base, unified.base);
    EXPECT_EQ (published.owners, unified.owners);
    EXPECT_EQ (published.mappings, unified.mappings);
    EXPECT_EQ (published.lines, unified.lines);
    EXPECT_EQ (published.rebuildRegion, scope.rebuildRegion);
    EXPECT_EQ (published.measurementRegion, scope.measurementRegion);
    EXPECT_EQ (published.revision, plan.revision);
    EXPECT_EQ (published.planHash, plan.hash);
    EXPECT_EQ (published.session, plan.session);
    EXPECT_EQ (published.sourceTerrainGuid, plan.sourceTerrainGuid);

    // Ordered owner identity: the region entry first, the path after it.
    ASSERT_EQ (published.ownersIdentity.size (), 2u);
    EXPECT_EQ (published.ownersIdentity[0].order, 0u);
    EXPECT_EQ (published.ownersIdentity[0].operation, plan.entries[0].rootOperation);
    EXPECT_FALSE (published.ownersIdentity[0].path);
    EXPECT_EQ (published.ownersIdentity[1].order, 1u);
    EXPECT_EQ (published.ownersIdentity[1].operation, plan.paths[0].operation);
    EXPECT_TRUE (published.ownersIdentity[1].path);

    // Full catalog retained; the mesh projection drops edges only and keeps
    // the native baseline prefix exact through the support table.
    EXPECT_EQ (published.inputProvenance.domains, 7u);
    const auto& mesh = published.meshProvenance;
    ASSERT_EQ (mesh.domains, geo::kMeshProvenanceDomains);
    EXPECT_EQ (mesh.counts.vertices, published.points.size () / 3);
    EXPECT_EQ (mesh.counts.edges, 0u);
    EXPECT_EQ (mesh.counts.faces, published.triangles.size () / 3);
    ASSERT_EQ (mesh.table.mappings.size (), mesh.counts.vertices + mesh.counts.faces);
    ASSERT_FALSE (mesh.table.predecessors.empty ());
    const auto& inputTable = published.inputProvenance.table.context;
    ASSERT_FALSE (mesh.table.supportTables.empty ());
    EXPECT_EQ (mesh.table.supportTables.back ().context.tableId, inputTable.tableId);
    for (size_t index = 0; index < unified.inputPointCount; ++index) {
        const auto& row = mesh.table.mappings[index];
        EXPECT_EQ (row.domain, geo::IdentityDomain::Vertex);
        EXPECT_EQ (row.outputIndex, index);
        ASSERT_EQ (row.predecessors.count, 1u);
        const auto& origin = mesh.table.predecessors.at (row.predecessors.offset);
        EXPECT_EQ (origin.tableId, inputTable.tableId);
        EXPECT_EQ (origin.entityIndex, index);
    }
    // Rebuild-created vertices above the mesh prefix are generated rows citing
    // the catalog contributors.
    for (size_t index = unified.inputPointCount; index < mesh.table.mappings.size (); ++index) {
        const auto& row = mesh.table.mappings[index];
        if (row.domain != geo::IdentityDomain::Vertex)
            continue;
        EXPECT_EQ (row.kind, MappingKind::Generated);
        ASSERT_GE (row.sources.count, 1u);
        ASSERT_GE (row.operations.count, 1u);
    }

    // Quantities: display totals are the 0.25 m convention against the
    // measurement region (recomputing them independently agrees bit for bit);
    // the kernel-exact rebuild-scope volumes are carried alongside.
    EXPECT_EQ (published.exactSite, unified.site);
    EXPECT_EQ (published.exactContext, unified.context);
    lg::TerrainVolumeRequest request;
    request.existing = { regionOnly.capture->points, regionOnly.capture->triangles };
    request.proposed = lg::TerrainVolumeRequest::Proposed::Mesh;
    request.proposedMesh = { published.points, published.triangles };
    request.cell = 0.25;
    request.property = published.measurementRegion;
    lg::TerrainVolumeReport report;
    ASSERT_TRUE (lg::MeasureTerrainVolumes (request, report, error)) << error;
    EXPECT_DOUBLE_EQ (published.displaySite.cut, report.site.cut);
    EXPECT_DOUBLE_EQ (published.displaySite.fill, report.site.fill);
    EXPECT_GT (published.quantities.fill, 0.0);

    // Per-domain rows plus the reconciling baseline row sum exactly to the
    // authoritative triangle-exact total.
    ASSERT_FALSE (published.domainQuantities.empty ());
    EXPECT_EQ (published.domainQuantities.front ().order, UINT32_MAX);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::fill), published.quantities.fill, 1e-9);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::cut), published.quantities.cut, 1e-9);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::area), published.quantities.area, 1e-9);
    EXPECT_NEAR (published.quantities.net, published.quantities.fill - published.quantities.cut, 1e-12);
}

// Authoritative quantities on a sloped base: a holed domain overlapped by a
// later nested domain. Holes are never charged to the surrounding domain, the
// overlap is charged to the later stack position within its full footprint,
// cut and fill are separated at the zero-height-difference contour, and every
// row reconciles exactly. Domain rings are constrained breaklines of this very
// triangulation, so no triangle straddles a boundary: whole-triangle
// attribution is exact and only the zero contour needs clipping.
TEST (EditPublishedTerrain, QuantitiesAttributeHolesOverlapsAndSplitCutFillExactly)
{
    const Base base (1.0, -4.0); // z = x - 4 in [-4, 4]: targets both cut and fill.
    const auto property = Ring ({ { 0, 0 }, { 8, 0 }, { 8, 8 }, { 0, 8 } });
    // Entry A grades [1,7]x[1,7] minus the hole [3,5]x[3,5] to 2.0; entry B
    // overrides [2,6]x[2,6] (including over A's hole) to 4.0.
    const auto outerA = Ring ({ { 1, 1 }, { 7, 1 }, { 7, 7 }, { 1, 7 } });
    const auto holeA = Ring ({ { 3, 3 }, { 5, 3 }, { 5, 5 }, { 3, 5 } });
    const auto domainB = Ring ({ { 2, 2 }, { 6, 2 }, { 6, 6 }, { 2, 6 } });
    std::vector<std::span<const double>> holes { holeA };
    const std::array partsA { lg::UnifiedDomainPart { outerA, holes } };
    const std::array partsB { lg::UnifiedDomainPart { domainB, std::span<const std::span<const double>> {} } };
    const std::array entries { ConstantEntry (0, 100, 200, 2.0, partsA), ConstantEntry (1, 101, 201, 4.0, partsB) };
    geo::EntityCounts counts { uint64_t (base.points.size () / 3 + 16), 16, uint64_t (base.triangles.size () / 3), 0 };
    const lg::UnifiedPlanRequest request {
        { base.points, base.triangles }, property, entries, {}, {}, {}, 1e-3, 7, 0xB0B1, Provenance (counts)
    };
    lg::UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedTerrain (request, unified, error)) << error;

    geo::FieldPlan plan;
    plan.version = geo::kFieldPlanVersion;
    plan.revision = 7;
    plan.captureRevision = 7;
    plan.session = { 1 };
    plan.sourceTerrainGuid = "guid";
    plan.hash = 0xB0B1;
    plan.entries = { PlanEntry (0, 100, 200, { Part ({ outerA, holeA }) }),
                     PlanEntry (1, 101, 201, { Part ({ domainB }) }) };
    geo::UnifiedLandscapeBuildScope scope;
    scope.rebuildRegion = property;
    scope.measurementRegion = property;
    geo::TerrainCapture baseline;
    baseline.points = base.points;
    baseline.triangles = base.triangles;

    geo::PublishedTerrain published;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, baseline, scope, published, error)) << error;

    // Area and volume attribution on this grid-aligned scene:
    // - B owns exactly its 16 m2 footprint, including over A's hole: the
    //   overlap is charged to the later stack position only.
    // - A owns its exclusive footprint (20 m2) plus the boundary band: every
    //   grid edge along its rings carries band triangles whose ring vertices
    //   A moved (hard ring vertices keep one height, so the neighbour
    //   triangle interpolates; no vertical wall is claimed). That is 26 m2.
    // - The hole interior is never charged to A: its vertices keep baseline
    //   heights and its triangles reconcile into the remaining rows.
    // - The untouched surround (2 m2 here) reconciles into the baseline row.
    const auto* rowA = Row (published, 0);
    const auto* rowB = Row (published, 1);
    const auto* rowBase = Row (published, UINT32_MAX);
    ASSERT_NE (rowA, nullptr);
    ASSERT_NE (rowB, nullptr);
    ASSERT_NE (rowBase, nullptr);
    EXPECT_NEAR (rowB->area, 16.0, 1e-9);
    EXPECT_NEAR (rowA->area + rowB->area + rowBase->area, published.quantities.area, 1e-9);
    EXPECT_NEAR (rowA->area, 46.0, 1e-9);
    EXPECT_NEAR (rowBase->area, 2.0, 1e-9);

    // The sloped baseline crosses both targets: A cuts and fills; B raises
    // everything inside its footprint: pure fill. The baseline row carries
    // only untouched triangles: with every boundary band attributed to the
    // domain that moved its ring vertices, its volume is exactly zero.
    EXPECT_GT (rowA->cut, 0.0);
    EXPECT_GT (rowA->fill, 0.0);
    EXPECT_NEAR (rowA->net, rowA->fill - rowA->cut, 1e-12);
    EXPECT_GT (rowB->fill, 0.0);
    EXPECT_EQ (rowB->cut, 0.0);
    EXPECT_EQ (rowBase->fill, 0.0);
    EXPECT_EQ (rowBase->cut, 0.0);
    EXPECT_GT (published.quantities.cut, 0.0);
    EXPECT_GT (published.quantities.fill, 0.0);
    EXPECT_NEAR (published.quantities.net, published.quantities.fill - published.quantities.cut, 1e-12);

    // Exact reconciliation across the whole partition, areas, volumes and
    // triangle counts alike.
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::area), published.quantities.area, 1e-9);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::cut), published.quantities.cut, 1e-9);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::fill), published.quantities.fill, 1e-9);
    double triangles = 0;
    for (const auto& row : published.domainQuantities)
        triangles += double (row.triangles);
    EXPECT_NEAR (triangles, double (published.quantities.triangles), 1e-9);

    // Owner mappings attribute the same way the footprints do: B inside its
    // ring, A in its exclusive annulus, none in the hole.
    for (size_t vertex = 0; vertex < published.owners.size (); ++vertex) {
        if (!published.owners[vertex])
            continue;
        const double x = published.points[vertex * 3], y = published.points[vertex * 3 + 1];
        const bool inB = x >= 2.0 && x <= 6.0 && y >= 2.0 && y <= 6.0;
        const bool inHole = x >= 3.0 && x <= 5.0 && y >= 3.0 && y <= 5.0;
        const bool inA = x >= 1.0 && x <= 7.0 && y >= 1.0 && y <= 7.0 && !inHole;
        if (inB)
            EXPECT_EQ (*published.owners[vertex], 1u) << x << ", " << y;
        else if (inA)
            EXPECT_EQ (*published.owners[vertex], 0u) << x << ", " << y;
        else
            ADD_FAILURE () << "unexpected owner outside every domain at " << x << ", " << y;
    }
}

// #112 P4c: a multi-part entry keeps its parts separate. The baseline is flat
// AT the target level, so no vertex moves and no owner is set: attribution is
// purely geometric. A flattened footprint would treat the second island's ring
// as a hole of the first and lose its area to the baseline row.
TEST (EditPublishedTerrain, QuantitiesAttributeIslandsByPartNotByFlattenedPseudoPolygon)
{
    const Base base (0.0, 2.0); // z = 2 everywhere: the level-2 target moves nothing.
    const auto property = Ring ({ { 0, 0 }, { 8, 0 }, { 8, 8 }, { 0, 8 } });
    const auto islandA = Ring ({ { 1, 1 }, { 3, 1 }, { 3, 3 }, { 1, 3 } });
    const auto islandB = Ring ({ { 5, 5 }, { 7, 5 }, { 7, 7 }, { 5, 7 } });
    const std::array parts { lg::UnifiedDomainPart { islandA, std::span<const std::span<const double>> {} },
                             lg::UnifiedDomainPart { islandB, std::span<const std::span<const double>> {} } };
    const std::array entries { ConstantEntry (0, 100, 200, 2.0, parts) };
    geo::EntityCounts counts { uint64_t (base.points.size () / 3 + 12), 12, uint64_t (base.triangles.size () / 3), 0 };
    const lg::UnifiedPlanRequest request {
        { base.points, base.triangles }, property, entries, {}, {}, {}, 1e-3, 7, 0x1510, Provenance (counts)
    };
    lg::UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedTerrain (request, unified, error)) << error;

    geo::FieldPlan plan;
    plan.version = geo::kFieldPlanVersion;
    plan.revision = 7;
    plan.captureRevision = 7;
    plan.session = { 1 };
    plan.sourceTerrainGuid = "guid";
    plan.hash = 0x1510;
    plan.entries = { PlanEntry (0, 100, 200, { Part ({ islandA }), Part ({ islandB }) }) };
    geo::UnifiedLandscapeBuildScope scope;
    scope.rebuildRegion = property;
    scope.measurementRegion = property;
    geo::TerrainCapture baseline;
    baseline.points = base.points;
    baseline.triangles = base.triangles;

    geo::PublishedTerrain published;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, baseline, scope, published, error)) << error;

    // Both 2 x 2 m islands are claimed geometrically although nothing moved:
    // area 8 m2 to the entry, the untouched 56 m2 to the baseline row, and the
    // partition reconciles exactly.
    const auto* rowA = Row (published, 0);
    const auto* rowBase = Row (published, UINT32_MAX);
    ASSERT_NE (rowA, nullptr);
    ASSERT_NE (rowBase, nullptr);
    EXPECT_NEAR (rowA->area, 8.0, 1e-9);
    EXPECT_NEAR (rowBase->area, 56.0, 1e-9);
    EXPECT_EQ (rowA->cut, 0.0);
    EXPECT_EQ (rowA->fill, 0.0);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::area), published.quantities.area, 1e-9);
    // The entry row carries its authored identity.
    ASSERT_EQ (published.ownersIdentity.size (), 1u);
    EXPECT_EQ (rowA->operation, published.ownersIdentity[0].operation);
    EXPECT_EQ (rowA->object, published.ownersIdentity[0].object);
}

// #112 P4c: every stack slot reports a row even when it moved nothing, so the
// per-domain table is complete; the partition still reconciles exactly.
TEST (EditPublishedTerrain, QuantitiesEmitAnExplicitZeroRowPerStackSlot)
{
    const Base base (0.0, 2.0);
    const auto property = Ring ({ { 0, 0 }, { 8, 0 }, { 8, 8 }, { 0, 8 } });
    const auto domainA = Ring ({ { 1, 1 }, { 3, 1 }, { 3, 3 }, { 1, 3 } });
    const auto domainB = Ring ({ { 5, 5 }, { 7, 5 }, { 7, 7 }, { 5, 7 } });
    const std::array partsA { lg::UnifiedDomainPart { domainA, std::span<const std::span<const double>> {} } };
    const std::array partsB { lg::UnifiedDomainPart { domainB, std::span<const std::span<const double>> {} } };
    const std::array entries { ConstantEntry (0, 100, 200, 2.0, partsA), ConstantEntry (1, 101, 201, 2.0, partsB) };
    geo::EntityCounts counts { uint64_t (base.points.size () / 3 + 12), 12, uint64_t (base.triangles.size () / 3), 0 };
    const lg::UnifiedPlanRequest request {
        { base.points, base.triangles }, property, entries, {}, {}, {}, 1e-3, 7, 0x1511, Provenance (counts)
    };
    lg::UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedTerrain (request, unified, error)) << error;

    geo::FieldPlan plan;
    plan.version = geo::kFieldPlanVersion;
    plan.revision = 7;
    plan.captureRevision = 7;
    plan.session = { 1 };
    plan.sourceTerrainGuid = "guid";
    plan.hash = 0x1511;
    plan.entries = { PlanEntry (0, 100, 200, { Part ({ domainA }) }), PlanEntry (1, 101, 201, { Part ({ domainB }) }) };
    geo::UnifiedLandscapeBuildScope scope;
    scope.rebuildRegion = property;
    scope.measurementRegion = property;
    geo::TerrainCapture baseline;
    baseline.points = base.points;
    baseline.triangles = base.triangles;

    geo::PublishedTerrain published;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, baseline, scope, published, error)) << error;

    // Baseline row first, then one row per stack slot in stack order.
    ASSERT_EQ (published.domainQuantities.size (), 3u);
    EXPECT_EQ (published.domainQuantities[0].order, UINT32_MAX);
    EXPECT_EQ (published.domainQuantities[1].order, 0u);
    EXPECT_EQ (published.domainQuantities[2].order, 1u);
    for (const auto& row : published.domainQuantities) {
        EXPECT_EQ (row.cut, 0.0);
        EXPECT_EQ (row.fill, 0.0);
    }
    EXPECT_NEAR (published.domainQuantities[1].area, 4.0, 1e-9);
    EXPECT_NEAR (published.domainQuantities[2].area, 4.0, 1e-9);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::area), published.quantities.area, 1e-9);
}

// #112 P4c: the C++ exact integration and the kernel's exact volumes integrate
// the same affine height difference over the same whole published surface, so
// the authoritative total and the exact site/context pair reconcile.
TEST (EditPublishedTerrain, ExactQuantitiesReconcileWithTheKernelVolumes)
{
    const Base base (1.0, -4.0); // z = x - 4: both cut and fill across domains.
    const auto property = Ring ({ { 0, 0 }, { 8, 0 }, { 8, 8 }, { 0, 8 } });
    const auto outerA = Ring ({ { 1, 1 }, { 7, 1 }, { 7, 7 }, { 1, 7 } });
    const auto holeA = Ring ({ { 3, 3 }, { 5, 3 }, { 5, 5 }, { 3, 5 } });
    const auto domainB = Ring ({ { 2, 2 }, { 6, 2 }, { 6, 6 }, { 2, 6 } });
    std::vector<std::span<const double>> holes { holeA };
    const std::array partsA { lg::UnifiedDomainPart { outerA, holes } };
    const std::array partsB { lg::UnifiedDomainPart { domainB, std::span<const std::span<const double>> {} } };
    const std::array entries { ConstantEntry (0, 100, 200, 2.0, partsA), ConstantEntry (1, 101, 201, 4.0, partsB) };
    geo::EntityCounts counts { uint64_t (base.points.size () / 3 + 16), 16, uint64_t (base.triangles.size () / 3), 0 };
    const lg::UnifiedPlanRequest request {
        { base.points, base.triangles }, property, entries, {}, {}, {}, 1e-3, 7, 0x1512, Provenance (counts)
    };
    lg::UnifiedTerrainResult unified;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedTerrain (request, unified, error)) << error;

    geo::FieldPlan plan;
    plan.version = geo::kFieldPlanVersion;
    plan.revision = 7;
    plan.captureRevision = 7;
    plan.session = { 1 };
    plan.sourceTerrainGuid = "guid";
    plan.hash = 0x1512;
    plan.entries = { PlanEntry (0, 100, 200, { Part ({ outerA, holeA }) }),
                     PlanEntry (1, 101, 201, { Part ({ domainB }) }) };
    geo::UnifiedLandscapeBuildScope scope;
    scope.rebuildRegion = property;
    scope.measurementRegion = property;
    geo::TerrainCapture baseline;
    baseline.points = base.points;
    baseline.triangles = base.triangles;

    geo::PublishedTerrain published;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, baseline, scope, published, error)) << error;

    // Two independent exact integrations of the same field over the same mesh:
    // the C++ zero-contour partition and the kernel's inside/outside split.
    EXPECT_NEAR (published.quantities.cut, published.exactSite.cut + published.exactContext.cut, 1e-9);
    EXPECT_NEAR (published.quantities.fill, published.exactSite.fill + published.exactContext.fill, 1e-9);
    EXPECT_NEAR (published.quantities.area, published.exactSite.area + published.exactContext.area, 1e-9);
    EXPECT_GT (published.quantities.cut, 0.0);
    EXPECT_GT (published.quantities.fill, 0.0);
}

// The unified build and the legacy region-first/path-second orchestration must
// be the same surface within the documented region-daylight density tolerance:
// sampled on a 0.25 m grid over the rebuild region, and in measured volumes.
// #112 P4b removed the composition from the production lane and P4c routed
// region-only through the unified authority, so the legacy oracle is
// reconstructed here exactly as the old orchestration ran it: the region
// evaluation composed into the proposed capture the path lane consumed.
TEST (EditPublishedTerrain, UnifiedMatchesLegacyOnAQueryGridWithinDocumentedTolerance)
{
    auto scene = Issue82Scene ();
    // As captured: an outline request. Since #149 T1 the unified build grades it as its own stage
    // over the ground the region composed -- what the legacy oracle below does in two passes.
    scene.options.outline = true;
    scene.options.source = 0;
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    scene.options.source = 8;
    const auto mixed = debugreplay::Run (scene);
    ASSERT_TRUE (mixed.built) << mixed.error;
    const auto plan = CompiledPlan (scene, *regionOnly.capture, true);
    ASSERT_TRUE (plan.ok) << plan.error;

    lg::UnifiedTerrainResult unified;
    lg::UnifiedLandscapeBuildScope scope;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error))
        << error;
    geo::PublishedTerrain unifiedPublished;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, *regionOnly.capture, scope, unifiedPublished, error))
        << error;

    // The legacy comparison oracle: the region evaluation (still run as the
    // diagnostic stage) composed into the proposed capture the removed
    // production orchestration used to feed the path lane.
    std::string composeError;
    const auto proposed = legacycomposition::ComposedCapture (*regionOnly.regions, composeError);
    ASSERT_TRUE (proposed) << composeError;
    lg::TerrainDesignJob pathJob;
    pathJob.doc = std::make_shared<const geomsrv::edit::Document> (scene.doc);
    pathJob.capture = proposed;
    pathJob.options = scene.options;
    const auto legacy = lg::RunTerrainDesign (pathJob);
    ASSERT_TRUE (legacy.built) << legacy.error;

    // The legacy display convention measures against the authored property
    // line evaluated by the region stage (exactly what production Totals
    // uses), or the validated terrain cover when the document has none.
    std::vector<double> measurement =
        lg::TerrainDesignArea (scene.doc, { regionOnly.capture->points, regionOnly.capture->triangles });
    if (!mixed.regions->evaluation.landscape.propertyLine.empty ()) {
        measurement.clear ();
        for (const auto& point : mixed.regions->evaluation.landscape.propertyLine)
            measurement.insert (measurement.end (), { point.x, point.y, point.z });
    }
    geo::PublishedTerrain legacyPublished;
    ASSERT_TRUE (lg::PublishTerrainDesign (legacy, scene.options.operation, scene.options.object, measurement,
                                           legacyPublished, error))
        << error;
    EXPECT_EQ (legacyPublished.rebuildRegion, legacy.result.region);
    ASSERT_EQ (legacyPublished.ownersIdentity.size (), 1u);
    EXPECT_TRUE (legacyPublished.ownersIdentity[0].path);

    // Sampled-error grid over the rebuild region. The legacy lane densifies
    // region daylight transitions with eight offset rings; the unified
    // topology interpolates the same faces across sparser vertices. Measured
    // on this fixture: max sampled error 0.244 m, display fill delta 15.9 m3
    // (the daylight transition band). The tolerances document that density
    // difference, not a semantic one; locus/offset-ring densification is the
    // approved follow-up slice.
    const auto samples = SampleGrid (unifiedPublished, legacyPublished, 0.25, error);
    ASSERT_FALSE (samples.first.empty ()) << error;
    ASSERT_EQ (samples.first.size (), samples.second.size ());
    size_t compared = 0;
    double maxError = 0.0;
    for (size_t index = 0; index < samples.first.size (); ++index) {
        if (!samples.first[index].onSurface || !samples.second[index].onSurface)
            continue;
        maxError = std::max (maxError, std::fabs (samples.first[index].height - samples.second[index].height));
        ++compared;
    }
    EXPECT_GT (compared, 1000u);
    EXPECT_LE (maxError, 0.25) << "measured sampled error " << maxError;

    // Display volumes agree within the same documented density tolerance.
    EXPECT_NEAR (unifiedPublished.displaySite.fill, legacyPublished.displaySite.fill, 20.0);
    EXPECT_NEAR (unifiedPublished.displaySite.cut, legacyPublished.displaySite.cut, 20.0);
}

// A Boundary-elevation region whose Boolean-extended footprint leaves its
// authored height surface cannot compile a complete target: the strict build
// gate refuses atomically at the adapter boundary, before any Rust call. The
// derived Boundary-Z refusal is never silently replaced by invented heights.
// #112 P4c: Boundary-Z acceptance at the publication boundary. A region whose
// derived footprint stays on its authored boundary surface builds, publishes
// with the triangulated authored target, and quantifies the domain under its
// authored identity — the counterpart of the leaving-surface refusal below.
TEST (EditPublishedTerrain, BoundaryDomainPublishesWithQuantitiesAndAuthoredIdentity)
{
    auto scene = Issue82Scene ();
    scene.options.source = 0;
    const auto regionIds = lg::TerrainRegionObjects (scene.doc);
    ASSERT_EQ (regionIds.size (), 1u);
    auto& object = *std::find_if (scene.doc.landscape->objects.begin (), scene.doc.landscape->objects.end (),
                                  [&] (const auto& candidate) { return candidate.id == regionIds.front (); });
    object.surface->elevation = lg::RegionElevationMode::Boundary;
    object.surface->slope = lg::RegionSlopeMode::None;

    const auto result = debugreplay::Run (scene);
    ASSERT_TRUE (result.built) << result.error;
    ASSERT_TRUE (result.published) << "region-only publishes the unified snapshot";
    const auto& published = *result.published;

    // One entry row under the authored operation/object identity, and the
    // partition reconciles into the total.
    const auto* row = Row (published, 0);
    ASSERT_NE (row, nullptr);
    ASSERT_EQ (published.ownersIdentity.size (), 1u);
    EXPECT_FALSE (published.ownersIdentity[0].path);
    EXPECT_EQ (row->operation, published.ownersIdentity[0].operation);
    EXPECT_EQ (row->object, published.ownersIdentity[0].object);
    EXPECT_EQ (row->operation, uint64_t (object.footprintOperation));
    EXPECT_EQ (row->object, uint64_t (object.id));
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::fill), published.quantities.fill, 1e-9);
    EXPECT_NEAR (Sum (published, &geo::PublishedDomainQuantity::cut), published.quantities.cut, 1e-9);
    // The same exactness guard as the synthetic quantity scenes.
    EXPECT_NEAR (published.quantities.cut, published.exactSite.cut + published.exactContext.cut, 1e-9);
    EXPECT_NEAR (published.quantities.fill, published.exactSite.fill + published.exactContext.fill, 1e-9);

    // The graded interior still matches the authored boundary target, and the
    // produced target really is the triangulated authored surface: the plan
    // transports surface points for this entry (verified again at the facade
    // in GeometryRustUnifiedTerrain.CompiledPlanBuildsContourAndBoundaryTargets).
    const std::array<double, 2> query { 6.5, 6 };
    std::vector<lg::TerrainSample> samples;
    lg::TerrainFieldSummary summary;
    std::string error;
    ASSERT_TRUE (lg::SampleTerrain ({ published.points, published.triangles }, query, 0.25, samples, summary, error))
        << error;
    ASSERT_EQ (samples.size (), 1u);
    EXPECT_NEAR (samples.front ().height, 0, 1e-6) << "The boundary domain grades to its authored surface.";
}

TEST (EditPublishedTerrain, DerivedBoundarySurfaceRefusesAtTheBuildBoundary)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (lg::StartLandscape (doc, { 1 }, error)) << error;
    doc.landscape->sourceTerrainGuid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
    const auto subject = [&] {
        const auto line = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
        EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error)) << error;
        return doc.landscape->objects.back ().id;
    }();
    const auto operand = [&] {
        const auto line = edit::AddPolyline (doc, { { 5, 0, 0 }, { 15, 0, 0 }, { 15, 10, 0 }, { 5, 10, 0 } }, true);
        EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error)) << error;
        return doc.landscape->objects.back ().id;
    }();
    // The subject's authored target is Boundary-Z, sampled from its own live
    // boundary cover [0,10]x[0,10].
    lg::RegionSurface surface;
    surface.elevation = lg::RegionElevationMode::Boundary;
    lg::MaterialSpec material;
    ASSERT_TRUE (lg::SetRegionSurface (doc, subject, surface, material, error)) << error;
    lg::RegionTerrain terrain;
    terrain.effect = lg::RegionTerrainEffect::Grade;
    ASSERT_TRUE (lg::SetRegionTerrain (doc, subject, terrain, error)) << error;

    // A union extends the subject's footprint to [0,15]x[0,10]: the derived
    // object keeps the Boundary surface, and its target now leaves the authored
    // height surface, which must refuse rather than invent heights.
    const auto rootOf = [&] (edit::Id object) {
        const auto found = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                         [=] (const auto& candidate) { return candidate.id == object; });
        EXPECT_NE (found, doc.landscape->objects.end ());
        return found->footprintOperation;
    };
    lg::DomainCommand command;
    command.kind = lg::OperationKind::Union;
    command.name = "Union";
    command.subject.input = rootOf (subject);
    command.operand.input = rootOf (operand);
    const auto applied = lg::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;

    lg::Id terrainSource = 0;
    ASSERT_TRUE (lg::SetLandscapeTerrain (doc, doc.landscape->sourceTerrainGuid, terrainSource, error)) << error;

    geomsrv::Mesh mesh;
    mesh.guid = doc.landscape->sourceTerrainGuid;
    mesh.vertices = { -5, -5, 0, 20, -5, 0, 20, 15, 0, -5, 15, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    lg::TerrainCapture capture;
    ASSERT_TRUE (lg::CaptureLandscapeTerrain (doc, mesh, { subject, 200, 100 }, capture, error)) << error;

    auto snapshot = geo::LandscapeSnapshot {};
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
    snapshot.captureRevision = capture.revision;
    snapshot.session = doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = doc.landscape->sourceTerrainGuid;
    const auto plan = geo::CompileLandscapeFieldPlan (snapshot);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_FALSE (plan.diagnostics.empty ());
    ASSERT_EQ (plan.diagnostics.front ().code, geo::FieldPlanDiagnosticCode::SurfacePrepareFailed);
    ASSERT_EQ (uint64_t (plan.diagnostics.front ().object), uint64_t (applied.objectId));

    lg::UnifiedTerrainResult unified;
    unified.points = { 42 };
    const auto before = unified;
    lg::UnifiedLandscapeBuildScope scope;
    scope.rebuildRegion = { 1 };
    EXPECT_FALSE (lg::BuildUnifiedLandscapeTerrain (doc, capture, plan, {}, unified, scope, error));
    EXPECT_EQ (0U, error.rfind ("FieldPlanIncomplete:", 0)) << error;
    EXPECT_EQ (unified, before);
    EXPECT_EQ (scope.rebuildRegion, std::vector<double> ({ 1 }));
}

// A document edit after compilation changes revision and content hash; the
// stale plan is refused at the adapter boundary and the recompiled plan
// publishes fresh identity.
TEST (EditPublishedTerrain, SourceEditChangesPlanIdentityAndStalePlanRefuses)
{
    auto scene = Issue82Scene ();
    scene.options.source = 0;
    const auto regionOnly = debugreplay::Run (scene);
    ASSERT_TRUE (regionOnly.built) << regionOnly.error;
    scene.options.source = 8;
    auto plan = CompiledPlan (scene, *regionOnly.capture, true);
    ASSERT_TRUE (plan.ok) << plan.error;
    const auto firstHash = plan.hash;

    lg::UnifiedTerrainResult unified;
    lg::UnifiedLandscapeBuildScope scope;
    std::string error;
    ASSERT_TRUE (lg::BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error))
        << error;
    geo::PublishedTerrain published;
    ASSERT_TRUE (lg::PublishUnifiedTerrain (unified, plan, *regionOnly.capture, scope, published, error)) << error;
    EXPECT_EQ (published.revision, scene.doc.revision);

    // Any live document edit moves the revision: the compiled plan no longer
    // describes the document and must refuse as stale, publishing nothing.
    edit::AddPolyline (scene.doc, { { 30, 30, 0 }, { 40, 30, 0 } }, false);
    ASSERT_GT (scene.doc.revision, plan.revision);
    const auto stale = unified;
    EXPECT_FALSE (lg::BuildUnifiedLandscapeTerrain (scene.doc, *regionOnly.capture, plan, {}, unified, scope, error));
    EXPECT_EQ (0U, error.rfind ("ProvenanceStale", 0)) << error;
    EXPECT_EQ (unified, stale);

    // Recompiled against the edited document on a fresh capture: fresh
    // revision and content hash.
    geomsrv::Mesh edited;
    edited.guid = scene.doc.landscape->sourceTerrainGuid;
    edited.vertices = regionOnly.capture->points;
    edited.triangles = regionOnly.capture->triangles;
    const auto capture = std::make_shared<lg::TerrainCapture> ();
    ASSERT_TRUE (
        lg::CaptureLandscapeTerrain (scene.doc, edited, { plan.entries.front ().object, 210, 110 }, *capture, error))
        << error;
    plan = CompiledPlan (scene, *capture, true);
    ASSERT_TRUE (plan.ok) << plan.error;
    EXPECT_GT (plan.revision, published.revision);
    EXPECT_NE (plan.hash, firstHash);
}

// #105 step 4 end to end: the Operations recipe names every row of a real mixed publication by its
// own stack position (region, then the path), the baseline row comes last, the rows reconcile to
// the exact total, and the recipe's plan hash is the one the build published.
TEST (EditPublishedTerrain, RecipeQuantityRowsNameEveryStackPositionOfAMixedBuild105)
{
    auto scene = Issue82Scene ();
    scene.options.source = 8;
    const auto mixed = debugreplay::Run (scene);
    ASSERT_TRUE (mixed.built) << mixed.error;
    ASSERT_TRUE (mixed.published);
    const auto& published = *mixed.published;
    // The job's own request (EditLandscapeTerrainJob.cpp RunLandscapeTerrainDesign).
    const auto request = geo::PathRequestFor (scene.options, !scene.region.empty ());
    const auto recipe = geo::ProjectLandscapeRecipe (scene.doc, request);
    ASSERT_TRUE (recipe.ok) << recipe.error;
    ASSERT_EQ (published.revision, recipe.revision);
    // The recipe shows the hash of the plan this build ran (its capture revision included).
    EXPECT_EQ (recipe.hash, published.planHash);

    const auto q = geo::ProjectRecipeQuantities (recipe, &published, false);
    ASSERT_TRUE (q.available);
    EXPECT_TRUE (q.current);
    ASSERT_EQ (q.rows.size (), published.domainQuantities.size ());
    ASSERT_GE (q.rows.size (), 3u) << "one region, the path and the baseline row";
    bool region = false, path = false;
    for (size_t i = 0; i < q.rows.size (); ++i) {
        const auto& row = q.rows[i];
        if (row.baseline) {
            EXPECT_EQ (i + 1, q.rows.size ()) << "the baseline row comes last";
            continue;
        }
        const auto named = std::find_if (recipe.rows.begin (), recipe.rows.end (),
                                         [&] (const geo::RecipeRow& r) { return r.order == row.order; });
        ASSERT_NE (named, recipe.rows.end ()) << "stack position " << row.order;
        EXPECT_EQ (named->object, row.object);
        EXPECT_EQ (named->name, row.name);
        region = region || named->stage == geo::RecipeStage::Region;
        path = path || named->stage == geo::RecipeStage::Path;
    }
    EXPECT_TRUE (region);
    EXPECT_TRUE (path);
    EXPECT_LT (q.residual, 1e-6);
    EXPECT_NEAR (q.total.fill, published.quantities.fill, 0);
    EXPECT_GT (q.total.fill + q.total.cut, 0);
}

// #105 step 5 acceptance: two overlapping regions, one cut under one fill, built for real. The
// quantity rows follow the recipe through a reorder, an effect change, undo and save/reload: the
// later region owns the overlap, each state's rows reconcile to its total, and returning to a state
// reproduces its quantities exactly.
namespace {
struct StackBuild {
    geo::LandscapeRecipe recipe;
    geo::TerrainDesignOutcome outcome;
    geo::RecipeQuantities quantities;
};
edit::Id StackRegion (edit::Document& doc, double x, double z)
{
    const auto line = edit::AddPolyline (doc, { { x, 5, 0 }, { x + 10, 5, 0 }, { x + 10, 15, 0 }, { x, 15, 0 } }, true);
    std::string error;
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
    const auto object = doc.landscape->objects.back ().id;
    geo::RegionSurface surface;
    surface.elevation = geo::RegionElevationMode::Constant;
    surface.first.z = z;
    EXPECT_TRUE (geo::SetRegionSurface (doc, object, surface, {}, error)) << error;
    geo::RegionTerrain terrain;
    terrain.effect = geo::RegionTerrainEffect::Grade;
    EXPECT_TRUE (geo::SetRegionTerrain (doc, object, terrain, error)) << error;
    return object;
}
StackBuild BuildStack (const edit::Document& doc, edit::Id owner)
{
    StackBuild out;
    geomsrv::Mesh mesh;
    mesh.guid = doc.landscape->sourceTerrainGuid;
    mesh.vertices = { 0, 0, 0, 30, 0, 0, 30, 20, 0, 0, 20, 0 };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    auto capture = std::make_shared<geo::TerrainCapture> ();
    std::string error;
    EXPECT_TRUE (geo::CaptureLandscapeTerrain (doc, mesh, { owner, 2, 1 }, *capture, error)) << error;
    geo::TerrainDesignJob job;
    job.doc = std::make_shared<const edit::Document> (doc);
    job.capture = capture;
    job.options.object = owner;
    out.outcome = geo::RunLandscapeTerrainDesign (job);
    EXPECT_TRUE (out.outcome.built) << out.outcome.error;
    out.recipe = geo::ProjectLandscapeRecipe (doc, std::nullopt);
    EXPECT_TRUE (out.recipe.ok) << out.recipe.error;
    out.quantities = geo::ProjectRecipeQuantities (out.recipe, out.outcome.published.get (), false);
    return out;
}
const geo::RecipeQuantityRow* RowOf (const geo::RecipeQuantities& q, edit::Id object)
{
    for (const auto& row : q.rows)
        if (!row.baseline && row.object == object)
            return &row;
    return nullptr;
}
std::vector<edit::Id> StackOrder (const geo::RecipeQuantities& q)
{
    std::vector<edit::Id> order;
    for (const auto& row : q.rows)
        if (!row.baseline)
            order.push_back (row.object);
    return order;
}
void ExpectCurrentAndReconciled (const StackBuild& build)
{
    ASSERT_TRUE (build.quantities.available) << build.quantities.reason;
    EXPECT_TRUE (build.quantities.current);
    ASSERT_TRUE (build.outcome.published);
    EXPECT_EQ (build.recipe.hash, build.outcome.published->planHash);
    EXPECT_LT (build.quantities.residual, 1e-6);
    double cut = 0, fill = 0;
    for (const auto& row : build.quantities.rows) {
        cut += row.cut;
        fill += row.fill;
    }
    EXPECT_NEAR (cut, build.quantities.total.cut, 1e-6);
    EXPECT_NEAR (fill, build.quantities.total.fill, 1e-6);
}
void ExpectSameQuantities (const geo::RecipeQuantities& a, const geo::RecipeQuantities& b)
{
    ASSERT_EQ (a.rows.size (), b.rows.size ());
    for (size_t i = 0; i < a.rows.size (); ++i) {
        EXPECT_EQ (a.rows[i].object, b.rows[i].object) << i;
        EXPECT_EQ (a.rows[i].order, b.rows[i].order) << i;
        EXPECT_NEAR (a.rows[i].cut, b.rows[i].cut, 1e-9) << i;
        EXPECT_NEAR (a.rows[i].fill, b.rows[i].fill, 1e-9) << i;
    }
    EXPECT_NEAR (a.total.cut, b.total.cut, 1e-9);
    EXPECT_NEAR (a.total.fill, b.total.fill, 1e-9);
}
} // namespace

TEST (EditPublishedTerrain, RecipeQuantitiesFollowReorderEffectUndoAndReloadOfOverlappingRegions105)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1 }, error)) << error;
    edit::Id terrain = 0;
    ASSERT_TRUE (geo::SetLandscapeTerrain (doc, "00112233-4455-6677-8899-AABBCCDDEEFF", terrain, error)) << error;
    const auto cut = StackRegion (doc, 5, -1);  // 10 x 10 at -1
    const auto fill = StackRegion (doc, 10, 1); // 10 x 10 at +1, overlapping the cut's right half
    edit::Session session (doc);

    // Recipe order: the cut, then the fill over it. The fill owns the 5 x 10 overlap.
    const auto first = BuildStack (session.Doc (), cut);
    ExpectCurrentAndReconciled (first);
    ASSERT_EQ (StackOrder (first.quantities), (std::vector<edit::Id> { cut, fill }));
    const auto* cutRow = RowOf (first.quantities, cut);
    const auto* fillRow = RowOf (first.quantities, fill);
    ASSERT_TRUE (cutRow && fillRow);
    EXPECT_GE (fillRow->fill, 100 - 1e-6) << "its whole footprint at +1";
    EXPECT_GE (cutRow->cut, 50 - 1e-6) << "its uncovered half at -1";
    EXPECT_LT (cutRow->cut, 90) << "not the overlap the fill owns";

    // Move the fill under the cut: one undoable edit; the old publication is stale for the new
    // recipe; the rebuilt rows swap ownership of the overlap.
    ASSERT_TRUE (session.Apply ([fill] (edit::Document& d, std::string& e) {
        return geo::MoveRecipeRegion (d, fill, -1, e);
    })) << session.Message ();
    const auto movedRecipe = geo::ProjectLandscapeRecipe (session.Doc (), std::nullopt);
    EXPECT_FALSE (geo::ProjectRecipeQuantities (movedRecipe, first.outcome.published.get (), false).current);
    const auto moved = BuildStack (session.Doc (), cut);
    ExpectCurrentAndReconciled (moved);
    ASSERT_EQ (StackOrder (moved.quantities), (std::vector<edit::Id> { fill, cut }));
    EXPECT_GE (RowOf (moved.quantities, cut)->cut, 100 - 1e-6) << "the cut now owns the overlap";
    EXPECT_GE (RowOf (moved.quantities, fill)->fill, 50 - 1e-6);
    EXPECT_LT (RowOf (moved.quantities, fill)->fill, 90);
    EXPECT_GT (moved.quantities.total.cut, first.quantities.total.cut);
    EXPECT_LT (moved.quantities.total.fill, first.quantities.total.fill);

    // Effect None takes the cut out of the recipe and its row out of the quantities.
    auto settings = std::find_if (session.Doc ().landscape->objects.begin (), session.Doc ().landscape->objects.end (),
                                  [&] (const geo::HardscapeObject& o) { return o.id == cut; })
                        ->terrain;
    settings.effect = geo::RegionTerrainEffect::None;
    ASSERT_TRUE (session.Apply ([cut, settings] (edit::Document& d, std::string& e) {
        return geo::SetRegionTerrain (d, cut, settings, e);
    })) << session.Message ();
    const auto fillOnly = BuildStack (session.Doc (), cut);
    ExpectCurrentAndReconciled (fillOnly);
    EXPECT_EQ (StackOrder (fillOnly.quantities), (std::vector<edit::Id> { fill }));
    EXPECT_GE (RowOf (fillOnly.quantities, fill)->fill, 100 - 1e-6);

    // Undo walks back through both states and reproduces each one's quantities.
    ASSERT_TRUE (session.Undo ());
    ExpectSameQuantities (BuildStack (session.Doc (), cut).quantities, moved.quantities);
    ASSERT_TRUE (session.Undo ());
    ExpectSameQuantities (BuildStack (session.Doc (), cut).quantities, first.quantities);

    // Save and reload the moved state: the same recipe order and the same quantities.
    ASSERT_TRUE (session.Redo ());
    std::string text;
    ASSERT_TRUE (geo::WriteLandscape (session.Doc (), geo::LandscapeStorage::Virtual, text, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
    const auto reloaded = BuildStack (loaded, cut);
    ExpectCurrentAndReconciled (reloaded);
    ExpectSameQuantities (reloaded.quantities, moved.quantities);
}
