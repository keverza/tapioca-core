#include "SunStudy/SunStudySolarExposure.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

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
