#include "Edit/EditPath.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace ed = geomsrv::edit;

namespace {
// 0 -> 50 m rising 5 %, then 50 -> 100 m falling 2 %.
std::vector<ed::GradeBreak> Crest (double curve = 0.0)
{
    std::vector<ed::GradeBreak> b (3);
    b[0] = { 1, 0.0, 100.0 };
    b[1] = { 2, 50.0, 102.5, curve };
    b[2] = { 3, 100.0, 101.5 };
    return b;
}
} // namespace

TEST (EditPathProfile, TangentsBetweenBreaks)
{
    const auto b = Crest ();
    std::string error;
    ASSERT_TRUE (ed::ValidateProfile (b, 100.0, error)) << error;
    double z = 0, g = 0;
    ASSERT_TRUE (ed::Elevation (b, 20.0, z, &g));
    EXPECT_NEAR (z, 101.0, 1e-12);
    EXPECT_NEAR (g, 0.05, 1e-12);
    ASSERT_TRUE (ed::Elevation (b, 75.0, z, &g));
    EXPECT_NEAR (z, 102.0, 1e-12);
    EXPECT_NEAR (g, -0.02, 1e-12);
    EXPECT_FALSE (ed::Elevation (b, 101.0, z));
}

TEST (EditPathProfile, VerticalCurveIsAParabolaJoiningBothTangents)
{
    const auto b = Crest (20.0);
    std::string error;
    ASSERT_TRUE (ed::ValidateProfile (b, 100.0, error)) << error;
    double z = 0, g = 0;
    // At the break the curve sits (g2 - g1) L / 8 below it.
    ASSERT_TRUE (ed::Elevation (b, 50.0, z, &g));
    EXPECT_NEAR (z, 102.5 + (-0.02 - 0.05) * 20.0 / 8.0, 1e-12);
    EXPECT_NEAR (g, 0.015, 1e-12);
    // Its ends meet the tangents in elevation and grade.
    for (double s : { 40.0, 60.0 }) {
        double zc = 0, gc = 0;
        ASSERT_TRUE (ed::Elevation (b, s, zc, &gc));
        const double tangent = s < 50.0 ? 100.0 + 0.05 * s : 102.5 - 0.02 * (s - 50.0);
        EXPECT_NEAR (zc, tangent, 1e-12);
        EXPECT_NEAR (gc, s < 50.0 ? 0.05 : -0.02, 1e-12);
    }
}

TEST (EditPathProfile, ValidationRefusesBrokenProfiles)
{
    std::string error;
    auto b = Crest (120.0);
    EXPECT_FALSE (ed::ValidateProfile (b, 100.0, error)); // the curve runs past the ends
    b = Crest ();
    EXPECT_FALSE (ed::ValidateProfile (b, 90.0, error));
    b[1].station = 0.0;
    EXPECT_FALSE (ed::ValidateProfile (b, 100.0, error));
    b = Crest ();
    b.front ().curve = 5.0;
    EXPECT_FALSE (ed::ValidateProfile (b, 100.0, error));
}

TEST (EditPathProfile, SetGradeKeepsTheFixedEnd)
{
    auto b = Crest ();
    std::string error;
    ASSERT_TRUE (ed::SetGrade (b, 0, 4.0, error));
    EXPECT_NEAR (b[1].z, 102.0, 1e-12); // the start held, the end moved
    b[1].fixed = true;
    ASSERT_TRUE (ed::SetGrade (b, 0, 2.0, error));
    EXPECT_NEAR (b[1].z, 102.0, 1e-12);
    EXPECT_NEAR (b[0].z, 101.0, 1e-12); // the fixed end held, the start moved
    b[0].fixed = true;
    EXPECT_FALSE (ed::SetGrade (b, 0, 1.0, error));
    EXPECT_FALSE (ed::SetBreakElevation (b, 2, 99.0, error));
    EXPECT_NE (error.find ("fixed"), std::string::npos);
}

TEST (EditPathProfile, InsertKeepsTheShapeDeleteAndMoveRespectRules)
{
    auto b = Crest (10.0);
    std::string error;
    ASSERT_TRUE (ed::InsertBreak (b, 25.0, 7, error)) << error;
    ASSERT_EQ (b.size (), 4u);
    EXPECT_NEAR (b[1].z, 101.25, 1e-12);
    EXPECT_FALSE (ed::InsertBreak (b, 52.0, 8, error)); // inside the curve
    EXPECT_FALSE (ed::InsertBreak (b, 0.0, 8, error));
    ASSERT_TRUE (ed::MoveBreak (b, 7, 30.0, error));
    EXPECT_FALSE (ed::MoveBreak (b, 7, 60.0, error)); // past its neighbour
    EXPECT_FALSE (ed::MoveBreak (b, 1, 5.0, error));  // the start stays
    b[1].fixed = true;
    EXPECT_FALSE (ed::DeleteBreak (b, 7, error));
    b[1].fixed = false;
    ASSERT_TRUE (ed::DeleteBreak (b, 7, error));
    EXPECT_FALSE (ed::DeleteBreak (b, 3, error));
}

TEST (EditPathProfile, CurveLengthFromK)
{
    auto b = Crest ();
    // 7 % change at K = 3 m per % asks for 21 m.
    EXPECT_NEAR (ed::CurveForK (b, 1, 3.0), 21.0, 1e-9);
    std::string error;
    ASSERT_TRUE (ed::SetCurve (b, 2, 21.0, error)) << error;
    EXPECT_FALSE (ed::SetCurve (b, 2, 200.0, error));
    EXPECT_FALSE (ed::SetCurve (b, 1, 5.0, error)); // not at an end
}

TEST (EditPathProfile, RestationFollowsAnchorsAndScalesTheRest)
{
    std::vector<ed::GradeBreak> b (4);
    b[0] = { 1, 0.0, 10.0 };
    b[1] = { 2, 20.0, 11.0 };
    b[2] = { 3, 60.0, 12.0, 0.0, true, 99 };
    b[3] = { 4, 100.0, 13.0 };
    std::string error;
    ASSERT_TRUE (ed::Restation (b, 100.0, 50.0, { { 99, 45.0 } }, error)) << error;
    EXPECT_NEAR (b[1].station, 10.0, 1e-12);
    EXPECT_NEAR (b[2].station, 45.0, 1e-12);
    EXPECT_NEAR (b[3].station, 50.0, 1e-12);
    EXPECT_NEAR (b[2].z, 12.0, 1e-12);                                     // heights never move with the plan
    EXPECT_FALSE (ed::Restation (b, 50.0, 40.0, { { 99, 45.0 } }, error)); // anchor past the end
}

TEST (EditPathProfile, ProfileFromTerrainSamples)
{
    std::vector<std::pair<double, double>> samples;
    for (int s = 0; s <= 40; ++s)
        samples.push_back ({ s, s <= 20 ? 0.1 * s : 2.0 - 0.05 * (s - 20) + (s == 30 ? 0.01 : 0.0) });
    ed::Id next = 10;
    const auto b = ed::ProfileFromSamples (samples, 0.05, next);
    ASSERT_EQ (b.size (), 3u);
    EXPECT_NEAR (b[1].station, 20.0, 1e-12);
    EXPECT_EQ (b[0].id, 10u);
    EXPECT_EQ (next, 13u);
    std::string error;
    EXPECT_TRUE (ed::ValidateProfile (b, 40.0, error)) << error;
}
