#include "Edit/EditArrange.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include "Edit/EditSelection.hpp"
#include "Edit/EditSnap.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace ed = geomsrv::edit;

namespace {
struct Scene {
    ed::Document doc;
    ed::Id a, b, c, path;
    std::string error;
    Scene ()
    {
        a = ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 } }, false, "kerb");
        b = ed::AddPolyline (doc, ed::Rectangle ({ 2, 0, 0 }, { 3, 1, 0 }), true, "bed");
        c = ed::AddPolyline (doc, ed::Rectangle ({ 5, 0, 0 }, { 6, 1, 0 }), true, "bed");
        path = ed::AddPath (doc, { { 0, 5, 0 }, { 10, 5, 0 } }, 1.5, "walk");
    }
    ed::Ref R (ed::Id id) const
    {
        return { id, ed::Part::Entity, ed::kNoId };
    }
    void Valid ()
    {
        std::string why;
        EXPECT_TRUE (ed::Validate (doc, why)) << why;
    }
};

std::vector<ed::Id> Ids (const std::vector<ed::Ref>& refs)
{
    std::vector<ed::Id> out;
    for (const auto& r : refs)
        out.push_back (r.entity);
    std::sort (out.begin (), out.end ());
    return out;
}
} // namespace

TEST (EditOrganize, ALayerHidesOrLocksWhatIsOnIt)
{
    Scene s;
    const ed::Id layer = ed::AddLayer (s.doc, "Planting");
    ASSERT_TRUE (ed::MoveToLayer (s.doc, { s.R (s.b), s.R (s.path) }, layer, s.error)) << s.error;
    EXPECT_TRUE (ed::Shown (s.doc, s.b));
    ed::FindLayer (s.doc, layer)->locked = true;
    EXPECT_TRUE (ed::Shown (s.doc, s.b));
    EXPECT_FALSE (ed::Pickable (s.doc, s.b));
    EXPECT_FALSE (ed::Pickable (s.doc, s.path));
    ed::FindLayer (s.doc, layer)->visible = false;
    EXPECT_FALSE (ed::Shown (s.doc, s.b));
    EXPECT_TRUE (ed::Pickable (s.doc, s.a));
    s.Valid ();
    // Deleting the layer leaves its objects on none.
    ASSERT_TRUE (ed::DeleteLayer (s.doc, layer, s.error));
    EXPECT_EQ (ed::MemberOf (s.doc, s.b)->layer, ed::kNoId);
    EXPECT_TRUE (ed::Pickable (s.doc, s.b));
    s.Valid ();
    EXPECT_FALSE (ed::MoveToLayer (s.doc, { s.R (s.a) }, layer, s.error));
}

TEST (EditOrganize, GroupsNestAndUngroupOneLevelAtATime)
{
    Scene s;
    const ed::Id inner = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, "beds", s.error);
    ASSERT_NE (inner, ed::kNoId) << s.error;
    EXPECT_EQ (ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, {}, s.error), ed::kNoId); // one group only
    // A vertex of a grouped object brings its whole group into the next one.
    const ed::Id vertex = ed::Find (s.doc, s.a)->vertices[0].id;
    const ed::Id outer =
        ed::GroupObjects (s.doc, { { s.a, ed::Part::Vertex, vertex }, s.R (s.path) }, "garden", s.error);
    ASSERT_NE (outer, ed::kNoId) << s.error;
    EXPECT_EQ (ed::FindGroup (s.doc, inner)->parent, outer);
    EXPECT_EQ (ed::OutermostGroup (s.doc, s.b), outer);
    EXPECT_EQ (ed::GroupMembers (s.doc, outer), (std::vector<ed::Id> { s.path, s.a, s.b }));
    s.Valid ();

    ASSERT_TRUE (ed::Ungroup (s.doc, { s.R (s.b) }, s.error)) << s.error;
    EXPECT_EQ (ed::FindGroup (s.doc, outer), nullptr);
    EXPECT_EQ (ed::FindGroup (s.doc, inner)->parent, ed::kNoId);
    EXPECT_EQ (ed::MemberOf (s.doc, s.path)->group, ed::kNoId);
    EXPECT_EQ (ed::OutermostGroup (s.doc, s.b), inner);
    s.Valid ();
    EXPECT_FALSE (ed::Ungroup (s.doc, { s.R (s.c) }, s.error));
}

TEST (EditOrganize, APickedObjectWidensToItsGroupUnlessTheGroupIsEntered)
{
    Scene s;
    const ed::Id inner = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, {}, s.error);
    const ed::Id outer = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.c) }, {}, s.error);
    ASSERT_NE (outer, ed::kNoId);
    EXPECT_EQ (Ids (ed::WidenToGroups (s.doc, { s.R (s.a) })), (std::vector<ed::Id> { s.a, s.b, s.c }));
    // Entered, the outer group picks its inner group whole, and the inner one its objects.
    EXPECT_EQ (Ids (ed::WidenToGroups (s.doc, { s.R (s.a) }, outer)), (std::vector<ed::Id> { s.a, s.b }));
    EXPECT_EQ (Ids (ed::WidenToGroups (s.doc, { s.R (s.c) }, outer)), (std::vector<ed::Id> { s.c }));
    EXPECT_EQ (Ids (ed::WidenToGroups (s.doc, { s.R (s.a) }, inner)), (std::vector<ed::Id> { s.a }));
    // A part stays a part; a locked member is not taken along.
    const ed::Ref vertex { s.a, ed::Part::Vertex, ed::Find (s.doc, s.a)->vertices[0].id };
    EXPECT_EQ (ed::WidenToGroups (s.doc, { vertex }), (std::vector<ed::Ref> { vertex }));
    ed::MemberOf (s.doc, s.c)->locked = true;
    EXPECT_EQ (Ids (ed::WidenToGroups (s.doc, { s.R (s.a) })), (std::vector<ed::Id> { s.a, s.b }));
}

TEST (EditOrganize, DeletingAGroupsLastObjectPrunesTheGroup)
{
    Scene s;
    const ed::Id g = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, {}, s.error);
    ASSERT_NE (g, ed::kNoId);
    std::erase_if (s.doc.entities, [&] (const ed::Entity& e) { return e.id == s.a || e.id == s.b; });
    ed::PruneGroups (s.doc);
    EXPECT_TRUE (s.doc.groups.empty ());
    s.Valid ();
}

TEST (EditOrganize, IsolateHidesTheRestAndShowIsolatedBringsOnlyThoseBack)
{
    Scene s;
    ASSERT_TRUE (ed::SetHidden (s.doc, { s.R (s.c) }, true, s.error));
    ASSERT_TRUE (ed::Isolate (s.doc, { s.R (s.a) }, s.error)) << s.error;
    EXPECT_TRUE (ed::Shown (s.doc, s.a));
    EXPECT_FALSE (ed::Shown (s.doc, s.b));
    EXPECT_FALSE (ed::Shown (s.doc, s.path));
    EXPECT_EQ (s.doc.isolated.size (), 2u); // c was already hidden
    // Isolating again starts from what the first isolation hid.
    ASSERT_TRUE (ed::Isolate (s.doc, { s.R (s.b) }, s.error));
    EXPECT_FALSE (ed::Shown (s.doc, s.a));
    EXPECT_TRUE (ed::Shown (s.doc, s.b));
    s.Valid ();
    EXPECT_TRUE (ed::ShowIsolated (s.doc));
    EXPECT_TRUE (ed::Shown (s.doc, s.a));
    EXPECT_TRUE (ed::Shown (s.doc, s.path));
    EXPECT_FALSE (ed::Shown (s.doc, s.c)); // hidden on its own, before any isolation
    EXPECT_TRUE (ed::ShowAll (s.doc));
    EXPECT_TRUE (ed::Shown (s.doc, s.c));
}

TEST (EditOrganize, SelectionQueriesTakeOnlyPickableObjects)
{
    Scene s;
    EXPECT_EQ (ed::SelectAll (s.doc).size (), 4u);
    ASSERT_TRUE (ed::SetLocked (s.doc, { s.R (s.a) }, true, s.error));
    EXPECT_EQ (Ids (ed::SelectAll (s.doc)), (std::vector<ed::Id> { s.b, s.c, s.path }));
    EXPECT_EQ (Ids (ed::Inverse (s.doc, { s.R (s.b) })), (std::vector<ed::Id> { s.c, s.path }));
    EXPECT_EQ (Ids (ed::SelectLike (s.doc, { s.R (s.b) }, ed::Likeness::Role)), (std::vector<ed::Id> { s.b, s.c }));
    EXPECT_EQ (Ids (ed::SelectLike (s.doc, { s.R (s.path) }, ed::Likeness::Kind)), (std::vector<ed::Id> { s.path }));
    const ed::Id layer = ed::AddLayer (s.doc, "Walls");
    ed::MoveToLayer (s.doc, { s.R (s.b), s.R (s.path) }, layer, s.error);
    EXPECT_EQ (Ids (ed::SelectLike (s.doc, { s.R (s.b) }, ed::Likeness::Layer)), (std::vector<ed::Id> { s.b, s.path }));
    ed::UnlockAll (s.doc);
    EXPECT_EQ (ed::SelectAll (s.doc).size (), 4u);
}

TEST (EditOrganize, ValidationRefusesBrokenOrganisation)
{
    Scene s;
    std::string why;
    ed::MemberOf (s.doc, s.a)->layer = 999;
    EXPECT_FALSE (ed::Validate (s.doc, why));
    ed::MemberOf (s.doc, s.a)->layer = ed::kNoId;
    const ed::Id g = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, {}, s.error);
    s.doc.groups.back ().parent = g; // itself
    EXPECT_FALSE (ed::Validate (s.doc, why));
    s.doc.groups.back ().parent = ed::kNoId;
    EXPECT_TRUE (ed::Validate (s.doc, why)) << why;
}

TEST (EditOrganize, APasteKeepsOnlyGroupsCopiedWholeAndArrivesShownAndUnlocked)
{
    Scene s;
    const ed::Id layer = ed::AddLayer (s.doc, "Planting");
    const ed::Id inner = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, "pair", s.error);
    ASSERT_NE (ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.c) }, {}, s.error), ed::kNoId);
    ed::MoveToLayer (s.doc, { s.R (s.a) }, layer, s.error);
    ed::SetLocked (s.doc, { s.R (s.a) }, true, s.error);
    // a and b fill the inner group but not the outer one.
    const auto clip = ed::Copy (s.doc, { s.R (s.a), s.R (s.b) });
    ASSERT_EQ (clip.groups.size (), 1u);
    EXPECT_EQ (clip.groups[0].id, inner);
    EXPECT_EQ (clip.groups[0].parent, ed::kNoId);
    std::vector<ed::Ref> pasted;
    ASSERT_TRUE (ed::Paste (s.doc, clip, { 0, -5, 0 }, pasted, s.error)) << s.error;
    const ed::Membership& m = *ed::MemberOf (s.doc, pasted[0].entity);
    EXPECT_NE (m.group, inner);
    EXPECT_EQ (ed::FindGroup (s.doc, m.group)->name, "pair");
    EXPECT_EQ (ed::MemberOf (s.doc, pasted[1].entity)->group, m.group);
    EXPECT_EQ (m.layer, layer);
    EXPECT_FALSE (m.locked);
    s.Valid ();
    // One object of a group copies loose.
    EXPECT_TRUE (ed::Copy (s.doc, { s.R (s.c) }).groups.empty ());
    EXPECT_EQ (ed::Copy (s.doc, { s.R (s.c) }).entities[0].member.group, ed::kNoId);
}

TEST (EditOrganize, OrganisationRoundTripsAndAnUnorganisedDocumentWritesNoneOfIt)
{
    Scene s;
    namespace geo = tapioca::geometry;
    ASSERT_TRUE (geo::StartLandscape (s.doc, { 1 }, s.error)) << s.error;
    std::string plain;
    ASSERT_TRUE (geo::WriteLandscape (s.doc, geo::LandscapeStorage::Virtual, plain, s.error)) << s.error;
    EXPECT_EQ (plain.find ("member"), std::string::npos);
    EXPECT_EQ (plain.find ("organization"), std::string::npos);

    const ed::Id layer = ed::AddLayer (s.doc, "Planting");
    ed::FindLayer (s.doc, layer)->rgba = 0x336699FFu;
    ed::FindLayer (s.doc, layer)->locked = true;
    ed::MoveToLayer (s.doc, { s.R (s.b), s.R (s.path) }, layer, s.error);
    ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.c) }, "pair", s.error);
    ed::Isolate (s.doc, { s.R (s.a) }, s.error);
    std::string text;
    ASSERT_TRUE (geo::WriteLandscape (s.doc, geo::LandscapeStorage::Virtual, text, s.error)) << s.error;
    ed::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, s.error)) << s.error;
    EXPECT_EQ (loaded.layers, s.doc.layers);
    EXPECT_EQ (loaded.groups, s.doc.groups);
    EXPECT_EQ (loaded.isolated, s.doc.isolated);
    EXPECT_EQ (ed::MemberOf (loaded, s.path)->layer, layer);
    EXPECT_EQ (*ed::MemberOf (loaded, s.c), *ed::MemberOf (s.doc, s.c));
    ed::Document older;
    ASSERT_TRUE (geo::ReadLandscape (plain, older, s.error)) << s.error;
    EXPECT_TRUE (older.layers.empty ());
}

TEST (EditOrganize, HidingIsOneUndoableStep)
{
    Scene s;
    ed::Session session (s.doc);
    const ed::Id a = s.a;
    ASSERT_TRUE (session.Apply ([a] (ed::Document& d, std::string& error) {
        return ed::SetHidden (d, { { a, ed::Part::Entity, ed::kNoId } }, true, error);
    })) << session.Message ();
    EXPECT_EQ (session.UndoDepth (), 1u);
    ASSERT_TRUE (session.Undo ());
    EXPECT_TRUE (ed::Shown (session.Doc (), a));
}

TEST (EditOrganize, ALockedReferenceLineCanBeHiddenAndLayeredButNotMoved)
{
    namespace geo = tapioca::geometry;
    ed::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1 }, error)) << error;
    const ed::Id line = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    ASSERT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::ReferenceLine, error)) << error;
    geo::ReferenceLineStyle style;
    style.locked = true;
    ASSERT_TRUE (geo::SetReferenceLineStyle (doc, doc.landscape->sources[0].id, style, error)) << error;
    const ed::Id layer = ed::AddLayer (doc, "Survey");
    ed::Session session (doc);
    const std::vector<ed::Ref> refs { { line, ed::Part::Entity, ed::kNoId } };
    ASSERT_TRUE (session.Apply ([refs, layer] (ed::Document& d, std::string& e) {
        return ed::SetHidden (d, refs, true, e) && ed::MoveToLayer (d, refs, layer, e);
    })) << session.Message ();
    EXPECT_FALSE (session.Apply ([line] (ed::Document& d, std::string&) {
        ed::Find (d, line)->vertices[0].p.x = 1;
        return true;
    }));
}

TEST (EditOrganize, HiddenObjectsAreNeitherPickedNorSnappedLockedOnesSnapButDoNotPick)
{
    Scene s;
    ed::PickView view;
    view.toScreen = [] (ed::Vec3 p, double& x, double& y) {
        x = p.x * 100.0;
        y = p.y * 100.0;
        return true;
    };
    ed::SnapQuery query;
    query.cursor = { 0.01, 0, 0 };
    query.tolerance = 0.1;
    query.modes = ed::kSnapEndpoint; // not the guides that run along b's bottom edge through here
    EXPECT_FALSE (ed::Pick (s.doc, view, 0, 0, ed::kPickAll, 5).empty ());
    ASSERT_TRUE (ed::SetLocked (s.doc, { s.R (s.a) }, true, s.error));
    EXPECT_TRUE (ed::Pick (s.doc, view, 0, 0, ed::kPickAll, 5).empty ());
    EXPECT_TRUE (ed::Snap (s.doc, {}, query).snapped);
    ed::SetLocked (s.doc, { s.R (s.a) }, false, s.error);
    ASSERT_TRUE (ed::SetHidden (s.doc, { s.R (s.a) }, true, s.error));
    EXPECT_TRUE (ed::Pick (s.doc, view, 0, 0, ed::kPickAll, 5).empty ());
    EXPECT_FALSE (ed::Snap (s.doc, {}, query).snapped);
    // A path on a hidden layer.
    const ed::Id layer = ed::AddLayer (s.doc, "Walks");
    ed::MoveToLayer (s.doc, { s.R (s.path) }, layer, s.error);
    EXPECT_FALSE (ed::Pick (s.doc, view, 500, 500, ed::kPickAll, 5).empty ());
    ed::FindLayer (s.doc, layer)->visible = false;
    EXPECT_TRUE (ed::Pick (s.doc, view, 500, 500, ed::kPickAll, 5).empty ());
    // A marquee round everything takes only what can be picked.
    ed::Selection sel;
    sel.Marquee (s.doc, view, -100, -100, 1100, 700, ed::kPickAll, false);
    EXPECT_EQ (Ids (sel.Items ()), (std::vector<ed::Id> { s.b, s.c }));
    // And a selection drops what has been hidden since.
    ed::SetHidden (s.doc, { s.R (s.b) }, true, s.error);
    sel.Prune (s.doc);
    EXPECT_EQ (Ids (sel.Items ()), (std::vector<ed::Id> { s.c }));
}

TEST (EditOrganize, GroupingInsideAnEnteredGroupMakesAGroupWithinIt)
{
    Scene s;
    const ed::Id outer = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b), s.R (s.c) }, "beds", s.error);
    ASSERT_NE (outer, ed::kNoId);
    const ed::Id inner = ed::GroupObjects (s.doc, { s.R (s.a), s.R (s.b) }, "pair", s.error, outer);
    ASSERT_NE (inner, ed::kNoId) << s.error;
    EXPECT_EQ (ed::FindGroup (s.doc, inner)->parent, outer);
    EXPECT_EQ (ed::OutermostGroup (s.doc, s.a), outer);
    EXPECT_EQ (ed::MemberOf (s.doc, s.c)->group, outer);
    s.Valid ();
    // Ungrouped from inside, the inner group goes and its objects stay in the outer one.
    ASSERT_TRUE (ed::Ungroup (s.doc, { s.R (s.a) }, s.error, outer)) << s.error;
    EXPECT_EQ (ed::FindGroup (s.doc, inner), nullptr);
    EXPECT_EQ (ed::MemberOf (s.doc, s.a)->group, outer);
    EXPECT_FALSE (ed::Ungroup (s.doc, { s.R (s.a) }, s.error, outer)); // loose inside it
}
