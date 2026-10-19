#include "Edit/EditProfileView.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
// A straight 100 m path, 100.0 -> 103.0, with a break at 40 m / 102.0.
struct Ramp {
    ed::Document doc;
    ed::Id path = 0, mid = 0;
    std::string error;
    Ramp ()
    {
        path = ed::AddPath (doc, { { 0, 0, 100.0 }, { 100, 0, 103.0 } }, 1.8);
        EXPECT_TRUE (ed::InsertPathBreak (doc, path, 40.0, error, &mid)) << error;
        EXPECT_TRUE (ed::SetPathElevation (doc, path, mid, 102.0, error)) << error;
    }
    const ed::Path& P () const
    {
        return *ed::FindPath (doc, path);
    }
};
} // namespace

TEST (EditProfileView, FitExaggeratesHeightsIntoTheCanvas)
{
    Ramp r;
    const auto v = ed::FitProfile (r.P (), 0, 0, 420, 140, 10);
    // 100 m over 400 px is 4 px/m; 3 m over 120 px wants 40 px/m: x10.
    EXPECT_NEAR (v.exaggeration, 10.0, 1e-9);
    double x = 0, y = 0;
    ed::ProfileToScreen (v, 0.0, 100.0, x, y);
    EXPECT_NEAR (x, 10.0, 1e-9);
    EXPECT_NEAR (y, 130.0, 1e-9);
    ed::ProfileToScreen (v, 100.0, 103.0, x, y);
    EXPECT_NEAR (x, 410.0, 1e-9);
    EXPECT_NEAR (y, 10.0, 1e-9);
    double s = 0, z = 0;
    ed::ProfileFromScreen (v, 210.0, 70.0, s, z);
    EXPECT_NEAR (s, 50.0, 1e-9);
    EXPECT_NEAR (z, 101.5, 1e-9);
    // A flat profile is never squashed below true scale and stays centred.
    const auto capped = ed::FitProfile (r.P (), 0, 0, 420, 140, 10, {}, 3.0);
    EXPECT_NEAR (capped.exaggeration, 3.0, 1e-9);
}

TEST (EditProfileView, PickBreaksBeforeTangents)
{
    Ramp r;
    const auto v = ed::FitProfile (r.P (), 0, 0, 420, 140, 10);
    double x = 0, y = 0;
    ed::ProfileToScreen (v, 40.0, 102.0, x, y);
    const auto hits = ed::PickProfile (r.P (), v, x + 2, y + 1, 6);
    ASSERT_GE (hits.size (), 2u);
    EXPECT_EQ (hits[0].kind, ed::ProfileHit::Kind::Break);
    EXPECT_EQ (hits[0].index, 1u);
    ed::ProfileToScreen (v, 70.0, 102.5, x, y);
    const auto tangent = ed::PickProfile (r.P (), v, x, y + 2, 6);
    ASSERT_EQ (tangent.size (), 1u);
    EXPECT_EQ (tangent[0].kind, ed::ProfileHit::Kind::Tangent);
    EXPECT_EQ (tangent[0].index, 1u);
}

TEST (EditProfileView, SteepPositiveAndNegativeProfilesFitWithLinearCompression)
{
    for (double height : { 1000000., -1000000. }) {
        ed::Document doc;
        ed::AddPath (doc, { { 0, 0, 0 }, { 10, 0, height } }, 2);
        const auto view = ed::FitProfile (doc.paths[0], 0, 0, 420, 150, 25);
        EXPECT_LT (view.exaggeration, 1);
        for (double t : { 0., .25, .5, 1. }) {
            double x = 0, y = 0, station = 0, z = 0;
            ed::ProfileToScreen (view, 10 * t, height * t, x, y);
            EXPECT_GE (y, 25 - 1e-8);
            EXPECT_LE (y, 125 + 1e-8);
            ed::ProfileFromScreen (view, x, y, station, z);
            EXPECT_NEAR (station, 10 * t, 1e-8);
            EXPECT_NEAR (z, height * t, 1e-8);
        }
    }
}

TEST (EditProfileView, DraggedBreakSnapsToRoundGrades)
{
    Ramp r;
    double s = 40.0, z = 102.03; // 5.075 % up from the start
    std::string hint;
    ed::SnapBreak (r.P (), 1, s, z, {}, hint);
    EXPECT_EQ (hint, "5.0 %");
    EXPECT_NEAR (z, 102.0, 1e-12);
    s = 40.0;
    z = 101.313; // 3.28 % before, 2.81 % after: no round grade near, so the elevation rounds
    ed::SnapBreak (r.P (), 1, s, z, {}, hint);
    EXPECT_TRUE (hint.empty ());
    EXPECT_NEAR (z, 101.31, 1e-12);
    ed::ProfileSnap stations;
    stations.stationStep = 5.0;
    s = 41.7;
    z = 102.0;
    ed::SnapBreak (r.P (), 1, s, z, stations, hint);
    EXPECT_NEAR (s, 40.0, 1e-12);
}

TEST (EditProfileView, DragBreakAndTangentThroughTheSession)
{
    Ramp r;
    ed::Session s (r.doc);
    ASSERT_TRUE (s.Begin ({ r.path, ed::Part::Break, r.mid }));
    ASSERT_TRUE (s.Move (ed::DragBreak (r.path, r.mid, 45.0, 102.4))) << s.Message ();
    ASSERT_TRUE (s.Move (ed::DragBreak (r.path, r.mid, 50.0, 102.2))) << s.Message ();
    ASSERT_TRUE (s.End ());
    const auto* p = ed::FindPath (s.Doc (), r.path);
    EXPECT_DOUBLE_EQ (p->profile[1].station, 50.0);
    EXPECT_DOUBLE_EQ (p->profile[1].z, 102.2);
    EXPECT_EQ (s.UndoDepth (), 1u);
    ASSERT_TRUE (s.Apply (ed::DragTangent (r.path, 0, -0.5))) << s.Message ();
    p = ed::FindPath (s.Doc (), r.path);
    EXPECT_DOUBLE_EQ (p->profile[0].z, 99.5);
    EXPECT_DOUBLE_EQ (p->profile[1].z, 101.7);
}

TEST (EditProfileView, FixedBreaksOnlySlideAndHoldTheirTangents)
{
    Ramp r;
    std::string error;
    ASSERT_TRUE (ed::FixElevationAt (r.doc, r.path, 40.0, 102.0, "door", error)) << error;
    ed::Session s (r.doc);
    EXPECT_FALSE (s.Apply (ed::DragBreak (r.path, r.mid, 40.0, 103.0)));
    EXPECT_TRUE (s.Apply (ed::DragBreak (r.path, r.mid, 42.0, 102.0))) << s.Message ();
    EXPECT_FALSE (s.Apply (ed::DragTangent (r.path, 0, 1.0)));
    // The start break is an end: it moves in height but never along the path.
    const ed::Id start = r.P ().profile.front ().id;
    ASSERT_TRUE (s.Apply (ed::DragBreak (r.path, start, 10.0, 100.2))) << s.Message ();
    EXPECT_DOUBLE_EQ (ed::FindPath (s.Doc (), r.path)->profile.front ().station, 0.0);
}

TEST (EditProfileView, DrawingCarriesLineMarkersCurvesAndGrades)
{
    Ramp r;
    std::string error;
    ASSERT_TRUE (ed::SetPathCurve (r.doc, r.path, r.mid, 10.0, error)) << error;
    const auto d = ed::DrawProfile (r.P (), 5.0, { { 0, 99.5 }, { 100, 102.0 } });
    ASSERT_EQ (d.breaks.size (), 3u);
    ASSERT_EQ (d.curves.size (), 1u);
    EXPECT_DOUBLE_EQ (d.curves[0].from, 35.0);
    ASSERT_EQ (d.grades.size (), 2u);
    EXPECT_NEAR (d.grades[0].percent, 5.0, 1e-9);
    EXPECT_NEAR (d.grades[1].percent, 1.0 / 60.0 * 100.0, 1e-9);
    EXPECT_EQ (d.terrain.size (), 2u);
    for (size_t i = 0; i + 1 < d.line.size (); ++i)
        EXPECT_LT (d.line[i].first, d.line[i + 1].first);
    // The drawn line passes below the break, along the curve.
    for (const auto& [s, z] : d.line)
        if (std::abs (s - 40.0) < 1e-9)
            EXPECT_LT (z, 102.0);
}
