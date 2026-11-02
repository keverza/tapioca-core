// #112 P2: the pure field-plan compiler. These tests pin the plan contract:
// deterministic repeat compiles, serialization-equivalent hashes, object-order
// independence, explicit-priority hash sensitivity, the h' math, configured
// Boolean composition, hole/island membership, path-after-region ordering,
// property-line transport and locked/mask coverage — and that compilation mutates
// nothing and the plan carries rings and stations, never triangles.
#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPolyline.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <optional>

namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;

namespace {
edit::Document Start ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    return doc;
}
edit::Id Source (edit::Document& doc, std::vector<edit::Vec3> points)
{
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, std::move (points), true);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    return id;
}
// A RegionBoundary square with its auto-owned configured region object.
edit::Id Region (edit::Document& doc, double x, double size = 10)
{
    const auto line =
        edit::AddPolyline (doc, { { x, 0, 0 }, { x + size, 0, 0 }, { x + size, size, 0 }, { x, size, 0 } }, true);
    std::string error;
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
    return doc.landscape->objects.back ().id;
}
void Configure (edit::Document& doc, edit::Id object, double z,
                geo::RegionTerrainEffect effect = geo::RegionTerrainEffect::Grade)
{
    geo::RegionSurface surface;
    surface.elevation = geo::RegionElevationMode::Constant;
    surface.first.z = z;
    geo::MaterialSpec material;
    material.preset = geo::MaterialPreset::HardPaving;
    material.fillGuid = "00112233-4455-6677-8899-AABBCCDDEEFF";
    material.surfaceGuid = "11223344-5566-7788-99AA-BBCCDDEEFF00";
    std::string error;
    ASSERT_TRUE (geo::SetRegionSurface (doc, object, surface, material, error)) << error;
    geo::RegionTerrain terrain;
    terrain.effect = effect;
    ASSERT_TRUE (geo::SetRegionTerrain (doc, object, terrain, error)) << error;
}
const geo::HardscapeObject& Object (const edit::Document& doc, edit::Id id)
{
    const auto object = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                      [=] (const auto& o) { return o.id == id; });
    EXPECT_NE (object, doc.landscape->objects.end ());
    return *object;
}
geo::LandscapeSnapshot Snapshot (const edit::Document& doc)
{
    geo::LandscapeSnapshot snapshot;
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
    snapshot.session = doc.landscape->sessionUuid;
    snapshot.sourceTerrainGuid = doc.landscape->sourceTerrainGuid;
    return snapshot;
}
geo::FieldPlan Compile (const edit::Document& doc, const std::optional<geo::PathDesignRequest>& path = std::nullopt)
{
    auto snapshot = Snapshot (doc);
    snapshot.path = path;
    return geo::CompileLandscapeFieldPlan (snapshot);
}
const geo::FieldPlanEntry* Entry (const geo::FieldPlan& plan, edit::Id object)
{
    const auto found = std::find_if (plan.entries.begin (), plan.entries.end (),
                                     [=] (const auto& entry) { return entry.object == object; });
    return found == plan.entries.end () ? nullptr : &*found;
}
// The rings-only plan shape: every part is an outer plus holes, never a
// triangulation.
void ExpectRingsOnly (const geo::FieldPlan& plan)
{
    for (const auto& entry : plan.entries) {
        ASSERT_FALSE (entry.parts.empty ());
        for (const auto& part : entry.parts) {
            ASSERT_FALSE (part.rings.empty ());
            for (const auto& ring : part.rings)
                EXPECT_GE (ring.size (), 3u) << "rings, never triangles";
        }
    }
}
edit::Id RootOf (const edit::Document& doc, edit::Id object)
{
    return Object (doc, object).footprintOperation;
}
void SetPriority (edit::Document& doc, edit::Id operation, int priority)
{
    for (auto& op : doc.landscape->operations)
        if (op.id == operation)
            op.priority = priority;
}
} // namespace

TEST (EditLandscapeFieldPlan, HeightCompositionMathIsAbsoluteTargetNotOffset)
{
    EXPECT_DOUBLE_EQ (geo::ComposeFieldHeight (geo::RegionTerrainEffect::None, 3, 5), 3);
    EXPECT_DOUBLE_EQ (geo::ComposeFieldHeight (geo::RegionTerrainEffect::Grade, 3, 5), 5);
    EXPECT_DOUBLE_EQ (geo::ComposeFieldHeight (geo::RegionTerrainEffect::Add, 3, 5), 5); // raise-only fill
    EXPECT_DOUBLE_EQ (geo::ComposeFieldHeight (geo::RegionTerrainEffect::Add, 6, 5), 6);
    EXPECT_DOUBLE_EQ (geo::ComposeFieldHeight (geo::RegionTerrainEffect::Subtract, 6, 5), 5); // lower-only cut
    EXPECT_DOUBLE_EQ (geo::ComposeFieldHeight (geo::RegionTerrainEffect::Subtract, 3, 5), 3);
}

TEST (EditLandscapeFieldPlan, RepeatCompileIsDeterministicAndCarriesRingsNotTriangles)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner, 2);
    const auto hole = Source (doc, { { 3, 3, 0 }, { 7, 3, 0 }, { 7, 7, 0 }, { 3, 7, 0 } });
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Difference;
    command.owner = owner;
    command.subject.input = RootOf (doc, owner);
    command.operand.input = hole;
    ASSERT_TRUE (geo::ApplyDomainCommand (doc, command).ok);

    const auto first = Compile (doc);
    const auto second = Compile (doc);
    ASSERT_TRUE (first.ok) << first.error;
    EXPECT_TRUE (second.ok) << second.error;
    EXPECT_EQ (first, second);
    EXPECT_NE (first.hash, 0u);
    ASSERT_EQ (first.entries.size (), 1u);
    EXPECT_EQ (first.entries[0].parts[0].rings.size (), 2u); // the hole is a ring, not a part
    ExpectRingsOnly (first);
    EXPECT_TRUE (first.diagnostics.empty ());
}

TEST (EditLandscapeFieldPlan, SerializationEquivalentSnapshotCompilesToTheSameHash)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner, 2);
    const auto plan = Compile (doc);

    std::string json, error;
    ASSERT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, json, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (json, loaded, error)) << error;
    ASSERT_TRUE (loaded.landscape);
    loaded.revision = doc.revision; // same capture stamp, as a reloaded session would carry
    const auto roundTrip = Compile (loaded);
    ASSERT_TRUE (roundTrip.ok) << roundTrip.error;
    EXPECT_EQ (roundTrip.hash, plan.hash);
    EXPECT_EQ (roundTrip.entries, plan.entries);
    EXPECT_EQ (roundTrip.masks, plan.masks);
    EXPECT_EQ (roundTrip.controls, plan.controls);
}

TEST (EditLandscapeFieldPlan, ReorderingTheObjectVectorDoesNotChangeThePlan)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 10);
    Configure (doc, a, 2);
    Configure (doc, b, 5);
    const auto before = Compile (doc);
    ASSERT_TRUE (before.ok) << before.error;

    std::reverse (doc.landscape->objects.begin (), doc.landscape->objects.end ());
    const auto after = Compile (doc);
    ASSERT_TRUE (after.ok) << after.error;
    EXPECT_EQ (after.hash, before.hash);
    EXPECT_EQ (after.entries, before.entries);
}

TEST (EditLandscapeFieldPlan, ExplicitPriorityChangesOrderAndHash)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5); // overlaps a
    Configure (doc, a, 2);
    Configure (doc, b, 5);
    const auto rootB = RootOf (doc, b);

    const auto unordered = Compile (doc);
    ASSERT_TRUE (unordered.ok) << unordered.error;
    ASSERT_EQ (unordered.entries.size (), 2u);
    // Default order: equal priority, topological DAG order, object id tie-break.
    EXPECT_EQ (Entry (unordered, a)->order, 0u);
    EXPECT_EQ (Entry (unordered, b)->order, 1u);

    SetPriority (doc, rootB, -1); // B now stacks before A
    const auto reordered = Compile (doc);
    ASSERT_TRUE (reordered.ok) << reordered.error;
    EXPECT_EQ (Entry (reordered, b)->order, 0u);
    EXPECT_EQ (Entry (reordered, a)->order, 1u);
    EXPECT_NE (reordered.hash, unordered.hash);

    SetPriority (doc, rootB, 3); // same order as default, still a different explicit plan
    const auto reprioritized = Compile (doc);
    ASSERT_TRUE (reprioritized.ok) << reprioritized.error;
    EXPECT_EQ (Entry (reprioritized, a)->order, 0u);
    EXPECT_EQ (Entry (reprioritized, b)->order, 1u);
    EXPECT_NE (reprioritized.hash, unordered.hash);
}

TEST (EditLandscapeFieldPlan, ConfiguredBooleanCompositionFollowsOperandsAndKeepsSubjectMetadata)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 5); // overlaps a
    Configure (doc, a, 2);
    Configure (doc, b, 5);
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Union;
    command.name = "Union";
    // Adjacent RegionBoundary squares share an edge, so a picked face cites
    // both configured sources and the fresh-face subject refuses (P1 rule).
    // The saved-set operand form names the subject's root directly instead.
    command.subject.input = RootOf (doc, a);
    command.operand.input = RootOf (doc, b);
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;

    const auto plan = Compile (doc);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.entries.size (), 3u); // A, B and the derived result each contribute one entry
    const auto* output = Entry (plan, applied.objectId);
    ASSERT_NE (output, nullptr);
    // A's metadata wins; the operand never contributes its own.
    ASSERT_TRUE (output->surface.has_value ());
    EXPECT_DOUBLE_EQ (output->surface->first.z, 2);
    EXPECT_EQ (output->terrain.effect, geo::RegionTerrainEffect::Grade);
    // The derived result follows its operands: it stacks after A and B, so it
    // wins in its own output domain and B is never applied twice over it.
    EXPECT_GT (output->order, Entry (plan, a)->order);
    EXPECT_GT (output->order, Entry (plan, b)->order);
    ASSERT_TRUE (Entry (plan, b)->surface.has_value ());
    EXPECT_DOUBLE_EQ (Entry (plan, b)->surface->first.z, 5); // B keeps its own domain
}

TEST (EditLandscapeFieldPlan, HoleKeepsMembershipOutOfTheSurroundingEntry)
{
    auto doc = Start ();
    const auto holed = Region (doc, 0);
    Configure (doc, holed, 2);
    const auto hole = Source (doc, { { 3, 3, 0 }, { 7, 3, 0 }, { 7, 7, 0 }, { 3, 7, 0 } });
    geo::DomainCommand punch;
    punch.kind = geo::OperationKind::Difference;
    punch.owner = holed;
    punch.subject.input = RootOf (doc, holed);
    punch.operand.input = hole;
    const auto punched = geo::ApplyDomainCommand (doc, punch);
    ASSERT_TRUE (punched.ok) << punched.error;

    const auto plan = Compile (doc);
    ASSERT_TRUE (plan.ok) << plan.error;
    const auto* holeEntry = Entry (plan, holed);
    ASSERT_NE (holeEntry, nullptr);
    ASSERT_EQ (holeEntry->parts.size (), 1u);
    ASSERT_EQ (holeEntry->parts[0].rings.size (), 2u); // outer + hole: the hole is not a member part
}

TEST (EditLandscapeFieldPlan, IslandsSplitIntoSeparateMembershipParts)
{
    auto doc = Start ();
    const auto islands = Region (doc, 20);
    Configure (doc, islands, 3);
    const auto bar = Source (doc, { { 24, -1, 0 }, { 26, -1, 0 }, { 26, 11, 0 }, { 24, 11, 0 } });
    geo::DomainCommand split;
    split.kind = geo::OperationKind::Difference;
    split.owner = islands;
    split.subject.input = RootOf (doc, islands);
    split.operand.input = bar;
    const auto applied = geo::ApplyDomainCommand (doc, split);
    ASSERT_TRUE (applied.ok) << applied.error;

    const auto plan = Compile (doc);
    ASSERT_TRUE (plan.ok) << plan.error;
    const auto* islandEntry = Entry (plan, islands);
    ASSERT_NE (islandEntry, nullptr);
    EXPECT_EQ (islandEntry->parts.size (), 2u); // two disjoint islands, one ring each
    for (const auto& part : islandEntry->parts)
        EXPECT_EQ (part.rings.size (), 1u);
}

TEST (EditLandscapeFieldPlan, PathBandFollowsEveryRegionEntry)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0, 20);
    Configure (doc, owner, 2);
    const auto path = edit::AddPath (doc, { { 0, 0, 10 }, { 40, 0, 12 } }, 4.0);
    geo::LandscapeSource centreline;
    centreline.pathId = path;
    centreline.role = geo::SourceRole::PathCenterline;
    std::string error;
    edit::Id source = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, centreline, source, error)) << error;
    geo::LandscapeOperation stroke;
    stroke.kind = geo::OperationKind::Stroke;
    stroke.inputs = { source };
    stroke.width = 4;
    edit::Id operation = 0;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, stroke, operation, error)) << error;

    geo::PathDesignRequest request;
    request.source = source;
    request.operation = operation;
    const auto plan = Compile (doc, request);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.entries.size (), 1u);
    ASSERT_EQ (plan.paths.size (), 1u);
    // Regions first, the path after: the current intended composition order.
    EXPECT_EQ (plan.entries[0].order, 0u);
    EXPECT_EQ (plan.paths[0].order, 1u);
    EXPECT_GT (plan.paths[0].order, plan.entries.back ().order);
    // Stations are the existing C++ preparation: 0..40 every 2 m on the profile.
    const auto& stations = plan.paths[0].stations;
    ASSERT_EQ (stations.size (), 21u);
    for (size_t i = 1; i < stations.size (); ++i)
        EXPECT_GT (stations[i].station, stations[i - 1].station);
    EXPECT_DOUBLE_EQ (stations.front ().centre.z, 10);
    EXPECT_DOUBLE_EQ (stations.back ().centre.z, 12);
    EXPECT_NEAR (stations[10].centre.z, 11, 1e-9); // the profile is linear
    // No path request: a region-only plan.
    EXPECT_TRUE (Compile (doc).paths.empty ());
}

TEST (EditLandscapeFieldPlan, PropertyLineIsCompiledAndChangesThePlanHash)
{
    auto doc = Start ();
    std::string error;
    const auto first = Source (doc, { { -10, -10, 0 }, { 30, -10, 0 }, { 30, 30, 0 }, { -10, 30, 0 } });
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, first, error)) << error;
    const auto before = Compile (doc);
    ASSERT_TRUE (before.ok) << before.error;
    ASSERT_EQ (before.propertyLine.size (), 4u);
    EXPECT_EQ (before.propertyLineSource, first);
    EXPECT_NE (before.propertyLineRevision, 0u);

    const auto second = Source (doc, { { -20, -20, 0 }, { 40, -20, 0 }, { 40, 40, 0 }, { -20, 40, 0 } });
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, second, error)) << error;
    const auto after = Compile (doc);
    ASSERT_TRUE (after.ok) << after.error;
    EXPECT_EQ (after.propertyLineSource, second);
    EXPECT_NE (after.propertyLine, before.propertyLine);
    EXPECT_NE (after.hash, before.hash);
}

TEST (EditLandscapeFieldPlan, SurfaceTargetsTransportNonConstantPlanesForTheUnifiedBuild)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    geo::RegionSurface surface;
    surface.elevation = geo::RegionElevationMode::Constant;
    surface.slope = geo::RegionSlopeMode::TwoPoints;
    surface.first = { 0, 0, 2 };
    surface.second = { 10, 0, 4 };
    geo::MaterialSpec material;
    std::string error;
    ASSERT_TRUE (geo::SetRegionSurface (doc, owner, surface, material, error)) << error;
    geo::RegionTerrain terrain;
    terrain.effect = geo::RegionTerrainEffect::Grade;
    ASSERT_TRUE (geo::SetRegionTerrain (doc, owner, terrain, error)) << error;

    const auto plan = Compile (doc);
    ASSERT_TRUE (plan.ok) << plan.error;
    const auto* entry = Entry (plan, owner);
    ASSERT_NE (entry, nullptr);
    ASSERT_TRUE (entry->surfacePoints.size () % 3 == 0);
    ASSERT_FALSE (entry->surfaceTriangles.empty ());
    ASSERT_TRUE (entry->surfaceTriangles.size () % 3 == 0);
    for (size_t i = 0; i + 2 < entry->surfacePoints.size (); i += 3) {
        const double expected = 2.0 + entry->surfacePoints[i] * 0.2;
        EXPECT_NEAR (entry->surfacePoints[i + 2], expected, 1e-9);
    }
}

TEST (EditLandscapeFieldPlan, PathStationsIncludeRangeTaperSamplesAndPreparedCrossfall)
{
    auto doc = Start ();
    const auto pathId = edit::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 1 } }, 4.0);
    auto* path = edit::FindPath (doc, pathId);
    ASSERT_NE (path, nullptr);
    edit::AlignmentGeometry geometry;
    std::string error;
    ASSERT_TRUE (edit::ValidatePath (*path, geometry, error)) << error;
    const auto rangeId = edit::NewId (doc);
    ASSERT_TRUE (edit::SplitSection (path->sections, geometry.length, 10, rangeId, error)) << error;

    geo::LandscapeSource centreline;
    centreline.pathId = pathId;
    centreline.role = geo::SourceRole::PathCenterline;
    edit::Id source = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, centreline, source, error)) << error;
    geo::LandscapeOperation stroke;
    stroke.kind = geo::OperationKind::Stroke;
    stroke.inputs = { source };
    edit::Id operation = 0;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, stroke, operation, error)) << error;

    geo::PathDesignRequest request;
    request.source = source;
    request.operation = operation;
    request.maxStep = 5;
    request.taper = 0.5;
    const auto plan = Compile (doc, request);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.paths.size (), 1u);
    const auto& stations = plan.paths[0].stations;
    // 0, 5, 9.5, 10, 15, 20: the range boundary and the pre-taper sample are distinct.
    ASSERT_EQ (stations.size (), 6u);
    ASSERT_EQ (plan.paths[0].crossfallPercent.size (), stations.size ());
    EXPECT_NEAR (stations[2].station, 9.5, 1e-12);
    EXPECT_NEAR (stations[3].station, 10.0, 1e-12);
    // Same-revision content sensitivity: rounding changes the derived mesh even
    // when a test edits the authored value without publishing another revision.
    path = edit::FindPath (doc, pathId); // Published document values may have moved since setup.
    ASSERT_NE (path, nullptr);
    path->sections.front ().rounding += 0.25;
    const auto rounded = Compile (doc, request);
    ASSERT_TRUE (rounded.ok) << rounded.error;
    EXPECT_NE (rounded.hash, plan.hash);
    EXPECT_NE (rounded.paths.front ().stations.front ().section.rounding,
               plan.paths.front ().stations.front ().section.rounding);
}

TEST (EditLandscapeFieldPlan, LockedControlsAndMasksAreCompiledWithCoverage)
{
    auto doc = Start ();
    std::string error;
    // A property line so terrain masks are bounded, then a configured region.
    const auto propertySource = Source (doc, { { -10, -10, 0 }, { 30, -10, 0 }, { 30, 30, 0 }, { -10, 30, 0 } });
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, propertySource, error)) << error;
    const auto owner = Region (doc, 0);
    Configure (doc, owner, 2);
    // A grading limit reaching the region: the mask square overlaps the region square.
    const auto mask = Source (doc, { { 5, 5, 0 }, { 15, 5, 0 }, { 15, 15, 0 }, { 5, 15, 0 } });
    geo::LandscapeOperation limit;
    limit.kind = geo::OperationKind::GradingLimit;
    limit.inputs = { RootOf (doc, owner), mask };
    edit::Id limitId = 0;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, limit, limitId, error)) << error;
    // Locked controls on an open straight guide: one whole line, one vertex,
    // and one enable-then-disable (disabled records persist but never compile).
    const auto guide = edit::AddPolyline (doc, { { 0, -5, 1 }, { 10, -5, 1 }, { 20, -5, 1 } }, false);
    geo::LandscapeSource guideSource;
    guideSource.entityId = guide;
    edit::Id guideId = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, guideSource, guideId, error)) << error;
    const std::array<edit::Id, 3> vertexIds { edit::Find (doc, guide)->vertices[0].id,
                                              edit::Find (doc, guide)->vertices[1].id,
                                              edit::Find (doc, guide)->vertices[2].id };
    ASSERT_TRUE (geo::SetLandscapeTerrainControl (doc, guideId, 0, true, error)) << error; // whole line
    ASSERT_TRUE (geo::SetLandscapeTerrainControl (doc, guideId, vertexIds[1], true, error)) << error;
    ASSERT_TRUE (geo::SetLandscapeTerrainControl (doc, guideId, vertexIds[2], true, error)) << error;
    ASSERT_TRUE (geo::SetLandscapeTerrainControl (doc, guideId, vertexIds[2], false, error)) << error;

    const auto plan = Compile (doc);
    ASSERT_TRUE (plan.ok) << plan.error;
    ASSERT_EQ (plan.propertyLine.size (), 4u);
    EXPECT_EQ (plan.propertyLineSource, propertySource);
    EXPECT_NE (plan.propertyLineRevision, 0u);
    ASSERT_EQ (plan.masks.size (), 1u);
    EXPECT_EQ (plan.masks[0].kind, geo::OperationKind::GradingLimit);
    EXPECT_EQ (plan.masks[0].operation, limitId);
    ASSERT_FALSE (plan.masks[0].parts.empty ());
    EXPECT_TRUE (std::find (plan.masks[0].sources.begin (), plan.masks[0].sources.end (), mask) !=
                 plan.masks[0].sources.end ());
    // The disabled control is not compiled; the line and the point are.
    ASSERT_EQ (plan.controls.size (), 2u);
    const auto line =
        std::find_if (plan.controls.begin (), plan.controls.end (), [] (const auto& c) { return !c.point; });
    const auto point =
        std::find_if (plan.controls.begin (), plan.controls.end (), [] (const auto& c) { return c.point; });
    ASSERT_NE (line, plan.controls.end ());
    ASSERT_NE (point, plan.controls.end ());
    ASSERT_EQ (line->line.size (), 3u);
    EXPECT_EQ (line->segments.size (), 2u);
    EXPECT_DOUBLE_EQ (line->line[0].z, 1);
    EXPECT_EQ (point->source, guideId);
    EXPECT_EQ (point->vertex, vertexIds[1]);
    EXPECT_DOUBLE_EQ (point->pointPosition.x, 10);
}

TEST (EditLandscapeFieldPlan, CompilationMutatesNothing)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner, 2);
    const auto guide = edit::AddPolyline (doc, { { 0, -5, 1 }, { 10, -5, 1 } }, false);
    geo::LandscapeSource guideSource;
    guideSource.entityId = guide;
    edit::Id guideId = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, guideSource, guideId, error)) << error;
    ASSERT_TRUE (geo::SetLandscapeTerrainControl (doc, guideId, 0, true, error)) << error;

    const auto before = doc;
    const auto plan = Compile (doc);
    ASSERT_TRUE (plan.ok) << plan.error;
    EXPECT_EQ (doc.entities, before.entities);
    EXPECT_EQ (doc.paths, before.paths);
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.layers, before.layers);
    EXPECT_EQ (doc.groups, before.groups);
    EXPECT_EQ (doc.isolated, before.isolated);
    EXPECT_EQ (doc.nextId, before.nextId);
    EXPECT_EQ (doc.revision, before.revision);
}

TEST (EditLandscapeFieldPlan, MissingSnapshotRefusesAndUnresolvedPathDiagnoses)
{
    const auto empty = geo::CompileLandscapeFieldPlan (geo::LandscapeSnapshot {});
    EXPECT_FALSE (empty.ok);
    ASSERT_EQ (empty.diagnostics.size (), 1u);
    EXPECT_EQ (empty.diagnostics[0].stage, geo::FieldPlanStage::Snapshot);

    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner, 2);
    // A design request naming no live centerline: the region stack still
    // compiles and the path problem is a located diagnostic, not a refusal.
    geo::PathDesignRequest request;
    request.source = doc.nextId + 10;
    const auto plan = Compile (doc, request);
    ASSERT_TRUE (plan.ok) << plan.error;
    EXPECT_TRUE (plan.paths.empty ());
    ASSERT_EQ (plan.diagnostics.size (), 1u);
    EXPECT_EQ (plan.diagnostics[0].stage, geo::FieldPlanStage::Path);
    EXPECT_EQ (plan.diagnostics[0].code, geo::FieldPlanDiagnosticCode::PathUnresolved);
    EXPECT_EQ (plan.diagnostics[0].source, request.source);
    std::string buildError;
    EXPECT_FALSE (geo::ValidateLandscapeFieldPlanForBuild (plan, buildError));
    EXPECT_NE (buildError.find ("FieldPlanIncomplete"), std::string::npos);
}
