#include "SunStudy/SunStudySolarExposure.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace evp::sunstudy;

namespace {
geomsrv::Mesh Square (double x, double y, double z, double size)
{
    geomsrv::Mesh mesh;
    mesh.vertices = { x, y, z, x + size, y, z, x + size, y + size, z, x, y + size, z };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    return mesh;
}

std::vector<SolarCutTriangle> Pieces (const geomsrv::Mesh& mesh, uint32_t m)
{
    std::vector<SolarCutTriangle> pieces;
    for (size_t t = 0; t < mesh.TriangleCount (); ++t) {
        SolarCutTriangle triangle;
        triangle.mesh = m;
        for (size_t c = 0; c < 3; ++c)
            std::copy_n (&mesh.vertices[3ull * mesh.triangles[3 * t + c]], 3, triangle.v[c]);
        pieces.push_back (triangle);
    }
    return pieces;
}

double Area (const std::vector<SolarCutTriangle>& triangles)
{
    double sum = 0.0;
    for (const auto& t : triangles) {
        double a[3], b[3];
        for (int k = 0; k < 3; ++k) {
            a[k] = t.v[1][k] - t.v[0][k];
            b[k] = t.v[2][k] - t.v[0][k];
        }
        const double n[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        sum += 0.5 * std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    return sum;
}

struct Fixture {
    geomsrv::Snapshot scene;
    SolarCutResult cut;
    std::vector<SunStep> suns;
    Fixture ()
    {
        scene.meshes = { Square (0, 0, 0, 4), Square (1, 1, 2, 2) };
        cut.removed = Pieces (scene.meshes[1], 1);
        cut.kept.emplace_back (1, geomsrv::Mesh {});
        SunStep sun;
        sun.direction[2] = 1;
        sun.time = { 12, 30 };
        suns = { sun };
    }
    bool Run (SolarExposureResult& out)
    {
        return ComputeSolarExposure (scene, cut, { 1, 1, 1 }, { 0, 1 }, suns, out);
    }
};
} // namespace

TEST (SunStudySolarExposure, ProjectsTheRealBoundaryWithoutACellGrid)
{
    Fixture f;
    f.suns[0].direction[0] = 0.3; // exact oblique displacement: 0.6 m
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 4.0, 1.0e-6);
    EXPECT_EQ (out.boundary.size (), 4u); // no receiver or blocker tessellation seam
    double minX = 100;
    for (const auto& t : out.triangles)
        for (const auto& p : t.v) {
            minX = (std::min) (minX, p[0]);
            EXPECT_DOUBLE_EQ (p[2], 0.0);
        }
    EXPECT_NEAR (minX, 0.4, 1.0e-6);
}

TEST (SunStudySolarExposure, RemainingStackedBlockerKeepsARealShadowHole)
{
    Fixture f;
    f.scene.meshes.push_back (Square (1.5, 1.5, 1, 1));
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 3.0, 1.0e-6);
    EXPECT_EQ (out.boundary.size (), 8u);
    const double origin[3] = { 2, 2, 10 }, down[3] = { 0, 0, -1 };
    EXPECT_EQ (ReadSolarExposure (out, origin, down), -1); // never fill a genuine remaining shadow
}

TEST (SunStudySolarExposure, SeveralSunsUnionOnceButKeepTheirHoverTimes)
{
    Fixture f;
    SunStep later = f.suns[0];
    later.time = { 13, 0 };
    later.direction[0] = 0.5;
    f.suns.push_back (later);
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 6.0, 1.0e-6);
    const double down[3] = { 0, 0, -1 }, first[3] = { 2.5, 2, 10 }, second[3] = { 0.5, 2, 10 },
                 both[3] = { 1.5, 2, 10 };
    EXPECT_EQ (ReadSolarExposure (out, first, down), 750);
    EXPECT_EQ (ReadSolarExposure (out, second, down), 780);
    EXPECT_EQ (ReadSolarExposure (out, both, down), 750);
    // The preview's x-ray semantics inspect the receiver even through the
    // original blocker. There is no ordinary-model visibility test here.
}

TEST (SunStudySolarExposure, ReceiverOpeningsAndRemovedReceiverPartsAreNotPainted)
{
    Fixture f;
    f.cut.kept.emplace_back (0, Square (0, 0, 0, 2));
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 1.0, 1.0e-6);
    f.scene.meshes[0].triangles = { 0, 1, 2 }; // the other half is not a measured surface
    f.cut.kept.erase (f.cut.kept.begin () + 1);
    ASSERT_TRUE (ComputeSolarExposure (f.scene, f.cut, { 1, 1 }, { 0 }, f.suns, out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 2.0, 1.0e-6);
}

TEST (SunStudySolarExposure, BelowPlaneAndBehindSurfaceSunsCannotWinLight)
{
    Fixture f;
    for (size_t v = 2; v < f.scene.meshes[1].vertices.size (); v += 3)
        f.scene.meshes[1].vertices[v] = -2;
    f.cut.removed = Pieces (f.scene.meshes[1], 1);
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out));
    EXPECT_TRUE (out.triangles.empty ());
    f.suns[0].direction[2] = -1;
    ASSERT_TRUE (f.Run (out));
    EXPECT_TRUE (out.triangles.empty ());
}

TEST (SunStudySolarExposure, VerticalReceiverAndTranslatedCoordinatesKeepTheSameArea)
{
    Fixture f;
    // Rotate floor to a south-facing wall, preserving winding; translate far
    // from the origin. Sun along +y and +z, projected roof is a 2 x 2 square.
    for (auto& mesh : f.scene.meshes)
        for (size_t v = 0; v < mesh.vertices.size (); v += 3) {
            const double y = mesh.vertices[v + 1], z = mesh.vertices[v + 2];
            mesh.vertices[v] += 100000;
            mesh.vertices[v + 1] = z + 200000;
            mesh.vertices[v + 2] = -y + 300000;
        }
    f.cut.removed = Pieces (f.scene.meshes[1], 1);
    f.suns[0].direction[1] = 1;
    f.suns[0].direction[2] = 0.1;
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 4.0, 1.0e-6);
}

TEST (SunStudySolarExposure, RefusalAndCancellationDiscardAllPartialOutput)
{
    Fixture f;
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out));
    ASSERT_FALSE (out.triangles.empty ());
    EXPECT_FALSE (ComputeSolarExposure (f.scene, f.cut, { 1, 1 }, { 0, 1 }, f.suns, out, {}, 1));
    EXPECT_TRUE (out.triangles.empty ());
    EXPECT_TRUE (out.steps.empty ());
    EXPECT_FALSE (out.error.empty ());
    EXPECT_FALSE (ComputeSolarExposure (f.scene, f.cut, { 1, 1 }, { 0, 1 }, f.suns, out, [] { return true; }));
    EXPECT_TRUE (out.triangles.empty ());
}

TEST (SunStudySolarExposure, SourceRetessellationAndDuplicateReceiversDoNotChangeTheRegion)
{
    Fixture f;
    SolarExposureResult first, second;
    ASSERT_TRUE (f.Run (first)) << first.error;
    f.scene.meshes[0].triangles = { 0, 1, 3, 1, 2, 3 };
    f.scene.meshes[1].triangles = { 0, 1, 3, 1, 2, 3 };
    f.cut.removed = Pieces (f.scene.meshes[1], 1);
    ASSERT_TRUE (ComputeSolarExposure (f.scene, f.cut, { 1, 1 }, { 0, 1, 0, 1 }, f.suns, second)) << second.error;
    EXPECT_NEAR (Area (first.triangles), Area (second.triangles), 1.0e-8);
    EXPECT_EQ (first.boundary.size (), second.boundary.size ());
}

TEST (SunStudySolarExposure, StraddlingBlockerOnlyProjectsItsSunwardPart)
{
    Fixture f;
    f.cut.removed.resize (1);
    f.cut.removed[0] = { { { 1, 1, -1 }, { 3, 1, 1 }, { 1, 3, 1 } }, 1 };
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 1.5, 2.0e-6);
}

TEST (SunStudySolarExposure, CoincidentShadowSliversAreNotPaintedButNarrowRealRegionsStay)
{
    // #168: a remaining blocker whose shadow edge almost meets the removed
    // part's leaves a hairline Difference; drawn, it was a stray yellow line.
    Fixture f;
    f.scene.meshes.push_back (Square (1.0005, 1, 1, 2));
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_TRUE (out.triangles.empty ());
    EXPECT_TRUE (out.boundary.empty ());
    // A 5 cm strip is real won sun and keeps its exact area.
    f.scene.meshes.back () = Square (1.05, 1, 1, 2);
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 0.05 * 2.0, 1.0e-6);
}

TEST (SunStudySolarExposure, LargeSceneStaysWithinTheDefaultBudget)
{
    // Twenty measured receivers in the removed part's shadow, thirty suns and
    // a dense scene away from the cut. Projecting every blocker per receiver
    // and sun (20 x 30 x 120k) exhausted the 20M work budget, so complex
    // scenes showed no lit surface at all (#168).
    Fixture f;
    std::vector<uint8_t> blocks { 1, 1 };
    std::vector<uint32_t> receivers;
    uint32_t global = 4;
    for (int i = 0; i < 20; ++i) {
        f.scene.meshes.push_back (Square (1.5 + 0.1 * (i % 5), 1.5 + 0.1 * (i / 5), 0.09 * i, 0.1));
        blocks.push_back (0);
        receivers.push_back (global);
        receivers.push_back (global + 1);
        global += 2;
    }
    for (int i = 0; i < 400; ++i) { // 120k blocker triangles
        geomsrv::Mesh dense;
        for (int k = 0; k < 150; ++k) {
            const double x = -300.0 - i, y = 0.01 * k;
            const uint32_t base = uint32_t (dense.vertices.size () / 3);
            dense.vertices.insert (dense.vertices.end (),
                                   { x, y, 5, x + 0.5, y, 5, x + 0.5, y + 0.01, 5, x, y + 0.01, 5 });
            dense.triangles.insert (dense.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
        f.scene.meshes.push_back (dense);
        blocks.push_back (1);
    }
    for (int s = 1; s < 30; ++s) {
        SunStep sun = f.suns[0];
        sun.time = { 12, 30 + s };
        sun.direction[0] = 0.001 * s;
        f.suns.push_back (sun);
    }
    SolarExposureResult out;
    ASSERT_TRUE (ComputeSolarExposure (f.scene, f.cut, blocks, receivers, f.suns, out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 20 * 0.1 * 0.1, 1.0e-6);
}

TEST (SunStudySolarExposure, ManyDistinctReceiverPlanesDoNotSpendTheBudget)
{
    // A measured terrain (or a solid-operated body) is thousands of triangles
    // each in its own plane. Searched pairwise, 6000 of them cost 18M work
    // units before a shadow was cast and the lit surface failed (#168).
    Fixture f;
    geomsrv::Mesh terrain;
    for (int i = 0; i <= 60; ++i)
        for (int j = 0; j <= 50; ++j)
            terrain.vertices.insert (terrain.vertices.end (),
                                     { 100.0 + i, double (j), 0.37 * std::sin (1.3 * i + 0.7 * j * j) });
    std::vector<uint32_t> receivers { 0, 1 };
    for (uint32_t i = 0; i < 60; ++i)
        for (uint32_t j = 0; j < 50; ++j) {
            const uint32_t a = i * 51 + j, b = a + 51;
            terrain.triangles.insert (terrain.triangles.end (), { a, b, b + 1, a, b + 1, a + 1 });
        }
    for (uint32_t t = 0; t < terrain.TriangleCount (); ++t)
        receivers.push_back (4 + t);
    f.scene.meshes.push_back (terrain);
    SolarExposureResult out;
    ASSERT_TRUE (ComputeSolarExposure (f.scene, f.cut, { 1, 1, 0 }, receivers, f.suns, out, {}, 2000000)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 4.0, 1.0e-6);
    EXPECT_TRUE (out.note.empty ()) << out.note;
    EXPECT_EQ (out.surfacesShaded, 1u); // only the floor can be shaded by the removed square
}

TEST (SunStudySolarExposure, ARunOutBudgetKeepsTheFinishedSurfacesAndSaysSo)
{
    // Twenty receivers under the removed square; a budget that lasts for some
    // of them shows those, exact, and says the rest are missing (#168).
    Fixture f;
    std::vector<uint8_t> blocks { 1, 1 };
    std::vector<uint32_t> receivers;
    uint32_t global = 4;
    for (int i = 0; i < 20; ++i) {
        f.scene.meshes.push_back (Square (1.5 + 0.1 * (i % 5), 1.5 + 0.1 * (i / 5), 0.09 * i, 0.1));
        blocks.push_back (0);
        receivers.push_back (global);
        receivers.push_back (global + 1);
        global += 2;
    }
    SolarExposureResult whole;
    ASSERT_TRUE (ComputeSolarExposure (f.scene, f.cut, blocks, receivers, f.suns, whole)) << whole.error;
    ASSERT_EQ (whole.surfacesDone, 20u);
    SolarExposureResult part;
    ASSERT_TRUE (ComputeSolarExposure (f.scene, f.cut, blocks, receivers, f.suns, part, {}, whole.work / 2))
        << part.error;
    EXPECT_TRUE (part.partial);
    EXPECT_FALSE (part.note.empty ());
    EXPECT_GT (part.surfacesDone, 0u);
    EXPECT_LT (part.surfacesDone, 20u);
    EXPECT_NEAR (Area (part.triangles), double (part.surfacesDone) * 0.1 * 0.1, 1.0e-6);
}

TEST (SunStudySolarExposure, BandsPartitionTheSweptUnionByMinutesGained)
{
    // Three suns 30 min apart slide the removed square's shadow 0, 0.5 and 1 m.
    // Swept between them, with each end step standing for its outer quarter
    // hour: the middle metre is lit 90 min, the strips beside it 75, the ends
    // 45 (#168). Bands are 15 min wide over a 90 min slice.
    Fixture f;
    SunStep second = f.suns[0], third = f.suns[0];
    second.time = { 13, 0 };
    second.direction[0] = 0.25;
    third.time = { 13, 30 };
    third.direction[0] = 0.5;
    f.suns.push_back (second);
    f.suns.push_back (third);
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 6.0, 1.0e-6);
    EXPECT_DOUBLE_EQ (out.bandMinutes, 15.0);
    ASSERT_EQ (out.MostLevel (), 6);
    double byLevel[7] = {};
    for (const auto& band : out.bands) {
        ASSERT_GE (band.level, 1);
        ASSERT_LE (band.level, 6);
        byLevel[band.level] += Area (band.triangles);
    }
    EXPECT_NEAR (byLevel[3], 2.0, 1.0e-6);
    EXPECT_NEAR (byLevel[5], 2.0, 1.0e-6);
    EXPECT_NEAR (byLevel[6], 2.0, 1.0e-6);
    EXPECT_NEAR (byLevel[1] + byLevel[2] + byLevel[4], 0.0, 1.0e-9);
    ASSERT_EQ (out.steps.size (), 3u);
    for (const auto& step : out.steps)
        EXPECT_EQ (step.boundary.size (), 4u) << step.minute; // each measured step keeps its own perimeter
}

TEST (SunStudySolarExposure, AThinStripSweepsTheGapBetweenStepsInsteadOfACombOfStrips)
{
    // A 10 cm bar's shadow jumps a metre between two steps. Sampled, the slice
    // was two separate strips with an unlit metre between them -- the jagged
    // comb seen live (#168). The sun crosses that metre: it is one region.
    Fixture f;
    f.scene.meshes[1] = Square (2, 1, 2, 2);
    for (size_t v = 0; v < f.scene.meshes[1].vertices.size (); v += 3)
        if (f.scene.meshes[1].vertices[v] > 2.5)
            f.scene.meshes[1].vertices[v] = 2.1;
    f.cut.removed = Pieces (f.scene.meshes[1], 1);
    SunStep later = f.suns[0];
    later.time = { 13, 0 };
    later.direction[0] = 0.5;
    f.suns.push_back (later);
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    EXPECT_NEAR (Area (out.triangles), 1.1 * 2.0, 1.0e-6);
    // One rectangle, no teeth: every perimeter edge lies on its four sides
    // (collinear joins where the end steps meet the sweep may split a side).
    const auto onSide = [] (double x, double y) {
        return std::abs (x - 1.0) < 1.0e-6 || std::abs (x - 2.1) < 1.0e-6 || std::abs (y - 1.0) < 1.0e-6 ||
               std::abs (y - 3.0) < 1.0e-6;
    };
    for (const auto& edge : out.boundary) {
        EXPECT_TRUE (onSide (edge[0], edge[1]) && onSide (edge[3], edge[4]));
        EXPECT_TRUE (std::abs (edge[0] - edge[3]) < 1.0e-6 || std::abs (edge[1] - edge[4]) < 1.0e-6);
    }
    ASSERT_EQ (out.steps.size (), 2u); // the clock-step view still shows each strip alone
    EXPECT_NEAR (Area (out.steps[0].triangles) + Area (out.steps[1].triangles), 0.4, 1.0e-6);
    const double down[3] = { 0, 0, -1 }, gap[3] = { 1.5, 2, 10 };
    const auto runs = ReadSolarExposureRuns (out, gap, down);
    ASSERT_EQ (runs.size (), 1u);
    EXPECT_DOUBLE_EQ (runs[0][0], 750.0);
    EXPECT_DOUBLE_EQ (runs[0][1], 780.0);
}

TEST (SunStudySolarExposure, HoverReadsWhenThatPlaceIsLit)
{
    Fixture f;
    SunStep later = f.suns[0];
    later.time = { 13, 0 };
    later.direction[0] = 0.5;
    f.suns.push_back (later);
    SolarExposureResult out;
    ASSERT_TRUE (f.Run (out)) << out.error;
    const double down[3] = { 0, 0, -1 }, first[3] = { 2.5, 2, 10 }, both[3] = { 1.5, 2, 10 }, off[3] = { 3.5, 2, 10 };
    using Runs = std::vector<std::array<double, 2>>;
    EXPECT_EQ (ReadSolarExposureRuns (out, first, down), (Runs { { 735.0, 780.0 } }));
    EXPECT_EQ (ReadSolarExposureRuns (out, both, down), (Runs { { 735.0, 795.0 } }));
    EXPECT_TRUE (ReadSolarExposureRuns (out, off, down).empty ());
}
