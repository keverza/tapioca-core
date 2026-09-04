// Tests for SunStudy/SunStudyWinding.
//
// The bar this code has to clear is asymmetric: a MISSED flip leaves a study
// wrong in a way a user can see (a sunlit facade reported dark), while a WRONG
// flip inverts a surface that was already correct and is far harder to notice.
// So most of these tests assert that something is left alone.

#include <vector>

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
    return OrientOutward (box.vertices.data (), box.vertices.size () / 3, box.triangles.data (),
                          box.triangles.size () / 3, groups, report);
}

} // namespace

TEST (SunStudyWinding, EmptyInputIsHandled)
{
    WindingReport report;
    EXPECT_TRUE (OrientOutward (nullptr, 0, nullptr, 0, nullptr, report).empty ());
    EXPECT_EQ (report.groups, 0u);
}

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

TEST (SunStudyWinding, FlippingIsIdempotent)
{
    Box box = MakeBox (2.0);
    FlipAll (box.triangles);

    WindingReport first;
    const std::vector<uint32_t> once = Orient (box, first);

    WindingReport second;
    const std::vector<uint32_t> twice =
        OrientOutward (box.vertices.data (), box.vertices.size () / 3, once.data (), once.size () / 3, nullptr, second);
    EXPECT_EQ (once, twice);
    EXPECT_EQ (second.flipped, 0u);
}

// ⚠️ AN OPEN SURFACE HAS NO INSIDE. A terrain mesh or a single slab face is the
// common case in a real snapshot, and guessing at its winding would invert
// surfaces that were already right.
TEST (SunStudyWinding, AnOpenSurfaceIsNeverFlipped)
{
    // One triangle: every edge is used once, so it cannot be closed.
    std::vector<double> vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    std::vector<uint32_t> triangles = { 0, 2, 1 }; // deliberately "wrong" way up

    WindingReport report;
    const std::vector<uint32_t> oriented = OrientOutward (vertices.data (), 3, triangles.data (), 1, nullptr, report);

    EXPECT_EQ (oriented, triangles);
    EXPECT_EQ (report.groups, 1u);
    EXPECT_EQ (report.closed, 0u);
    EXPECT_EQ (report.flipped, 0u);
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

// ⚠️ WINDING IS CONSISTENT ONLY WITHIN AN ELEMENT. Testing the merged snapshot
// as one body would find it open -- boxes do not share edges -- and so would
// never flip anything at all.
TEST (SunStudyWinding, EachElementIsJudgedSeparately)
{
    Box good = MakeBox (2.0);
    Box bad = MakeBox (2.0, 10.0);
    FlipAll (bad.triangles);

    std::vector<double> vertices = good.vertices;
    std::vector<uint32_t> triangles = good.triangles;
    const uint32_t base = static_cast<uint32_t> (good.vertices.size () / 3);
    vertices.insert (vertices.end (), bad.vertices.begin (), bad.vertices.end ());
    for (uint32_t index : bad.triangles)
        triangles.push_back (base + index);

    std::vector<uint32_t> groups (24);
    for (size_t f = 0; f < 24; ++f)
        groups[f] = (f < 12) ? 0u : 1u;

    WindingReport report;
    const std::vector<uint32_t> oriented =
        OrientOutward (vertices.data (), vertices.size () / 3, triangles.data (), 24, groups.data (), report);

    EXPECT_EQ (report.groups, 2u);
    EXPECT_EQ (report.closed, 2u);
    EXPECT_EQ (report.flipped, 1u);
    EXPECT_EQ (report.flippedTriangles, 12u);

    // The element that was already right is untouched.
    for (size_t i = 0; i < 36; ++i)
        EXPECT_EQ (oriented[i], triangles[i]);
}

TEST (SunStudyWinding, TwoElementsMergedWithoutGroupsReadAsOpen)
{
    Box a = MakeBox (2.0);
    Box b = MakeBox (2.0, 10.0);

    std::vector<double> vertices = a.vertices;
    std::vector<uint32_t> triangles = a.triangles;
    const uint32_t base = static_cast<uint32_t> (a.vertices.size () / 3);
    vertices.insert (vertices.end (), b.vertices.begin (), b.vertices.end ());
    for (uint32_t index : b.triangles)
        triangles.push_back (base + index);

    WindingReport report;
    OrientOutward (vertices.data (), vertices.size () / 3, triangles.data (), 24, nullptr, report);

    // Two disjoint boxes as ONE group: every edge is still shared by exactly
    // two faces, so this one does prove closed -- and the combined volume is
    // positive, so nothing flips. The point is that the group id is what makes
    // the judgement meaningful, not that the merge is rejected.
    EXPECT_EQ (report.groups, 1u);
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

// Adjacency is a question about positions: the snapshot merges elements without
// sharing vertices, so an index-based test would find no shared edges at all.
TEST (SunStudyWinding, AdjacencyIsByPositionNotByIndex)
{
    Box box = MakeBox (2.0);
    FlipAll (box.triangles);

    // Duplicate every corner so no two faces share an index.
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    for (size_t i = 0; i < box.triangles.size (); ++i) {
        const double* p = box.vertices.data () + static_cast<size_t> (box.triangles[i]) * 3;
        triangles.push_back (static_cast<uint32_t> (vertices.size () / 3));
        vertices.push_back (p[0]);
        vertices.push_back (p[1]);
        vertices.push_back (p[2]);
    }

    WindingReport report;
    OrientOutward (vertices.data (), vertices.size () / 3, triangles.data (), triangles.size () / 3, nullptr, report);

    EXPECT_EQ (report.closed, 1u) << "unshared indices defeated the closed-surface proof";
    EXPECT_EQ (report.flipped, 1u);
}

TEST (SunStudyWinding, OutOfRangeIndicesDoNotCrashOrProveClosed)
{
    std::vector<double> vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    std::vector<uint32_t> triangles = { 0, 1, 77 };

    WindingReport report;
    const std::vector<uint32_t> oriented = OrientOutward (vertices.data (), 3, triangles.data (), 1, nullptr, report);

    EXPECT_EQ (oriented, triangles);
    EXPECT_EQ (report.groups, 0u);
    EXPECT_EQ (report.flipped, 0u);
}
