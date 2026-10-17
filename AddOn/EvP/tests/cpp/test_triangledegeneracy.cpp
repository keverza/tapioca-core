#include "Geometry/TriangleDegeneracy.hpp"
#include <gtest/gtest.h>

TEST (TriangleDegeneracy, CapturedCollinearFanPiecesHaveNoSurfaceArea)
{
    const double a[] = { 17.941956492089474, 11.087750888585974, 3.0 };
    const double b[] = { 17.941956492089474, 11.337750888585974, 3.0 };
    const double c[] = { 17.941956492089474, 5.520580431635307, 3.0 };
    EXPECT_TRUE (geomsrv::IsRoundoffZeroTriangle (a, b, c));
    const double d[] = { 17.991956492089475, 13.040286674170414, 3.0 };
    const double e[] = { 17.691956492089474, 13.040286674170414, 3.0000000000000004 };
    const double f[] = { 13.171426334379099, 13.040286674170414, 3.0 };
    EXPECT_TRUE (geomsrv::IsRoundoffZeroTriangle (d, e, f));
}

TEST (TriangleDegeneracy, KeepsSmallPositiveAreaAndRefusesToHideInvalidNumbers)
{
    const double a[] = { 0, 0, 0 }, b[] = { 1e-8, 0, 0 }, c[] = { 0, 1e-8, 0 };
    EXPECT_FALSE (geomsrv::IsRoundoffZeroTriangle (a, b, c));
    const double thin[] = { 1, 1e-10, 0 }, end[] = { 1, 0, 0 };
    EXPECT_FALSE (geomsrv::IsRoundoffZeroTriangle (a, end, thin));
    const double bad[] = { NAN, 0, 0 };
    EXPECT_FALSE (geomsrv::IsRoundoffZeroTriangle (a, end, bad));
    EXPECT_TRUE (geomsrv::IsRoundoffZeroTriangle (a, a, a));
}
