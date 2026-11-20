// Tests for SunStudy/SunStudyWinding.
//
// The bar this code has to clear is asymmetric: a MISSED flip leaves a study
// wrong in a way a user can see (a sunlit facade reported dark), while a WRONG
// flip inverts a surface that was already correct and is far harder to notice.
// So most of these tests assert that something is left alone.

#include <vector>

#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyWinding.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// An axis-aligned box as 12 triangles, wound outward.
struct Box {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
};

Box MakeBox (double size, double originX = 0.0, double originY = 0.0, double originZ = 0.0)
{
    Box box;
    const double s = size;
    const double x = originX, y = originY, z = originZ;
    box.vertices = {
        x, y, z,     x + s, y, z,     x + s, y + s, z,     x, y + s, z,
        x, y, z + s, x + s, y, z + s, x + s, y + s, z + s, x, y + s, z + s,
    };
    box.triangles = {
        0, 3, 2, 0, 2, 1, // bottom, normal -Z
        4, 5, 6, 4, 6, 7, // top,    normal +Z
        0, 1, 5, 0, 5, 4, // -Y
        1, 2, 6, 1, 6, 5, // +X
        2, 3, 7, 2, 7, 6, // +Y
        3, 0, 4, 3, 4, 7, // -X
    };
    return box;
}

void FlipAll (std::vector<uint32_t>& triangles)
{
    for (size_t f = 0; f * 3 + 2 < triangles.size (); ++f)
        std::swap (triangles[f * 3 + 1], triangles[f * 3 + 2]);
}

std::vector<uint32_t> Orient (const Box& box, WindingReport& report, const uint32_t* groups = nullptr)
{
    return kernels::OrientOutward (box.vertices.data (), box.vertices.size () / 3, box.triangles.data (),
                                   box.triangles.size () / 3, groups, groups != nullptr ? box.triangles.size () / 3 : 0,
                                   report);
}

} // namespace

TEST (SunStudyWinding, AnOutwardBoxHasPositiveVolume)
{
    const Box box = MakeBox (2.0);
    std::vector<uint32_t> faces (box.triangles.size () / 3);
    for (size_t i = 0; i < faces.size (); ++i)
        faces[i] = static_cast<uint32_t> (i);

    EXPECT_NEAR (SignedVolume (box.vertices.data (), box.triangles.data (), faces.data (), faces.size ()), 8.0, 1e-9);
}

TEST (SunStudyWinding, AnInwardBoxHasNegativeVolume)
{
    Box box = MakeBox (2.0);
    FlipAll (box.triangles);
    std::vector<uint32_t> faces (box.triangles.size () / 3);
    for (size_t i = 0; i < faces.size (); ++i)
        faces[i] = static_cast<uint32_t> (i);

    EXPECT_NEAR (SignedVolume (box.vertices.data (), box.triangles.data (), faces.data (), faces.size ()), -8.0, 1e-9);
}

TEST (SunStudyWinding, AnAlreadyOutwardBoxIsLeftAlone)
{
    const Box box = MakeBox (2.0);
    WindingReport report;
    const std::vector<uint32_t> oriented = Orient (box, report);

    EXPECT_EQ (oriented, box.triangles);
    EXPECT_EQ (report.groups, 1u);
    EXPECT_EQ (report.closed, 1u);
    EXPECT_EQ (report.flipped, 0u);
}

TEST (SunStudyWinding, AnInwardBoxIsFlipped)
{
    Box box = MakeBox (2.0);
    FlipAll (box.triangles);

    WindingReport report;
    const std::vector<uint32_t> oriented = Orient (box, report);

    EXPECT_EQ (report.closed, 1u);
    EXPECT_EQ (report.flipped, 1u);
    EXPECT_EQ (report.flippedTriangles, 12u);

    // And the result now encloses a positive volume.
    std::vector<uint32_t> faces (12);
    for (size_t i = 0; i < 12; ++i)
        faces[i] = static_cast<uint32_t> (i);
    EXPECT_NEAR (SignedVolume (box.vertices.data (), oriented.data (), faces.data (), 12), 8.0, 1e-9);
}

TEST (SunStudyWinding, ABoxMissingAFaceIsNotClosedSoIsLeftAlone)
{
    Box box = MakeBox (2.0);
    FlipAll (box.triangles);
    box.triangles.resize (box.triangles.size () - 3); // drop one triangle

    WindingReport report;
    const std::vector<uint32_t> oriented = Orient (box, report);

    EXPECT_EQ (oriented, box.triangles);
    EXPECT_EQ (report.closed, 0u);
    EXPECT_EQ (report.flipped, 0u);
}

// ⚠️ THE CENTRING TEST. At national-grid coordinates the triple product runs to
// ~1e17 while the volume is ~1e0, and without centring float64 cancellation
// makes the SIGN -- the entire answer -- noise.
TEST (SunStudyWinding, TheSignSurvivesSurveyCoordinates)
{
    Box box = MakeBox (2.0, 624000.0, 6172000.0, 120.0);
    FlipAll (box.triangles);

    WindingReport report;
    Orient (box, report);

    EXPECT_EQ (report.closed, 1u);
    EXPECT_EQ (report.flipped, 1u) << "the inward box was not detected at survey coordinates";
}

TEST (SunStudyWinding, AnOutwardBoxAtSurveyCoordinatesIsStillLeftAlone)
{
    const Box box = MakeBox (2.0, 624000.0, 6172000.0, 120.0);
    WindingReport report;
    const std::vector<uint32_t> oriented = Orient (box, report);

    EXPECT_EQ (report.closed, 1u);
    EXPECT_EQ (report.flipped, 0u);
    EXPECT_EQ (oriented, box.triangles);
}
