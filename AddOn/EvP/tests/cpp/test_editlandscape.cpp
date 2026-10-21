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

TEST (EditLandscape, OffLatticeBooleansKeepOneVerifiedSupportPerPiece)
{
    // Clipper snaps output vertices to its 1e-6 lattice. Drawn coordinates are
    // rarely on it; the snap must not cut supportless slivers at the corners.
    for (double o : { 1.0 / 3.0, 0.1234567891, 1000.0 + 1.0 / 7.0 }) {
        auto doc = Start ();
        const auto a = Square (doc, o, o), b = Square (doc, o + 4, o - 1, 2);
        Object (doc, Operation (doc, geo::OperationKind::Difference, { a, b }));
        geo::LandscapeResult result;
        ASSERT_TRUE (geo::EvaluateLandscape (doc, result)) << "offset " << o << ": " << result.error;
        ASSERT_EQ (result.regions.size (), 1u);
        EXPECT_NEAR (result.regions[0].area, 98, 1.0e-4) << o;
        EXPECT_EQ (result.regions[0].sources, (std::vector<edit::Id> { a, b }));
        size_t pieces = 0;
        for (const auto& ring : result.regions[0].boundary)
            for (const auto& piece : ring) {
                ++pieces;
                EXPECT_EQ (piece.support.size (), 1u) << o << " at " << piece.a.x << "," << piece.a.y;
            }
        EXPECT_EQ (pieces, 8u) << o;
    }
}

TEST (EditLandscape, ArcFaceBooleanRetainsArcSupport)
{
    // A semicircular D minus a centred strip: two caps, each bounded by part of
    // the one arc segment, part of the straight bottom and one strip side.
    constexpr double kPi = 3.14159265358979323846;
    auto doc = Start ();
    geo::LandscapeSource arc;
    arc.entityId = edit::AddPolyline (doc, { { 5, 0, 0 }, { -5, 0, 0 }, { 0, 0, 0 } }, true);
    arc.arcAngles = { kPi, 0, 0 };
    edit::Id d = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, arc, d, error)) << error;
    const auto strip = Source (doc, { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 9, 0 }, { -1, 9, 0 } });
    Object (doc, Operation (doc, geo::OperationKind::Difference, { d, strip }));
    geo::LandscapeResult result;
    ASSERT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    ASSERT_EQ (result.regions.size (), 2u);
    // Strip-covered part of the half disc: integral of sqrt(25 - x^2) over [-1, 1].
    const double removed = std::sqrt (24.0) + 25.0 * std::asin (0.2);
    EXPECT_NEAR (Area (result), 12.5 * kPi - removed, 0.02);
    const auto arcSegment = doc.entities[0].segments[0].id;
    for (const auto& region : result.regions) {
        EXPECT_EQ (region.sources, (std::vector<edit::Id> { d, strip }));
        bool onArc = false;
        for (const auto& piece : region.boundary[0]) {
            ASSERT_EQ (piece.support.size (), 1u);
            onArc = onArc || (piece.support[0].source == d && piece.support[0].segment == arcSegment);
        }
        EXPECT_TRUE (onArc);
    }
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
    const auto before = doc;
    doc.landscape->objects.front ().footprintOperation = difference;
    geo::LandscapeResult result;
    std::string error;
    ASSERT_TRUE (geo::RebuildLandscape (doc, result, error, &before)) << error;
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

TEST (EditLandscape, HigherPriorityMaterialOverridesLowerConflictsInEveryCatalogOrder)
{
    auto doc = Start ();
    Object (doc, Operation (doc, geo::OperationKind::RegionSelect, { Square (doc, 0, 0) }));
    const auto region = Evaluate (doc).regions.front ().regionId;
    for (int priority : { std::numeric_limits<int>::min (), -7, std::numeric_limits<int>::max () }) {
        doc.landscape->assignments.clear ();
        for (int i = 0; i < 3; ++i) {
            geo::MaterialAssignment assignment;
            assignment.id = edit::NewId (doc);
            assignment.region = region;
            assignment.priority = i == 2                                         ? priority
                                  : priority == std::numeric_limits<int>::min () ? priority
                                                                                 : priority - 1;
            assignment.material.preset = i == 0 ? geo::MaterialPreset::Grass : geo::MaterialPreset::Gravel;
            doc.landscape->assignments.push_back (assignment);
        }
        // At INT_MIN there is no lower rank; make the tied materials agree.
        if (priority == std::numeric_limits<int>::min ())
            doc.landscape->assignments[0].material = doc.landscape->assignments[2].material;
        const auto expected = doc.landscape->assignments[2].material;
        const auto original = doc.landscape->assignments;
        std::vector<int> order { 0, 1, 2 };
        do {
            for (size_t i = 0; i < order.size (); ++i)
                doc.landscape->assignments[i] = original[order[i]];
            const auto before = doc;
            geo::LandscapeResult result;
            ASSERT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
            ASSERT_EQ (result.regions.size (), 1u);
            EXPECT_EQ (result.regions[0].material, expected);
            EXPECT_EQ (result.regions[0].regionId, region);
            EXPECT_EQ (doc.landscape, before.landscape);
            EXPECT_EQ (doc.nextId, before.nextId);
        } while (std::next_permutation (order.begin (), order.end ()));
    }
}

TEST (EditLandscape, ConflictingHighestPriorityMaterialsRefuseAtomicallyInEveryCatalogOrder)
{
    auto doc = Start ();
    Object (doc, Operation (doc, geo::OperationKind::RegionSelect, { Square (doc, 0, 0) }));
    const auto region = Evaluate (doc).regions.front ().regionId;
    for (int i = 0; i < 3; ++i) {
        geo::MaterialAssignment assignment;
        assignment.id = edit::NewId (doc);
        assignment.region = region;
        assignment.priority = i == 0 ? -1 : 0;
        assignment.material.preset = i == 2 ? geo::MaterialPreset::Grass : geo::MaterialPreset::Gravel;
        doc.landscape->assignments.push_back (assignment);
    }
    const auto original = doc.landscape->assignments;
    std::vector<int> order { 0, 1, 2 };
    do {
        for (size_t i = 0; i < order.size (); ++i)
            doc.landscape->assignments[i] = original[order[i]];
        const auto before = doc;
        geo::LandscapeResult result;
        std::string error;
        EXPECT_FALSE (geo::RebuildLandscape (doc, result, error, &before));
        EXPECT_NE (error.find ("ProvenanceConflict"), std::string::npos);
        ASSERT_EQ (result.diagnostics.size (), 1u);
        EXPECT_EQ (result.diagnostics[0].code, geo::LandscapeDiagnosticCode::ProvenanceConflict);
        EXPECT_EQ (result.diagnostics[0].object, doc.landscape->objects[0].id);
        EXPECT_EQ (doc.landscape, before.landscape);
        EXPECT_EQ (doc.nextId, before.nextId);
        EXPECT_EQ (doc.revision, before.revision);
    } while (std::next_permutation (order.begin (), order.end ()));
}

TEST (EditLandscape, InvalidAndMaskedAssignmentsDoNotParticipateInWholeRegionPriority)
{
    auto doc = Start ();
    const auto source = Square (doc, 0, 0);
    const auto operation = Operation (doc, geo::OperationKind::RegionSelect, { source });
    Object (doc, operation);
    const auto region = Evaluate (doc).regions.front ().regionId;
    for (int i = 0; i < 3; ++i) {
        geo::MaterialAssignment assignment;
        assignment.id = edit::NewId (doc);
        assignment.region = region;
        assignment.priority = i == 0 ? -1 : std::numeric_limits<int>::max ();
        assignment.material.preset = i == 0 ? geo::MaterialPreset::Grass : geo::MaterialPreset::Gravel;
        assignment.valid = i != 1;
        assignment.mask = i == 2 ? operation : 0;
        doc.landscape->assignments.push_back (assignment);
    }
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].material.preset, geo::MaterialPreset::Grass);
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

TEST (EditLandscape, PropertyLineRefusalsAreNamedAndLeaveTheDocumentUntouched)
{
    constexpr double kPi = 3.14159265358979323846;
    auto doc = Start ();
    const auto open = Source (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 } }, false);
    const auto bowtie = Source (doc, { { 0, 0, 0 }, { 10, 10, 0 }, { 10, 0, 0 }, { 0, 10, 0 } });
    geo::LandscapeSource arc;
    arc.entityId = edit::AddPolyline (doc, { { 20, 0, 0 }, { 30, 0, 0 }, { 30, 10, 0 }, { 20, 10, 0 } }, true);
    arc.arcAngles = { kPi / 4, 0, 0, 0 };
    edit::Id arced = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, arc, arced, error)) << error;
    const auto untouched = doc;
    const std::vector<std::pair<edit::Id, std::string>> cases {
        { open, "PropertyLineOutside" },
        { bowtie, "PropertyLineOutside" },
        { arced, "PropertyLineArcUnsupported" },
        { 999999, "does not exist" },
    };
    for (const auto& [source, expected] : cases) {
        EXPECT_FALSE (geo::SetLandscapePropertyLine (doc, source, error)) << source;
        EXPECT_NE (error.find (expected), std::string::npos) << source << ": " << error;
        EXPECT_EQ (doc.landscape, untouched.landscape) << source;
        EXPECT_EQ (doc.nextId, untouched.nextId) << source;
        EXPECT_EQ (doc.revision, untouched.revision) << source;
    }
}

TEST (EditLandscape, PropertyLineLoopIsCarriedUnchangedThroughEditsInsideIt)
{
    auto doc = Start ();
    const auto line = Square (doc, 0, 0, 20);
    const auto inner = Square (doc, 5, 5, 5);
    std::string error;
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, line, error)) << error;
    Object (doc, Operation (doc, geo::OperationKind::RegionSelect, { inner }));
    const std::vector<edit::Vec3> loop { { 0, 0, 0 }, { 20, 0, 0 }, { 20, 20, 0 }, { 0, 20, 0 } };
    const auto initial = Evaluate (doc);
    EXPECT_EQ (initial.propertyLine, loop);
    EXPECT_NE (initial.propertyLineRevision, 0u);
    EXPECT_DOUBLE_EQ (initial.boundaryTolerance, 0.001);
    const auto innerEntity = doc.landscape->sources[1].entityId, lineEntity = doc.landscape->sources[0].entityId;
    edit::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, innerEntity, edit::Find (d, innerEntity)->vertices[2].id, { 12, 12, 0 }, e);
    })) << session.Message ();
    const auto edited = Evaluate (session.Doc ());
    EXPECT_NEAR (Area (edited), 25 + 2 * 0.5 * 5 * 2, 1.0e-6); // the dragged corner adds two triangles
    EXPECT_EQ (edited.propertyLine, loop);
    EXPECT_EQ (edited.propertyLineRevision, initial.propertyLineRevision);
    EXPECT_EQ (session.Doc ().landscape->propertyLineSourceId, line);
    // Editing the line itself changes the exposed loop and its revision.
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, lineEntity, edit::Find (d, lineEntity)->vertices[2].id, { 25, 25, 0 }, e);
    })) << session.Message ();
    const auto moved = Evaluate (session.Doc ());
    EXPECT_EQ (moved.propertyLine[2], (edit::Vec3 { 25, 25, 0 }));
    EXPECT_NE (moved.propertyLineRevision, initial.propertyLineRevision);
}

TEST (EditLandscape, PathStrokeCombinesWithAPlaygroundAndACircleCutter)
{
    constexpr double kPi = 3.14159265358979323846;
    auto doc = Start ();
    const auto path = edit::AddPath (doc, { { 0, 5, 0 }, { 30, 5, 0 } }, 2, "walk");
    geo::LandscapeSource centreline;
    centreline.pathId = path;
    centreline.role = geo::SourceRole::PathCenterline;
    edit::Id walk = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, centreline, walk, error)) << error;
    const auto stroke = Operation (doc, geo::OperationKind::Stroke, { walk });
    const auto playground = Square (doc, 10, 0);
    geo::LandscapeSource circle;
    circle.entityId = edit::AddPolyline (doc, { { 17, 5, 0 }, { 15, 7, 0 }, { 13, 5, 0 }, { 15, 3, 0 } }, true);
    circle.arcAngles.assign (4, kPi / 2);
    edit::Id cutter = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, circle, cutter, error)) << error;
    // A 30 x 2 walk crosses a 10 x 10 playground over 10 x 2.
    const std::vector<std::pair<geo::OperationKind, double>> expected {
        { geo::OperationKind::Union, 140 },
        { geo::OperationKind::Intersection, 20 },
        { geo::OperationKind::Xor, 120 },
        { geo::OperationKind::Difference, 40 },
    };
    for (const auto& [kind, area] : expected) {
        auto variant = doc;
        Object (variant, Operation (variant, kind, { stroke, playground }));
        const auto result = Evaluate (variant);
        EXPECT_NEAR (Area (result), area, 1.0e-6) << int (kind);
        for (const auto& region : result.regions) {
            EXPECT_FALSE (region.sources.empty ());
            EXPECT_TRUE (std::all_of (region.sources.begin (), region.sources.end (),
                                      [&] (edit::Id s) { return s == walk || s == playground; }));
        }
    }
    const auto joined = Operation (doc, geo::OperationKind::Union, { stroke, playground });
    Object (doc, Operation (doc, geo::OperationKind::Difference, { joined, cutter }));
    const auto result = Evaluate (doc);
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].rings.size (), 2u);
    EXPECT_NEAR (Area (result), 140 - 4 * kPi, 0.02);
    EXPECT_EQ (result.regions[0].sources, (std::vector<edit::Id> { walk, playground, cutter }));
}

namespace {
bool Has (const geo::LandscapeResult& result, geo::LandscapeDiagnosticCode code)
{
    return std::any_of (result.diagnostics.begin (), result.diagnostics.end (),
                        [=] (const auto& d) { return d.code == code; });
}
} // namespace

TEST (EditLandscape, ChangedFaceTopologyRefusesWithTheNormativeProvenanceDiagnostic)
{
    // The product selector reports AmbiguousFace; the kernel failure behind it is
    // ProvenanceUnattributable, reported alongside it rather than replaced by it.
    auto doc = Start ();
    const auto a = Square (doc, 0, 0), b = Square (doc, 5, 0);
    const auto faces = Evaluate (doc).faces;
    const auto overlap =
        std::find_if (faces.begin (), faces.end (), [] (const auto& f) { return f.seed.x > 5 && f.seed.x < 10; });
    ASSERT_NE (overlap, faces.end ());
    geo::LandscapeOperation select;
    select.kind = geo::OperationKind::RegionSelect;
    select.inputs = { a, b };
    select.faces = { overlap->descriptor };
    edit::Id id = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, select, id, error)) << error;
    Object (doc, id);
    const auto before = doc;
    const auto entity = doc.landscape->sources[1].entityId;
    for (auto& v : edit::Find (doc, entity)->vertices)
        v.p.x += 20; // the squares no longer overlap: the selected face is gone
    const auto edited = doc;
    geo::LandscapeResult result;
    EXPECT_FALSE (geo::RebuildLandscape (doc, result, error, &before));
    EXPECT_NE (error.find ("changed topology"), std::string::npos) << error;
    EXPECT_TRUE (Has (result, geo::LandscapeDiagnosticCode::AmbiguousFace));
    EXPECT_TRUE (Has (result, geo::LandscapeDiagnosticCode::ProvenanceUnattributable));
    EXPECT_EQ (doc.landscape, edited.landscape);
    EXPECT_EQ (doc.nextId, edited.nextId);
    geo::LandscapeResult evaluated;
    EXPECT_FALSE (geo::EvaluateLandscape (doc, evaluated));
    EXPECT_TRUE (Has (evaluated, geo::LandscapeDiagnosticCode::ProvenanceUnattributable));
}

TEST (EditLandscape, EquallyRankedConflictingAssignmentsRefuseWithProvenanceConflict)
{
    auto doc = Start ();
    Object (doc, Operation (doc, geo::OperationKind::RegionSelect, { Square (doc, 0, 0) }));
    const auto region = Evaluate (doc).regions[0].regionId;
    for (auto preset : { geo::MaterialPreset::Grass, geo::MaterialPreset::Gravel }) {
        geo::MaterialAssignment assignment;
        assignment.id = edit::NewId (doc);
        assignment.region = region;
        assignment.material.preset = preset;
        doc.landscape->assignments.push_back (assignment);
    }
    geo::LandscapeResult result;
    EXPECT_FALSE (geo::EvaluateLandscape (doc, result));
    EXPECT_NE (result.error.find ("ProvenanceConflict"), std::string::npos) << result.error;
    EXPECT_TRUE (Has (result, geo::LandscapeDiagnosticCode::ProvenanceConflict));
    EXPECT_TRUE (result.regions.empty ());
    // A higher priority is an explicit resolution, not a silent choice.
    doc.landscape->assignments[1].priority = 1;
    const auto resolved = Evaluate (doc);
    ASSERT_EQ (resolved.regions.size (), 1u);
    EXPECT_EQ (resolved.regions[0].material.preset, geo::MaterialPreset::Gravel);
}

TEST (EditLandscape, TerrainFootprintRefusesMalformedRingsBeforeClipping)
{
    auto doc = Start ();
    std::string error;
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, Square (doc, 1, 1, 1), error)) << error;
    const std::vector<edit::Vec3> square { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } };
    std::vector<std::vector<edit::Vec3>> invalid {
        {},
        { { 0, 0, 0 }, { 10, 0, 0 } },
        { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 } },
        // Both lobes contain area; a Boolean alone accepts this bowtie.
        { { 0, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 }, { 10, 0, 0 } },
        // A retraced spur does not change area, but is not a simple ring.
        { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 5, 10, 0 }, { 5, 12, 0 }, { 5, 10, 0 }, { 0, 10, 0 } }
    };
    for (double value : { std::numeric_limits<double>::quiet_NaN (), std::numeric_limits<double>::infinity (),
                          -std::numeric_limits<double>::infinity (), 1.0e10 }) {
        auto ring = square;
        ring[2].x = value;
        invalid.push_back (std::move (ring));
    }
    invalid.push_back (std::vector<edit::Vec3> (20001, { 0, 0, 0 }));
    const auto before = doc;
    for (size_t i = 0; i < invalid.size (); ++i) {
        SCOPED_TRACE (i);
        EXPECT_FALSE (geo::ValidateLandscapeTerrainEdit (doc, invalid[i], error));
        EXPECT_NE (error.find ("PropertyLineOutside"), std::string::npos) << error;
        EXPECT_EQ (doc.landscape, before.landscape);
        EXPECT_EQ (doc.nextId, before.nextId);
        EXPECT_EQ (doc.revision, before.revision);
    }
}

TEST (EditLandscape, TerrainFootprintChecksWholeConcaveBoundaryInEitherWinding)
{
    auto doc = Start ();
    std::string error;
    const auto property = Square (doc, 1, 1, 8);
    ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, property, error)) << error;
    std::vector<edit::Vec3> containing { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } };
    // Every property vertex is inside, but its top edge crosses the notch.
    std::vector<edit::Vec3> notched { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 6, 10, 0 },
                                      { 6, 5, 0 }, { 4, 5, 0 },  { 4, 10, 0 },  { 0, 10, 0 } };
    for (int winding = 0; winding < 2; ++winding) {
        EXPECT_TRUE (geo::ValidateLandscapeTerrainEdit (doc, containing, error)) << error;
        EXPECT_TRUE (error.empty ());
        EXPECT_FALSE (geo::ValidateLandscapeTerrainEdit (doc, notched, error));
        EXPECT_NE (error.find ("PropertyLineOutside"), std::string::npos);
        std::reverse (containing.begin (), containing.end ());
        std::reverse (notched.begin (), notched.end ());
    }
}

TEST (EditLandscape, PropertyLineRefusesRetracedSpursAtomically)
{
    auto doc = Start ();
    const auto property = Source (
        doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 5, 10, 0 }, { 5, 12, 0 }, { 5, 10, 0 }, { 0, 10, 0 } });
    const auto before = doc;
    std::string error;
    EXPECT_FALSE (geo::SetLandscapePropertyLine (doc, property, error));
    EXPECT_NE (error.find ("PropertyLineOutside"), std::string::npos) << error;
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.nextId, before.nextId);
    EXPECT_EQ (doc.revision, before.revision);
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

namespace {
bool Translate (edit::Document& doc, edit::Id entity, double dx)
{
    auto* found = edit::Find (doc, entity);
    if (!found)
        return false;
    for (auto& v : found->vertices)
        v.p.x += dx;
    return true;
}
const geo::DerivedRegion* RegionLeftOf (const geo::LandscapeResult& result, double x, bool left)
{
    for (const auto& region : result.regions)
        if ((region.seed.x < x) == left)
            return &region;
    return nullptr;
}
} // namespace

TEST (EditLandscape, DisconnectedSiblingSharingAuthoredSegmentsIsNotAPredecessor)
{
    // Both pieces of the field are bounded by the field's own bottom and top
    // segments. Sharing an authored segment is not evidence that area flowed.
    auto doc = Start ();
    const auto field = Source (doc, { { 0, 0, 0 }, { 20, 0, 0 }, { 20, 10, 0 }, { 0, 10, 0 } });
    const auto strip = Source (doc, { { 8, -1, 0 }, { 12, -1, 0 }, { 12, 11, 0 }, { 8, 11, 0 } });
    const auto notch = Square (doc, -10, 2, 2);
    const auto pieces = Operation (doc, geo::OperationKind::Difference, { field, strip });
    Object (doc, Operation (doc, geo::OperationKind::Difference, { pieces, notch }));
    const auto initial = Evaluate (doc);
    ASSERT_EQ (initial.regions.size (), 2u);
    const auto left = RegionLeftOf (initial, 10, true)->regionId, right = RegionLeftOf (initial, 10, false)->regionId;
    const auto notchEntity = doc.landscape->sources[2].entityId;
    edit::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string&) { return Translate (d, notchEntity, 13); }))
        << session.Message ();
    const auto holed = Evaluate (session.Doc ());
    ASSERT_EQ (holed.regions.size (), 2u);
    const auto* changed = RegionLeftOf (holed, 10, true);
    const auto* sibling = RegionLeftOf (holed, 10, false);
    EXPECT_NEAR (changed->area, 76, 1.0e-6);
    EXPECT_NE (changed->regionId, left);
    EXPECT_EQ (changed->predecessorRegions, (std::vector<edit::Id> { left }));
    EXPECT_EQ (sibling->regionId, right);
    // One edit changes both siblings: each descends only from its own footprint.
    const auto holedLeft = changed->regionId;
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string&) { return Translate (d, notchEntity, 10); }))
        << session.Message ();
    const auto moved = Evaluate (session.Doc ());
    ASSERT_EQ (moved.regions.size (), 2u);
    const auto* restored = RegionLeftOf (moved, 10, true);
    const auto* notched = RegionLeftOf (moved, 10, false);
    EXPECT_NEAR (restored->area, 80, 1.0e-6);
    EXPECT_NEAR (notched->area, 76, 1.0e-6);
    // The restored shape matches an inactive identity; history is not revived.
    EXPECT_NE (restored->regionId, left);
    EXPECT_EQ (restored->predecessorRegions, (std::vector<edit::Id> { holedLeft }));
    EXPECT_NE (notched->regionId, right);
    EXPECT_EQ (notched->predecessorRegions, (std::vector<edit::Id> { right }));
}

TEST (EditLandscape, ReshapedArcCarriesOnlyItsOwnFragmentsLineage)
{
    // A D whose single arc bounds both disconnected pieces over different
    // subdomains. One edit grows the arc and cuts a hole into the left piece.
    constexpr double kPi = 3.14159265358979323846;
    auto doc = Start ();
    geo::LandscapeSource arc;
    arc.entityId = edit::AddPolyline (doc, { { 5, 0, 0 }, { -5, 0, 0 }, { 0, 0, 0 } }, true);
    arc.arcAngles = { kPi, 0, 0 };
    edit::Id d = 0;
    std::string error;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, arc, d, error)) << error;
    const auto strip = Source (doc, { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 9, 0 }, { -1, 9, 0 } });
    const auto notch = Square (doc, -20, 1, 1);
    const auto pieces = Operation (doc, geo::OperationKind::Difference, { d, strip });
    Object (doc, Operation (doc, geo::OperationKind::Difference, { pieces, notch }));
    const auto initial = Evaluate (doc);
    ASSERT_EQ (initial.regions.size (), 2u);
    const auto left = RegionLeftOf (initial, 0, true)->regionId, right = RegionLeftOf (initial, 0, false)->regionId;
    const auto notchEntity = doc.landscape->sources[2].entityId;
    edit::Session session (doc);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& doc, std::string&) {
        doc.landscape->sources[0].arcAngles = { 1.2 * kPi, 0, 0 };
        return Translate (doc, notchEntity, 16);
    })) << session.Message ();
    const auto after = Evaluate (session.Doc ());
    ASSERT_EQ (after.regions.size (), 2u);
    const auto* holed = RegionLeftOf (after, 0, true);
    const auto* grown = RegionLeftOf (after, 0, false);
    EXPECT_EQ (holed->rings.size (), 2u);
    EXPECT_NE (holed->regionId, left);
    EXPECT_EQ (holed->predecessorRegions, (std::vector<edit::Id> { left }));
    EXPECT_EQ (grown->regionId, right);
    EXPECT_GT (grown->area, RegionLeftOf (initial, 0, false)->area);
}

TEST (EditLandscape, ReplacedFootprintDescendsByAreaNotBySharedSegments)
{
    auto doc = Start ();
    const auto first = Square (doc, 0, 0), overlapping = Square (doc, 5, 5), distant = Square (doc, 40, 0);
    const auto object = Object (doc, Operation (doc, geo::OperationKind::RegionSelect, { first }));
    const auto original = Evaluate (doc).regions.front ().regionId;
    const auto overlap = Operation (doc, geo::OperationKind::RegionSelect, { overlapping });
    const auto far = Operation (doc, geo::OperationKind::RegionSelect, { distant });
    std::string error;
    geo::LandscapeResult result;
    auto before = doc;
    doc.landscape->objects.front ().footprintOperation = overlap;
    ASSERT_TRUE (geo::RebuildLandscape (doc, result, error, &before)) << error;
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].objectId, object);
    EXPECT_EQ (result.regions[0].predecessorRegions, (std::vector<edit::Id> { original }));
    const auto replaced = result.regions[0].regionId;
    before = doc;
    doc.landscape->objects.front ().footprintOperation = far;
    ASSERT_TRUE (geo::RebuildLandscape (doc, result, error, &before)) << error;
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_NE (result.regions[0].regionId, replaced);
    EXPECT_TRUE (result.regions[0].predecessorRegions.empty ());
}

TEST (EditLandscape, LineageWithoutMatchingBeforeStateRefusesAtomically)
{
    auto doc = Start ();
    const auto a = Square (doc, 0, 0);
    const auto selected = Operation (doc, geo::OperationKind::RegionSelect, { a });
    Object (doc, selected);
    const auto previous = Evaluate (doc).regions.front ().regionId;
    const auto cutter = Source (doc, { { 4, -1, 0 }, { 6, -1, 0 }, { 6, 11, 0 }, { 4, 11, 0 } });
    const auto difference = Operation (doc, geo::OperationKind::Difference, { selected, cutter });
    const auto before = doc;
    doc.landscape->objects.front ().footprintOperation = difference;
    const auto edited = doc;
    geo::LandscapeResult result;
    std::string error;
    EXPECT_FALSE (geo::RebuildLandscape (doc, result, error));
    EXPECT_NE (error.find ("ProvenanceUnattributable"), std::string::npos) << error;
    EXPECT_EQ (doc.landscape, edited.landscape);
    EXPECT_EQ (doc.nextId, edited.nextId);
    // A before-state with another identity catalog is not this document's past.
    auto stranger = before;
    stranger.landscape->regions.front ().predecessors = { previous };
    EXPECT_FALSE (geo::RebuildLandscape (doc, result, error, &stranger));
    EXPECT_NE (error.find ("ProvenanceUnattributable"), std::string::npos) << error;
    EXPECT_EQ (doc.landscape, edited.landscape);
    EXPECT_EQ (doc.nextId, edited.nextId);
    ASSERT_TRUE (geo::RebuildLandscape (doc, result, error, &before)) << error;
    ASSERT_EQ (result.regions.size (), 2u);
    for (const auto& r : result.regions)
        EXPECT_EQ (r.predecessorRegions, (std::vector<edit::Id> { previous }));
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
            ASSERT_FALSE (piece.parameters.empty ()) << "a stroke side carries its centreline subdomain";
            for (const auto& p : piece.parameters)
                EXPECT_EQ (p.source, source);
            for (const auto& support : piece.support)
                EXPECT_EQ (support.source, source);
        }
    EXPECT_TRUE (horizontal);
    EXPECT_TRUE (vertical);
}
