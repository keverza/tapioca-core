#include "ArchViz/HudVirtualEditor.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace edit = geomsrv::edit;
namespace hud = geomsrv::archviz::edithud;
namespace geo = tapioca::geometry;
namespace {
edit::Document Demo ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::SeedLandscapeDemo (doc, 0, error)) << error;
    return doc;
}
double Area (hud::Session& s)
{
    double area = 0;
    for (const auto& region : hud::Landscape (s).regions)
        area += region.area;
    return area;
}
void Pick (hud::Session& s, const std::vector<size_t>& indices)
{
    ASSERT_TRUE (hud::SetLandscapeMode (s, hud::LandscapeMode::Face));
    const auto faces = hud::Landscape (s).faces;
    for (auto index : indices) {
        ASSERT_LT (index, faces.size ());
        ASSERT_TRUE (hud::PickLandscapeFace (s, faces[index].seed, true, true));
    }
}
const geo::LandscapeOperation& Set (const hud::Session& s, edit::Id id)
{
    const auto& ops = s.edits.Doc ().landscape->operations;
    const auto op = std::find_if (ops.begin (), ops.end (), [=] (const auto& o) { return o.id == id; });
    EXPECT_NE (op, ops.end ());
    return *op;
}
} // namespace

TEST (HudLandscapeSets, EmptySetsAreAuthoredOperationsNotOutputObjectsOrASecondSelectionStore)
{
    hud::Session s (Demo ());
    ASSERT_TRUE (hud::CreateLandscapeFaceSet (s)) << s.message;
    const auto id = s.landscapeSet;
    EXPECT_NE (id, 0u);
    EXPECT_TRUE (Set (s, id).faces.empty ());
    EXPECT_TRUE (Set (s, id).inputs.empty ());
    EXPECT_TRUE (s.edits.Doc ().landscape->objects.empty ());
    EXPECT_TRUE (hud::Landscape (s).regions.empty ());
    EXPECT_TRUE (hud::Landscape (s).diagnostics.empty ());
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    ASSERT_TRUE (s.edits.Undo ());
    hud::Landscape (s);
    EXPECT_EQ (s.landscapeSet, 0u);
    ASSERT_TRUE (s.edits.Redo ());
    EXPECT_TRUE (hud::IsLandscapeFaceSet (Set (s, id)));
    std::string definition, error;
    ASSERT_TRUE (geo::WriteLandscape (s.edits.Doc (), geo::LandscapeStorage::Virtual, definition, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (definition, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, s.edits.Doc ().landscape);
}

TEST (HudLandscapeSets, FaceMembershipCanBeAddedRecalledAndRemovedToAnActuallyEmptySet)
{
    hud::Session s (Demo ());
    ASSERT_TRUE (hud::CreateLandscapeFaceSet (s));
    const auto id = s.landscapeSet;
    Pick (s, { 0, 1 });
    ASSERT_TRUE (hud::EditLandscapeFaceSet (s)) << s.message;
    EXPECT_EQ (Set (s, id).faces.size (), 2u);
    EXPECT_TRUE (s.selection.Items ().empty ());
    EXPECT_TRUE (s.edits.Doc ().landscape->objects.empty ());
    const auto depth = s.edits.UndoDepth ();
    ASSERT_TRUE (hud::SelectLandscapeFaceSet (s));
    ASSERT_EQ (s.selection.Items ().size (), 2u);
    for (const auto& ref : s.selection.Items ()) {
        EXPECT_EQ (ref.part, edit::Part::LandscapeFace);
        EXPECT_EQ (ref.revision, s.edits.Doc ().revision);
    }
    EXPECT_EQ (s.edits.UndoDepth (), depth);
    ASSERT_TRUE (hud::EditLandscapeFaceSet (s, true)) << s.message;
    EXPECT_TRUE (Set (s, id).faces.empty ());
    EXPECT_TRUE (Set (s, id).inputs.empty ());
    EXPECT_NEAR (Area (s), 0, 1.0e-9);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (Set (s, id).faces.size (), 2u);
    ASSERT_TRUE (s.edits.Redo ());
    EXPECT_TRUE (Set (s, id).faces.empty ());
    Pick (s, { 1 });
    ASSERT_TRUE (hud::EditLandscapeFaceSet (s)) << s.message;
    EXPECT_EQ (Set (s, id).faces.size (), 1u);
}

TEST (HudLandscapeSets, GlobalBooleanUsesSavedAAndBNotCurrentPicksAndLeavesBothSetsIntact)
{
    hud::Session s (Demo ());
    const auto faces = hud::Landscape (s).faces;
    ASSERT_GT (faces.size (), 2u);
    ASSERT_TRUE (hud::CreateLandscapeFaceSet (s));
    const auto a = s.landscapeSet;
    Pick (s, { 0, 1 });
    ASSERT_TRUE (hud::EditLandscapeFaceSet (s));
    ASSERT_TRUE (hud::CreateLandscapeFaceSet (s));
    const auto b = s.landscapeSet;
    Pick (s, { 1 });
    ASSERT_TRUE (hud::EditLandscapeFaceSet (s));
    const auto setA = Set (s, a), setB = Set (s, b);
    s.landscapeSet = a;
    s.landscapeOperandSet = b;
    Pick (s, { 2 }); // Deliberately not either saved operand.
    const auto depth = s.edits.UndoDepth ();
    ASSERT_TRUE (hud::BooleanLandscapeFaceSets (s, geo::OperationKind::Union)) << s.message;
    EXPECT_NEAR (Area (s), faces[0].area + faces[1].area, 1.0e-5);
    EXPECT_EQ (s.edits.UndoDepth (), depth + 1);
    EXPECT_EQ (s.edits.Doc ().landscape->objects.size (), 1u);
    ASSERT_TRUE (hud::BooleanLandscapeFaceSets (s, geo::OperationKind::Difference)) << s.message;
    EXPECT_NEAR (Area (s), faces[0].area, 1.0e-5);
    EXPECT_EQ (s.edits.Doc ().landscape->objects.size (), 1u);
    EXPECT_EQ (s.edits.Doc ().landscape->operations.back ().inputs, (std::vector<edit::Id> { a, b }));
    EXPECT_EQ (Set (s, a), setA);
    EXPECT_EQ (Set (s, b), setB);
    EXPECT_EQ (s.edits.Doc ().landscape->sources.size (), 6u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_NEAR (Area (s), faces[0].area + faces[1].area, 1.0e-5);
    ASSERT_TRUE (s.edits.Redo ());
    EXPECT_NEAR (Area (s), faces[0].area, 1.0e-5);
    ASSERT_TRUE (hud::BooleanLandscapeFaceSets (s, geo::OperationKind::Intersection)) << s.message;
    EXPECT_NEAR (Area (s), faces[1].area, 1.0e-5);
    std::string definition, error;
    ASSERT_TRUE (geo::WriteLandscape (s.edits.Doc (), geo::LandscapeStorage::Virtual, definition, error)) << error;
    ASSERT_TRUE (s.edits.Undo ());
    ASSERT_TRUE (hud::ReloadLandscape (s, definition)) << s.message;
    EXPECT_EQ (s.landscapeSet, 0u);
    EXPECT_EQ (s.landscapeOperandSet, 0u);
    EXPECT_EQ (Set (s, a), setA);
    EXPECT_EQ (Set (s, b), setB);
}

TEST (HudLandscapeSets, InvalidOperandsAndStaleFaceSelectionRefuseWithoutAllocatingOrCreatingOutput)
{
    hud::Session s (Demo ());
    ASSERT_TRUE (hud::CreateLandscapeFaceSet (s));
    const auto a = s.landscapeSet;
    const auto before = s.edits.Doc ();
    for (auto b : { edit::Id { 0 }, a, before.landscape->sources[0].id, before.nextId + 10 }) {
        s.landscapeOperandSet = b;
        EXPECT_FALSE (hud::BooleanLandscapeFaceSets (s, geo::OperationKind::Difference));
        EXPECT_EQ (s.edits.Doc ().landscape, before.landscape);
        EXPECT_EQ (s.edits.Doc ().nextId, before.nextId);
    }
    Pick (s, { 0 });
    const auto stale = s.selection.Items ();
    ASSERT_TRUE (s.edits.Apply ([] (edit::Document& doc, std::string&) {
        doc.entities[0].vertices[2].p.x += 1;
        return true;
    }));
    s.selection.Set (stale);
    const auto next = s.edits.Doc ().nextId;
    EXPECT_FALSE (hud::EditLandscapeFaceSet (s));
    EXPECT_TRUE (Set (s, a).faces.empty ());
    EXPECT_EQ (s.edits.Doc ().nextId, next);
    EXPECT_TRUE (s.edits.Doc ().landscape->objects.empty ());
}
