#include "Edit/EditLandscapeEntrance.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace {
namespace edit = geomsrv::edit;
using namespace tapioca::geometry;

// A 10 x 6 m building outline from (0, 0), and a path from (20, 3) to (14, 3).
struct Site {
    edit::Document doc;
    edit::Id outline = 0, source = 0, path = 0;
    std::string error;
    Site ()
    {
        EXPECT_TRUE (StartLandscape (doc, { 1 }, error)) << error;
        outline = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 6, 0 }, { 0, 6, 0 } }, true);
        EXPECT_TRUE (SetLandscapeSourceRole (doc, outline, SourceRole::Building, error)) << error;
        for (const auto& s : doc.landscape->sources)
            if (s.entityId == outline)
                source = s.id;
        path = edit::AddPath (doc, { { 20, 3, 0 }, { 14, 3, 0 } }, 2.0);
    }
};
} // namespace

TEST (EditLandscapeEntrance, AnEntranceSitsOnItsOutlineAndFollowsItsEdits)
{
    Site site;
    Id door = 0;
    // A point near the east wall lands on it, half way up: 10 + 3 m along from (0, 0).
    ASSERT_TRUE (AddLandscapeEntrance (site.doc, site.source, { 11, 3, 0 }, 0.15, "", door, site.error)) << site.error;
    ASSERT_EQ (site.doc.landscape->entrances.size (), 1U);
    EXPECT_NEAR (site.doc.landscape->entrances[0].along, 13.0, 1e-9);
    Vec3 at;
    ASSERT_TRUE (LandscapeEntrancePoint (site.doc, site.doc.landscape->entrances[0], at));
    EXPECT_NEAR (at.x, 10, 1e-9);
    EXPECT_NEAR (at.y, 3, 1e-9);
    EXPECT_DOUBLE_EQ (at.z, 0.15);
    // The building grows east: the entrance stays on its wall, the same distance along.
    auto* e = edit::Find (site.doc, site.outline);
    e->vertices[1].p.x = e->vertices[2].p.x = 12;
    ASSERT_TRUE (LandscapeEntrancePoint (site.doc, site.doc.landscape->entrances[0], at));
    EXPECT_NEAR (at.x, 12, 1e-9);
    EXPECT_NEAR (at.y, 1, 1e-9);
    EXPECT_EQ (EntranceNear (site.doc, { 12.2, 1, 0 }, 0.5)->id, door);
    EXPECT_EQ (EntranceNear (site.doc, { 20, 20, 0 }, 0.5), nullptr);
}

TEST (EditLandscapeEntrance, OnlyABuildingOutlineTakesEntrancesAndLosingTheRoleDropsThem)
{
    Site site;
    Id door = 0;
    const auto open = edit::AddPolyline (site.doc, { { 0, 10, 0 }, { 5, 10, 0 } }, false);
    EXPECT_FALSE (SetLandscapeSourceRole (site.doc, open, SourceRole::Building, site.error));
    EXPECT_NE (site.error.find ("closed"), std::string::npos);
    EXPECT_FALSE (AddLandscapeEntrance (site.doc, 999, { 0, 0, 0 }, 0, "", door, site.error));
    ASSERT_TRUE (AddLandscapeEntrance (site.doc, site.source, { 5, -1, 0 }, 0, "", door, site.error)) << site.error;
    ASSERT_TRUE (SetLandscapeSourceRole (site.doc, site.outline, SourceRole::Guide, site.error)) << site.error;
    EXPECT_TRUE (site.doc.landscape->entrances.empty ());
}

TEST (EditLandscapeEntrance, EntrancesAndTheirDoorsSurviveSaveAndLoad)
{
    Site site;
    Id door = 0;
    const std::string guid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
    ASSERT_TRUE (AddLandscapeEntrance (site.doc, site.source, { 5, -1, 0 }, 0.3, guid, door, site.error)) << site.error;
    std::string text;
    ASSERT_TRUE (WriteLandscape (site.doc, LandscapeStorage::Virtual, text, site.error)) << site.error;
    edit::Document loaded;
    ASSERT_TRUE (ReadLandscape (text, loaded, site.error)) << site.error;
    ASSERT_EQ (loaded.landscape->entrances.size (), 1U);
    EXPECT_EQ (loaded.landscape->entrances[0], site.doc.landscape->entrances[0]);
    ASSERT_TRUE (RemoveLandscapeEntrance (site.doc, door, site.error)) << site.error;
    EXPECT_FALSE (RemoveLandscapeEntrance (site.doc, door, site.error));
}

TEST (EditLandscapeEntrance, APathReachesAnEntranceAtItsThresholdHeight)
{
    Site site;
    Id door = 0;
    ASSERT_TRUE (AddLandscapeEntrance (site.doc, site.source, { 10.5, 3, 0 }, 0.45, "", door, site.error))
        << site.error;
    ASSERT_TRUE (ReachLandscapeEntrance (site.doc, site.path, door, site.error)) << site.error;
    const auto& p = *edit::FindPath (site.doc, site.path);
    // The nearer end (14, 3) moved onto the entrance; that end is fixed at the threshold.
    EXPECT_NEAR (p.points.back ().p.x, 10, 1e-9);
    EXPECT_NEAR (p.points.back ().p.y, 3, 1e-9);
    const auto& end = p.profile.back ();
    EXPECT_TRUE (end.fixed);
    EXPECT_DOUBLE_EQ (end.z, 0.45);
    EXPECT_EQ (end.binding, EntranceBinding (door));
    EXPECT_FALSE (p.profile.front ().fixed) << "the other end is free";
}

TEST (EditLandscapeEntrance, ALineReferencedFromArchicadIsLockedInTheEditor)
{
    Site site;
    for (auto& s : site.doc.landscape->sources)
        if (s.id == site.source)
            s.archicadGuid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA", s.reference = true;
    edit::Session session (site.doc);
    const auto move = [&] (edit::Document& d, std::string&) {
        edit::Find (d, site.outline)->vertices[0].p.x -= 1;
        return true;
    };
    EXPECT_FALSE (session.Apply (move));
    EXPECT_NE (session.Message ().find ("referenced from Archicad"), std::string::npos) << session.Message ();
    EXPECT_FALSE (session.Apply ([&] (edit::Document& d, std::string& e) {
        d.entities.erase (std::remove_if (d.entities.begin (), d.entities.end (),
                                          [&] (const edit::Entity& x) { return x.id == site.outline; }),
                          d.entities.end ());
        return DeleteLandscapeSource (d, site.source, e);
    }));
    // Entrances on it, its role and other lines stay editable.
    Id door = 0;
    EXPECT_TRUE (session.Apply ([&] (edit::Document& d, std::string& e) {
        return AddLandscapeEntrance (d, site.source, { 5, -1, 0 }, 0, "", door, e);
    })) << session.Message ();
    EXPECT_TRUE (session.Apply ([&] (edit::Document& d, std::string& e) {
        return edit::MovePathPoint (d, site.path, edit::FindPath (d, site.path)->points[0].id, { 22, 3, 0 }, e);
    })) << session.Message ();
}
