#include "Edit/EditLandscape.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace {
std::array<uint8_t, 16> Uuid ()
{
    return { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
}
edit::Document Start ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, Uuid (), error)) << error;
    return doc;
}
edit::Id Source (edit::Document& doc, std::vector<edit::Vec3> points, bool closed = true)
{
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, points, closed);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    return id;
}
edit::Id Square (edit::Document& doc, double x, double y, double size = 10)
{
    return Source (doc, { { x, y, 0 }, { x + size, y, 0 }, { x + size, y + size, 0 }, { x, y + size, 0 } });
}
edit::Id Operation (edit::Document& doc, geo::OperationKind kind, std::vector<edit::Id> inputs)
{
    geo::LandscapeOperation op;
    op.kind = kind;
    op.inputs = std::move (inputs);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    return id;
}
edit::Id Object (edit::Document& doc, edit::Id op)
{
    geo::HardscapeObject object;
    object.footprintOperation = op;
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
    return id;
}
geo::LandscapeResult Evaluate (const edit::Document& doc)
{
    geo::LandscapeResult result;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    return result;
}
double Area (const geo::LandscapeResult& result)
{
    double sum = 0;
    for (const auto& r : result.regions)
        sum += r.area;
    return sum;
}
} // namespace

TEST (EditLandscape, OneDocumentOwnsIdsAndSessionUndo)
{
    edit::Session session;
    ASSERT_TRUE (
        session.Apply ([] (edit::Document& d, std::string& e) { return geo::StartLandscape (d, Uuid (), e); }));
    EXPECT_EQ (session.UndoDepth (), 1u);
    EXPECT_TRUE (session.Doc ().landscape.has_value ());
    ASSERT_TRUE (session.Undo ());
    EXPECT_FALSE (session.Doc ().landscape.has_value ());
    ASSERT_TRUE (session.Redo ());
    EXPECT_TRUE (session.Doc ().landscape.has_value ());
    std::string error;
    EXPECT_TRUE (edit::Validate (session.Doc (), error)) << error;
}

TEST (EditLandscape, BooleanAreasHolesAndOrderedDifference)
{
    for (auto kind : { geo::OperationKind::Union, geo::OperationKind::Difference, geo::OperationKind::Intersection,
                       geo::OperationKind::Xor }) {
        auto doc = Start ();
        const auto a = Square (doc, 0, 0), b = Square (doc, 5, 0);
        Object (doc, Operation (doc, kind, { a, b }));
        const double expected = kind == geo::OperationKind::Union ? 150 : kind == geo::OperationKind::Xor ? 100 : 50;
        const auto result = Evaluate (doc);
        EXPECT_NEAR (Area (result), expected, 1.0e-6);
        EXPECT_EQ (doc.landscape->sources.size (), 2u);
        for (const auto& r : result.regions) {
            EXPECT_NE (r.regionId, 0u);
            EXPECT_FALSE (r.operations.empty ());
            EXPECT_FALSE (r.boundary.empty ());
        }
    }
    auto doc = Start ();
    const auto outer = Square (doc, 0, 0), inner = Square (doc, 3, 3, 4);
    Object (doc, Operation (doc, geo::OperationKind::Difference, { outer, inner }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].rings.size (), 2u);
    EXPECT_NEAR (Area (result), 84, 1.0e-6);
    EXPECT_EQ (result.regions[0].sources, (std::vector<edit::Id> { outer, inner }));
}

TEST (EditLandscape, DisconnectedBooleanOutputsDoNotAcquireUnrelatedSources)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 30, 0);
    Object (doc, Operation (doc, geo::OperationKind::Union, { a, b }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 2u);
    EXPECT_EQ (result.regions[0].sources.size (), 1u);
    EXPECT_EQ (result.regions[1].sources.size (), 1u);
    EXPECT_NE (result.regions[0].sources, result.regions[1].sources);
}

TEST (EditLandscape, UnusedDistantCutterIsNotAContributor)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 30, 0);
    Object (doc, Operation (doc, geo::OperationKind::Difference, { a, b }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].sources, (std::vector<edit::Id> { a }));
}

TEST (EditLandscape, OpenGuidesAssembleFacesWithoutBecomingFilledSources)
{
    auto doc = Start ();
    const auto a = Source (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    Source (doc, { { 10, 0, 0 }, { 5, 5, 0 } }, false);
    Source (doc, { { 5, 5, 0 }, { 0, 0, 0 } }, false);
    auto result = Evaluate (doc);
    ASSERT_EQ (result.faces.size (), 1u);
    EXPECT_NEAR (result.faces[0].area, 25, 1.0e-9);
    geo::LandscapeOperation select;
    select.kind = geo::OperationKind::RegionSelect;
    select.inputs = { a };
    select.faces = { result.faces[0].descriptor };
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, select, id, error)) << error;
    Object (doc, id);
    EXPECT_NEAR (Area (Evaluate (doc)), 25, 1.0e-6);
}

TEST (EditLandscape, SourceMovePreservesSemanticRegionAndHistory)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0);
    const auto op = Operation (doc, geo::OperationKind::RegionSelect, { a });
    Object (doc, op);
    const auto before = Evaluate (doc);
    ASSERT_EQ (before.regions.size (), 1u);
    const auto source = doc.landscape->sources.front ();
    const auto* entity = edit::Find (doc, source.entityId);
    const auto vertex = entity->vertices[1].id;
    edit::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, source.entityId, vertex, { 12, 0, 0 }, e);
    }));
    const auto after = Evaluate (session.Doc ());
    ASSERT_EQ (after.regions.size (), 1u);
    EXPECT_NEAR (Area (after), 110, 1.0e-6);
    EXPECT_EQ (after.regions[0].regionId, before.regions[0].regionId);
    EXPECT_EQ (after.regions[0].operations, (std::vector<edit::Id> { op }));
    ASSERT_TRUE (session.Undo ());
    EXPECT_NEAR (Area (Evaluate (session.Doc ())), 100, 1.0e-6);
    ASSERT_TRUE (session.Redo ());
    EXPECT_NEAR (Area (Evaluate (session.Doc ())), 110, 1.0e-6);
}

TEST (EditLandscape, SplitAllocatesNewRegionsWithLineageAndInvalidatesAssignment)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0);
    const auto selected = Operation (doc, geo::OperationKind::RegionSelect, { a });
    const auto object = Object (doc, selected);
    const auto previous = Evaluate (doc).regions.front ().regionId;
    geo::MaterialAssignment assignment;
    assignment.id = edit::NewId (doc);
    assignment.region = previous;
    assignment.material.preset = geo::MaterialPreset::Grass;
    doc.landscape->assignments.push_back (assignment);
    const auto cutter = Source (doc, { { 4, -1, 0 }, { 6, -1, 0 }, { 6, 11, 0 }, { 4, 11, 0 } });
    const auto difference = Operation (doc, geo::OperationKind::Difference, { selected, cutter });
    doc.landscape->objects.front ().footprintOperation = difference;
    geo::LandscapeResult result;
    std::string error;
    ASSERT_TRUE (geo::RebuildLandscape (doc, result, error)) << error;
    ASSERT_EQ (result.regions.size (), 2u);
    for (const auto& r : result.regions) {
        EXPECT_NE (r.regionId, previous);
        EXPECT_EQ (r.objectId, object);
        EXPECT_EQ (r.predecessorRegions, (std::vector<edit::Id> { previous }));
    }
    EXPECT_FALSE (doc.landscape->assignments[0].valid);
    EXPECT_EQ (doc.landscape->assignments[0].region, previous);
}

TEST (EditLandscape, DisableAndDeleteRetainConstructionHistory)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 5, 0);
    const auto op = Operation (doc, geo::OperationKind::Difference, { a, b });
    Object (doc, op);
    std::string error;
    ASSERT_TRUE (geo::SetLandscapeOperationEnabled (doc, op, false, error)) << error;
    EXPECT_NEAR (Area (Evaluate (doc)), 100, 1.0e-6);
    ASSERT_TRUE (geo::SetLandscapeOperationEnabled (doc, op, true, error)) << error;
    EXPECT_NEAR (Area (Evaluate (doc)), 50, 1.0e-6);
    ASSERT_TRUE (geo::DeleteLandscapeSource (doc, b, error)) << error;
    const auto result = Evaluate (doc);
    EXPECT_TRUE (result.regions.empty ());
    EXPECT_EQ (doc.landscape->operations.size (), 1u);
    EXPECT_EQ (doc.landscape->sources.size (), 2u);
    EXPECT_TRUE (std::any_of (result.diagnostics.begin (), result.diagnostics.end (), [] (const auto& d) {
        return d.code == geo::LandscapeDiagnosticCode::OrphanedOperation;
    }));
}

TEST (EditLandscape, MaterialMaskIsAConstraintWhileDifferenceMakesAHole)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 3, 3, 4);
    Object (doc, Operation (doc, geo::OperationKind::MaterialMask, { a, b }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].rings.size (), 1u);
    EXPECT_NEAR (Area (result), 100, 1.0e-6);
}

TEST (EditLandscape, InvalidMutationLeavesDocumentAndAllocatorUntouched)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0);
    const auto before = doc;
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Difference;
    op.inputs = { a };
    edit::Id id = 123;
    std::string error;
    EXPECT_FALSE (geo::AddLandscapeOperation (doc, op, id, error));
    EXPECT_EQ (id, 123u);
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.nextId, before.nextId);
    EXPECT_EQ (doc.revision, before.revision);
}

TEST (EditLandscape, CyclesDuplicateIdsAndExhaustionRefuse)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0);
    const auto op = Operation (doc, geo::OperationKind::RegionSelect, { a });
    doc.landscape->operations[0].inputs = { op };
    std::string error;
    EXPECT_FALSE (geo::ValidateLandscape (doc, error));
    EXPECT_NE (error.find ("DagCycle"), std::string::npos);
    doc.landscape->operations[0].inputs = { a };
    doc.landscape->operations[0].id = a;
    EXPECT_FALSE (geo::ValidateLandscape (doc, error));
    doc = Start ();
    doc.nextId = std::numeric_limits<edit::Id>::max ();
    EXPECT_EQ (edit::NewId (doc), 0u);
    EXPECT_EQ (doc.nextId, std::numeric_limits<edit::Id>::max ());
}

TEST (EditLandscape, UnknownAndWrongDomainInputsRefuseButDeletedSourcesRemainOrphans)
{
    auto doc = Start ();
    const auto source = Square (doc, 0, 0);
    Operation (doc, geo::OperationKind::RegionSelect, { source });
    const auto before = doc;
    for (const auto input : { doc.nextId + 1, doc.entities[0].id, doc.entities[0].vertices[0].id }) {
        doc = before;
        doc.landscape->operations[0].inputs = { input };
        std::string error;
        EXPECT_FALSE (geo::ValidateLandscape (doc, error));
        EXPECT_NE (error.find ("operations.inputs"), std::string::npos) << error;
        geo::LandscapeResult result;
        EXPECT_FALSE (geo::EvaluateLandscape (doc, result));
    }
    doc = before;
    std::string error;
    ASSERT_TRUE (geo::DeleteLandscapeSource (doc, source, error)) << error;
    EXPECT_TRUE (geo::ValidateLandscape (doc, error)) << error;
    const auto result = Evaluate (doc);
    EXPECT_TRUE (std::any_of (result.diagnostics.begin (), result.diagnostics.end (), [] (const auto& d) {
        return d.code == geo::LandscapeDiagnosticCode::OrphanedOperation;
    }));
}

TEST (EditLandscape, PropertyLineRequiresClosedStraightSimpleGeometryAndContainsItsEdges)
{
    auto doc = Start ();
    const auto a = Square (doc, 1, 1, 8);
    std::string error;
    EXPECT_FALSE (geo::ValidateLandscapeTerrainEdit (doc, {}, error));
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, a, error)) << error;
    EXPECT_TRUE (
        geo::ValidateLandscapeTerrainEdit (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, error))
        << error;
    EXPECT_FALSE (
        geo::ValidateLandscapeTerrainEdit (doc, { { 0, 0, 0 }, { 5, 0, 0 }, { 5, 5, 0 }, { 0, 5, 0 } }, error));
    doc.landscape->sources[0].arcAngles = { 0.1, 0, 0, 0 };
    geo::LandscapeResult result;
    EXPECT_FALSE (geo::EvaluateLandscape (doc, result));
    EXPECT_NE (result.error.find ("PropertyLineArcUnsupported"), std::string::npos);
}

TEST (EditLandscape, ShuffledCatalogsKeepRegionIdsAndFullHistory)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 5, 0);
    const auto unionOp = Operation (doc, geo::OperationKind::Union, { a, b });
    const auto outer = Operation (doc, geo::OperationKind::RegionSelect, { unionOp });
    Object (doc, outer);
    const auto before = Evaluate (doc);
    std::reverse (doc.landscape->sources.begin (), doc.landscape->sources.end ());
    std::reverse (doc.landscape->operations.begin (), doc.landscape->operations.end ());
    const auto after = Evaluate (doc);
    ASSERT_EQ (after.regions.size (), before.regions.size ());
    EXPECT_EQ (after.regions[0].regionId, before.regions[0].regionId);
    EXPECT_EQ (after.regions[0].descriptor, before.regions[0].descriptor);
    EXPECT_EQ (after.regions[0].operations, (std::vector<edit::Id> { unionOp, outer }));
}

TEST (EditLandscape, OpenGuideSplitsAnAttributedSurfaceWithoutDeletingSources)
{
    auto doc = Start ();
    const auto surface = Square (doc, 0, 0);
    const auto guide = Source (doc, { { 4, -1, 0 }, { 4, 11, 0 } }, false);
    Object (doc, Operation (doc, geo::OperationKind::Split, { surface, guide }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 2u);
    std::vector<double> areas;
    for (const auto& r : result.regions) {
        areas.push_back (r.area);
        EXPECT_EQ (r.sources, (std::vector<edit::Id> { surface, guide }));
        EXPECT_TRUE (r.seed.x > 0 && r.seed.x < 10 && r.seed.y > 0 && r.seed.y < 10);
    }
    std::sort (areas.begin (), areas.end ());
    EXPECT_NEAR (areas[0], 40, 1.0e-6);
    EXPECT_NEAR (areas[1], 60, 1.0e-6);
    EXPECT_EQ (doc.landscape->sources.size (), 2u);
}

TEST (EditLandscape, SameSourceSetDoesNotPreserveRegionsAcrossSplitOrMerge)
{
    auto doc = Start ();
    const auto source = Square (doc, 0, 0);
    Object (doc, Operation (doc, geo::OperationKind::RegionSelect, { source }));
    const auto old = Evaluate (doc).regions[0].regionId;
    const auto entity = doc.entities[0].id, vertex = doc.entities[0].vertices[1].id;
    edit::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, entity, vertex, { 5, 15, 0 }, e);
    })) << session.Message ();
    const auto split = Evaluate (session.Doc ());
    ASSERT_EQ (split.regions.size (), 2u);
    std::vector<edit::Id> predecessors;
    for (const auto& region : split.regions) {
        EXPECT_NE (region.regionId, old);
        EXPECT_EQ (region.sources, (std::vector<edit::Id> { source }));
        EXPECT_EQ (region.predecessorRegions, (std::vector<edit::Id> { old }));
        predecessors.push_back (region.regionId);
    }
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, entity, vertex, { 10, 0, 0 }, e);
    })) << session.Message ();
    const auto merged = Evaluate (session.Doc ());
    ASSERT_EQ (merged.regions.size (), 1u);
    std::sort (predecessors.begin (), predecessors.end ());
    EXPECT_NE (merged.regions[0].regionId, old);
    EXPECT_EQ (merged.regions[0].predecessorRegions, predecessors);
}

TEST (EditLandscape, StrokeAndOffsetUseCurrentSourceGeometryAndReportCollapse)
{
    auto doc = Start ();
    const auto line = Source (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    const auto stroke = Operation (doc, geo::OperationKind::Stroke, { line });
    Object (doc, stroke);
    EXPECT_NEAR (Area (Evaluate (doc)), 15 + 3.141592653589793 * 0.75 * 0.75, 0.01);
    doc = Start ();
    const auto square = Square (doc, 0, 0);
    geo::LandscapeOperation offset;
    offset.kind = geo::OperationKind::Offset;
    offset.inputs = { square };
    offset.offset = -6;
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, offset, id, error)) << error;
    Object (doc, id);
    const auto result = Evaluate (doc);
    EXPECT_TRUE (result.regions.empty ());
    EXPECT_TRUE (std::any_of (result.diagnostics.begin (), result.diagnostics.end (),
                              [] (const auto& d) { return d.code == geo::LandscapeDiagnosticCode::OffsetCollapsed; }));
}

TEST (EditLandscape, PathStrokeUsesTheExistingAlignmentProfileAndSectionDefinition)
{
    auto doc = Start ();
    const auto path = edit::AddPath (doc, { { 20, 0, 0 }, { 30, 0, 1 } }, 2, "walk");
    geo::LandscapeSource source;
    source.pathId = path;
    source.role = geo::SourceRole::PathCenterline;
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    Object (doc, Operation (doc, geo::OperationKind::Stroke, { id }));
    EXPECT_NEAR (Area (Evaluate (doc)), 20, 1.0e-6);
}

TEST (EditLandscape, CancelAndUndoNeverReuseAllocatedLandscapeIdentities)
{
    edit::Session session (Start ());
    ASSERT_TRUE (session.Begin ());
    ASSERT_TRUE (session.Move ([] (edit::Document& d, std::string&) {
        Square (d, 0, 0);
        return true;
    }));
    const auto highWater = session.Doc ().nextId;
    session.Cancel ();
    EXPECT_EQ (session.Doc ().nextId, highWater);
    EXPECT_TRUE (session.Doc ().landscape->sources.empty ());
    ASSERT_TRUE (session.Apply ([] (edit::Document& d, std::string&) {
        Square (d, 0, 0);
        return true;
    }));
    EXPECT_GE (session.Doc ().landscape->sources[0].id, highWater);
}

TEST (EditLandscape, NestedBooleanIslandRetainsHoleOrientationAndActualSources)
{
    auto doc = Start ();
    const auto outer = Square (doc, 0, 0), hole = Square (doc, 2, 2, 6), island = Square (doc, 4, 4, 2);
    const auto annulus = Operation (doc, geo::OperationKind::Difference, { outer, hole });
    Object (doc, Operation (doc, geo::OperationKind::Union, { annulus, island }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 2u);
    EXPECT_NEAR (Area (result), 68, 1.0e-6);
    for (const auto& region : result.regions) {
        EXPECT_EQ (region.sources, region.rings.size () == 2 ? (std::vector<edit::Id> { outer, hole })
                                                             : (std::vector<edit::Id> { island }));
        for (size_t r = 0; r < region.rings.size (); ++r) {
            double twiceArea = 0;
            const auto& ring = region.rings[r];
            for (size_t i = 0; i < ring.size (); ++i)
                twiceArea += ring[i].x * ring[(i + 1) % ring.size ()].y - ring[(i + 1) % ring.size ()].x * ring[i].y;
            EXPECT_GT (r == 0 ? twiceArea : -twiceArea, 0);
        }
    }
}

TEST (EditLandscape, BooleanBoundarySubdomainsStayOnTheirAuthoredSegments)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 5, 0);
    Object (doc, Operation (doc, geo::OperationKind::Difference, { a, b }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            EXPECT_FALSE (piece.generated);
            ASSERT_FALSE (piece.parameters.empty ());
            for (const auto& parameter : piece.parameters) {
                const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                                  [&] (const auto& s) { return s.id == parameter.source; });
                ASSERT_NE (source, doc.landscape->sources.end ());
                const auto* entity = edit::Find (doc, source->entityId);
                ASSERT_LT (parameter.piece, entity->segments.size ());
                const auto start = entity->vertices[parameter.piece].p;
                const auto end = entity->vertices[(parameter.piece + 1) % entity->vertices.size ()].p;
                const auto p = edit::Lerp (start, end, parameter.sourceStart);
                const auto q = edit::Lerp (start, end, parameter.sourceEnd);
                EXPECT_NEAR (edit::DistanceXY (piece.a, parameter.reversed ? q : p), 0, 2.0e-6);
                EXPECT_NEAR (edit::DistanceXY (piece.b, parameter.reversed ? p : q), 0, 2.0e-6);
            }
        }
}

TEST (EditLandscape, BentStrokeBoundariesDoNotAcquireDistantSourceSegments)
{
    auto doc = Start ();
    const auto source = Source (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, false);
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Stroke;
    op.inputs = { source };
    op.width = 2;
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    Object (doc, id);
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    // Two width-2 rectangles overlap in one unit square. Exposed round parts
    // are the two end semicircles and one outside quarter-circle at the bend.
    EXPECT_NEAR (Area (result), 39 + 1.25 * 3.141592653589793, 0.015);
    const auto& segments = doc.entities[0].segments;
    bool horizontal = false, vertical = false;
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            const auto midpoint = edit::Lerp (piece.a, piece.b, 0.5);
            if (std::abs (midpoint.y + 1) < 1.0e-6 && midpoint.x > 1 && midpoint.x < 9) {
                horizontal = true;
                ASSERT_EQ (piece.support.size (), 1u);
                EXPECT_EQ (piece.support[0].segment, segments[0].id);
            }
            if (std::abs (midpoint.x - 11) < 1.0e-6 && midpoint.y > 1 && midpoint.y < 9) {
                vertical = true;
                ASSERT_EQ (piece.support.size (), 1u);
                EXPECT_EQ (piece.support[0].segment, segments[1].id);
            }
            EXPECT_TRUE (piece.generated);
            EXPECT_TRUE (piece.parameters.empty ());
            for (const auto& support : piece.support)
                EXPECT_EQ (support.source, source);
        }
    EXPECT_TRUE (horizontal);
    EXPECT_TRUE (vertical);
}
