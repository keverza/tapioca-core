#include "Edit/EditWorkingPlane.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace ed = geomsrv::edit;

TEST (EditWorkingPlane, WorldXYHasAnExplicitDatumAndSnapAdapter)
{
    const auto plane = ed::WorldXY (103.25);
    ASSERT_TRUE (ed::ValidPlane (plane));
    EXPECT_TRUE (ed::IsWorldXY (plane));
    const auto local = ed::ToLocal (plane, { 4, 6, 105.25 });
    EXPECT_EQ (local, (ed::Vec3 { 4, 6, 2 }));
    EXPECT_EQ (ed::ToWorld (plane, local), (ed::Vec3 { 4, 6, 105.25 }));
    EXPECT_EQ (ed::ProjectToPlane (ed::SnapPlane (plane), { 4, 6, -10 }), (ed::Vec3 { 4, 6, 103.25 }));
}

TEST (EditWorkingPlane, ObliqueFramesRoundTripWithoutBecomingWorldXY)
{
    auto p = ed::WorldXY (12);
    p.source = ed::PlaneSource::Explicit;
    p.u = { 0, 1, 0 }, p.v = { 0, 0, 1 }, p.normal = { 1, 0, 0 };
    ASSERT_TRUE (ed::ValidPlane (p));
    EXPECT_FALSE (ed::IsWorldXY (p));
    const ed::Vec3 world { 7, 8, 9 };
    EXPECT_NEAR (ed::Distance (ed::ToWorld (p, ed::ToLocal (p, world)), world), 0, 1e-12);
    ed::Vec3 hit;
    ASSERT_TRUE (ed::HitPlane (p, { 5, 8, 9 }, { -2, 0, 0 }, hit));
    EXPECT_EQ (hit, (ed::Vec3 { 0, 8, 9 }));
}

TEST (EditWorkingPlane, RayHitsRefuseEdgeOnBehindAndInvalidInputWithoutOutput)
{
    const auto p = ed::WorldXY (5);
    ed::Vec3 out { 1, 2, 3 };
    for (const auto direction : { ed::Vec3 { 1, 0, 0 }, ed::Vec3 { 0, 0, 1 }, ed::Vec3 {} }) {
        EXPECT_FALSE (ed::HitPlane (p, { 0, 0, 10 }, direction, out));
        EXPECT_EQ (out, (ed::Vec3 { 1, 2, 3 }));
    }
    ASSERT_TRUE (ed::HitPlane (p, { 4, 6, 10 }, { 0, 0, -2 }, out));
    EXPECT_EQ (out, (ed::Vec3 { 4, 6, 5 }));
}

TEST (EditWorkingPlane, InvalidOrLeftHandedFramesCannotReplaceAPlane)
{
    const auto original = ed::WorldXY (5);
    auto p = original;
    std::string error;
    for (int i = 0; i < 4; ++i) {
        auto bad = original;
        if (i == 0)
            bad.origin.z = std::numeric_limits<double>::quiet_NaN ();
        if (i == 1)
            bad.u = {};
        if (i == 2)
            bad.v = bad.u;
        if (i == 3)
            bad.normal = { 0, 0, -1 };
        EXPECT_FALSE (ed::ChangePlane (p, bad, false, error));
        EXPECT_TRUE (ed::SamePlane (p, original));
        EXPECT_FALSE (error.empty ());
    }
}

TEST (EditWorkingPlane, APlaneLockRefusesAutomaticButNotExplicitChanges)
{
    auto p = ed::WorldXY (5);
    p.locked = true;
    const auto original = p;
    auto next = ed::WorldXY (12);
    next.locked = true;
    std::string error;
    EXPECT_FALSE (ed::ChangePlane (p, next, true, error));
    EXPECT_TRUE (ed::SamePlane (p, original));
    EXPECT_TRUE (ed::ChangePlane (p, next, false, error));
    EXPECT_TRUE (error.empty ());
    EXPECT_DOUBLE_EQ (p.origin.z, 12);
}
