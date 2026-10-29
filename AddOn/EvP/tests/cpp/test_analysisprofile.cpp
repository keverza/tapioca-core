#include "ArchViz/AnalysisProfile.hpp"

#include <gtest/gtest.h>
#include <cmath>

using namespace geomsrv::archviz::analysisprofile;

TEST (AnalysisProfile, AFlatProfileIsOneHeightAllAlong)
{
    const Profile flat = Flat (20.0, 1.6);
    for (const double station : { -1.0, 0.0, 7.3, 20.0, 25.0 })
        EXPECT_NEAR (ZAt (flat, station), 1.6, 1e-12);
}

TEST (AnalysisProfile, AnInsertedAnchorLiesOnTheCurveAndLiftsItWhenMoved)
{
    Profile profile = Flat (20.0, 1.6);
    const size_t index = Insert (profile, 10.0);
    ASSERT_EQ (index, 1u);
    ASSERT_EQ (profile.anchors.size (), 3u);
    EXPECT_NEAR (profile.anchors[1].station, 10.0, 1e-9);
    EXPECT_NEAR (ZAt (profile, 10.0), 1.6, 1e-9) << "inserting does not change the height";
    MoveAnchor (profile, 1, 10.0, 4.0);
    EXPECT_NEAR (ZAt (profile, 10.0), 4.0, 1e-9);
    EXPECT_GT (ZAt (profile, 5.0), 1.6);
    EXPECT_LT (ZAt (profile, 5.0), 4.0);
    EXPECT_NEAR (ZAt (profile, 0.0), 1.6, 1e-9) << "the ends stay";
    EXPECT_EQ (Insert (profile, 10.0), 1u) << "one anchor per station";
}

TEST (AnalysisProfile, TheEndsKeepTheirStationsAndAnInteriorAnchorStaysBetweenItsNeighbours)
{
    Profile profile = Flat (10.0, 0.0);
    MoveAnchor (profile, 0, 3.0, 1.0);
    EXPECT_DOUBLE_EQ (profile.anchors[0].station, 0.0);
    EXPECT_DOUBLE_EQ (profile.anchors[0].z, 1.0);
    Insert (profile, 5.0);
    MoveAnchor (profile, 1, 50.0, 0.0);
    EXPECT_LT (profile.anchors[1].station, 10.0);
    EXPECT_FALSE (Remove (profile, 0));
    EXPECT_FALSE (Remove (profile, 2));
    EXPECT_TRUE (Remove (profile, 1));
    EXPECT_EQ (profile.anchors.size (), 2u);
}

TEST (AnalysisProfile, ALongHandleIsClampedSoEachStationHasOneHeight)
{
    Profile profile = Flat (10.0, 0.0);
    MoveHandle (profile, 0, 40.0, 30.0, true);   // far past the next anchor
    MoveHandle (profile, 1, -30.0, -5.0, false); // its in-handle, as far back
    const auto curve = Sample (profile, 64);
    for (size_t i = 1; i < curve.size (); ++i)
        EXPECT_GE (curve[i].first, curve[i - 1].first - 1e-12) << "the station only grows";
    const auto tip = HandleTip (profile, 0, true);
    EXPECT_LE (tip.first, 5.0 + 1e-12) << "drawn where the curve uses it";
}

TEST (AnalysisProfile, FitStretchesTheProfileToANewPathLength)
{
    Profile profile = Flat (10.0, 2.0);
    Insert (profile, 5.0);
    MoveAnchor (profile, 1, 5.0, 3.0);
    Fit (profile, 20.0);
    EXPECT_DOUBLE_EQ (profile.anchors.back ().station, 20.0);
    EXPECT_NEAR (profile.anchors[1].station, 10.0, 1e-9);
    EXPECT_NEAR (ZAt (profile, 10.0), 3.0, 1e-9);
}

TEST (AnalysisProfile, TheMetadataRoundTripsAndRefusesWhatIsNotAProfile)
{
    Profile profile = Flat (12.0, 1.5);
    Insert (profile, 4.0);
    MoveAnchor (profile, 1, 4.0, 2.5);
    MoveHandle (profile, 1, 5.0, 3.0, true);
    const auto property = Encode (profile);
    EXPECT_EQ (property.key, kPropertyKey);
    Profile read;
    std::string error;
    ASSERT_TRUE (Decode (property, read, error)) << error;
    ASSERT_EQ (read.anchors.size (), profile.anchors.size ());
    for (const double station : { 0.0, 2.0, 4.0, 9.0, 12.0 })
        EXPECT_NEAR (ZAt (read, station), ZAt (profile, station), 1e-12);

    auto backwards = property;
    std::swap (backwards.value.list[0], backwards.value.list[2]);
    EXPECT_FALSE (Decode (backwards, read, error));
    auto single = property;
    single.value.list.resize (1);
    EXPECT_FALSE (Decode (single, read, error));
}
