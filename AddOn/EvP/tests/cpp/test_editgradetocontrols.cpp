// #109: grade paths to control points -- each enabled fixed-height point within reach of a path
// sets its elevation where it projects onto it; the nearer of two at one place wins; ends with
// none in reach keep their height; a path with none in reach is untouched.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditGradeToControls.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPolyline.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace ed = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {
// A fixed-height point at `p`: the first vertex of a short straight guide.
void Control (ed::Document& doc, ed::Vec3 p)
{
    std::string error;
    const ed::Id line = ed::AddPolyline (doc, { p, { p.x, p.y + 20, p.z } }, false);
    geo::LandscapeSource source;
    source.entityId = line;
    ed::Id id = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    ASSERT_TRUE (geo::SetLandscapeTerrainControl (doc, id, ed::Find (doc, line)->vertices[0].id, true, error)) << error;
}

struct Site {
    ed::Document doc;
    ed::Id walk = 0, far = 0;
    Site ()
    {
        std::string error;
        EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3 }, error)) << error;
        walk = ed::AddPath (doc, { { 0, 0, 0 }, { 30, 0, 0 } }, 2.0, "walk");
        far = ed::AddPath (doc, { { 0, 100, 0 }, { 30, 100, 0 } }, 2.0, "far");
        Control (doc, { 10.0, 0.5, 2.0 });  // in reach, but...
        Control (doc, { 10.3, -0.2, 3.0 }); // ...this one is nearer the centreline at the same place
        Control (doc, { 20.0, 4.0, 9.0 });  // out of reach
        Control (doc, { 30.4, 0.0, 1.5 });  // past the end: the end takes it
    }
};

const ed::GradeBreak* BreakNear (const ed::Path& p, double station)
{
    for (const auto& b : p.profile)
        if (std::abs (b.station - station) < 1e-6)
            return &b;
    return nullptr;
}
} // namespace

TEST (EditGradeToControls, ControlsInReachSetThePathsHeightsAndTheRestStay109)
{
    Site s;
    geo::GradeToControlsReport report;
    std::string error;
    ASSERT_TRUE (geo::GradePathsToControls (s.doc, { s.walk, s.far }, 1.0, report, error)) << error;
    EXPECT_EQ (report.controls, 4u);
    ASSERT_EQ (report.graded.size (), 1u);
    EXPECT_EQ (report.graded[0].path, s.walk);
    EXPECT_EQ (report.graded[0].points, 2u);
    EXPECT_EQ (report.untouched, (std::vector<ed::Id> { s.far }));
    EXPECT_EQ (report.skipped, 1u) << "the farther of the two at station 10";
    EXPECT_NE (report.Summary ().find ("Graded 1 path to 2 control points"), std::string::npos) << report.Summary ();

    const ed::Path* walk = ed::FindPath (s.doc, s.walk);
    ASSERT_NE (walk, nullptr);
    const auto* mid = BreakNear (*walk, 10.3);
    ASSERT_NE (mid, nullptr);
    EXPECT_DOUBLE_EQ (mid->z, 3.0);
    EXPECT_TRUE (mid->fixed);
    EXPECT_EQ (mid->binding, "control");
    EXPECT_DOUBLE_EQ (walk->profile.back ().z, 1.5) << "the end in reach takes the control's height";
    EXPECT_DOUBLE_EQ (walk->profile.back ().station, 30.0) << "and stays the end";
    EXPECT_DOUBLE_EQ (walk->profile.front ().z, 0.0) << "an end with none in reach keeps its height";
    double z = 0.0;
    ASSERT_TRUE (ed::Elevation (walk->profile, 20.15, z));
    EXPECT_NEAR (z, 2.25, 1e-9) << "straight grade between";
    const ed::Path* far = ed::FindPath (s.doc, s.far);
    for (const auto& b : far->profile)
        EXPECT_DOUBLE_EQ (b.z, 0.0);

    // Again: the breaks it fixed are reused, nothing is added.
    const auto breaks = walk->profile.size ();
    ASSERT_TRUE (geo::GradePathsToControls (s.doc, { s.walk }, 1.0, report, error)) << error;
    EXPECT_EQ (ed::FindPath (s.doc, s.walk)->profile.size (), breaks);

    // A wider reach takes the third control too.
    ASSERT_TRUE (geo::GradePathsToControls (s.doc, { s.walk }, 5.0, report, error)) << error;
    EXPECT_NE (BreakNear (*ed::FindPath (s.doc, s.walk), 20.0), nullptr);

    EXPECT_FALSE (geo::GradePathsToControls (s.doc, { s.walk }, 0.0, report, error));
    EXPECT_FALSE (geo::GradePathsToControls (s.doc, {}, 1.0, report, error));
    EXPECT_FALSE (geo::GradePathsToControls (s.doc, { ed::Id (99999) }, 1.0, report, error));
}

// A grade break another binding fixed (a door's threshold) is kept, and the control is reported.
TEST (EditGradeToControls, AnotherFixedHeightIsKept109)
{
    Site s;
    std::string error;
    ASSERT_TRUE (ed::FixElevationAt (s.doc, s.walk, 10.0, 0.4, "door", error)) << error;
    geo::GradeToControlsReport report;
    ASSERT_TRUE (geo::GradePathsToControls (s.doc, { s.walk }, 1.0, report, error)) << error;
    const auto* door = BreakNear (*ed::FindPath (s.doc, s.walk), 10.0);
    ASSERT_NE (door, nullptr);
    EXPECT_DOUBLE_EQ (door->z, 0.4);
    EXPECT_EQ (door->binding, "door");
    EXPECT_EQ (report.skipped, 2u) << "the farther control, and the one the door's height kept out";
    ASSERT_EQ (report.graded.size (), 1u);
    EXPECT_EQ (report.graded[0].points, 1u) << "only the end";
}

// The editor's command: the selected paths, one undo step, the outcome in the message; nothing
// in reach is no edit at all.
TEST (EditGradeToControls, TheEditorGradesTheSelectedPathsAsOneUndo109)
{
    Site site;
    geomsrv::archviz::edithud::Session s (site.doc);
    EXPECT_FALSE (geomsrv::archviz::edithud::GradeSelectedPathsToControls (s));
    EXPECT_NE (s.message.find ("Select the paths"), std::string::npos) << s.message;

    s.selection.Set ({ { site.far, ed::Part::Entity, 0 } });
    EXPECT_FALSE (geomsrv::archviz::edithud::GradeSelectedPathsToControls (s));
    EXPECT_NE (s.message.find ("No control point lies within reach"), std::string::npos) << s.message;
    EXPECT_EQ (s.edits.UndoDepth (), 0u);

    s.selection.Set ({ { site.walk, ed::Part::Entity, 0 }, { site.far, ed::Part::Entity, 0 } });
    ASSERT_TRUE (geomsrv::archviz::edithud::GradeSelectedPathsToControls (s)) << s.message;
    EXPECT_NE (s.message.find ("Graded 1 path"), std::string::npos) << s.message;
    EXPECT_NE (s.message.find ("1 had none in reach"), std::string::npos) << s.message;
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_DOUBLE_EQ (ed::FindPath (s.edits.Doc (), site.walk)->profile.back ().z, 0.0);
}
