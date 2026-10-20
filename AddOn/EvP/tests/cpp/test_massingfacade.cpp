#include "ArchViz/MassingSlices.hpp"
#include "Geometry/Primitives.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <limits>

namespace ms = geomsrv::archviz::massingslices;
namespace {
ms::Input SurfaceSlab (double height = 9, double x = 0, double width = 10, double bottom = 0)
{
    ms::Input input;
    input.slab.guid = "surface:" + std::to_string (x) + ":" + std::to_string (bottom);
    input.slab.outer.xy = { x, 0, x + width, 0, x + width, 10, x, 10 };
    input.slab.bottom = bottom;
    input.slab.top = bottom + height;
    geomsrv::Mesh mesh;
    std::string error;
    EXPECT_TRUE (geomsrv::engine::MakeBox ({ x + width / 2, 5, bottom + height / 2 }, width, 10, height, mesh, error));
    input.body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
    return input;
}

ms::Input OperatedCourtyard ()
{
    auto input = SurfaceSlab (9);
    geomsrv::Mesh mesh;
    const std::array<std::array<double, 2>, 8> xy {
        { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 }, { 4, 4 }, { 6, 4 }, { 6, 6 }, { 4, 6 } }
    };
    const auto vertex = [&] (int at, double z) {
        mesh.triangles.push_back (uint32_t (mesh.VertexCount ()));
        mesh.vertices.insert (mesh.vertices.end (), { xy[at][0], xy[at][1], z });
    };
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        for (const auto& tri : { std::array<int, 3> { i, j, j + 4 }, std::array<int, 3> { i, j + 4, i + 4 } }) {
            for (int k : { 0, 1, 2 })
                vertex (tri[k], 9);
            for (int k : { 2, 1, 0 })
                vertex (tri[k], 0);
        }
        for (int ring : { 0, 4 }) {
            const int a = ring == 0 ? i : j + 4, b = ring == 0 ? j : i + 4;
            vertex (a, 0);
            vertex (b, 0);
            vertex (b, 9);
            vertex (a, 0);
            vertex (b, 9);
            vertex (a, 9);
        }
    }
    input.body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
    return input;
}
} // namespace

TEST (MassingFacade, OperatedFacadeExcludesDuplicateSharedIntersectingAndNestedFaces)
{
    const auto a = SurfaceSlab ();
    double area = 0;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ a }, area, error)) << error;
    EXPECT_NEAR (area, 360, 1e-5);
    ASSERT_TRUE (ms::Facade ({ a, a }, area, error)) << error;
    EXPECT_NEAR (area, 360, 1e-5);
    ASSERT_TRUE (ms::Facade ({ a, SurfaceSlab (9, 10) }, area, error)) << error;
    EXPECT_NEAR (area, 540, 1e-5);
    ASSERT_TRUE (ms::Facade ({ a, SurfaceSlab (9, 5) }, area, error)) << error;
    EXPECT_NEAR (area, 450, 1e-5);
    ASSERT_TRUE (ms::Facade ({ a, SurfaceSlab (3, 10) }, area, error)) << error;
    EXPECT_NEAR (area, 420, 1e-5);
    ASSERT_TRUE (ms::Facade ({ a, SurfaceSlab (3, 5, 10, 3) }, area, error)) << error;
    EXPECT_NEAR (area, 390, 1e-5);
    ASSERT_TRUE (ms::Facade ({ a, SurfaceSlab (3, 2, 6, 3) }, area, error)) << error;
    EXPECT_NEAR (area, 360, 1e-5);
}

TEST (MassingFacade, IncludesSeventyThroughNinetyDegreesAndUsesTrueInclinedArea)
{
    for (double angle : { 69.0, 69.999, 70.0, 75.0, 90.0 }) {
        auto input = SurfaceSlab (3);
        auto mesh = *input.body;
        const double radians = angle * 3.14159265358979323846 / 180;
        for (size_t i = 0; i < mesh.vertices.size (); i += 3)
            mesh.vertices[i] += mesh.vertices[i + 2] / std::tan (radians);
        // Bogus smooth normals must not change geometric slope/area.
        mesh.normals.assign (mesh.vertices.size (), 0);
        input.body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
        double area = 0;
        std::string error;
        ASSERT_TRUE (ms::Facade ({ input }, area, error)) << angle << ": " << error;
        EXPECT_NEAR (area, angle < 70 ? 60 : 60 + 60 / std::sin (radians), 1e-5) << angle;
    }
}

TEST (MassingFacade, SharedInclinedWallsAndDifferentlyTriangulatedDuplicatesAreExcluded)
{
    auto a = SurfaceSlab (3), b = SurfaceSlab (3, 10);
    const double radians = 75 * 3.14159265358979323846 / 180;
    for (auto* input : { &a, &b }) {
        auto mesh = *input->body;
        for (size_t i = 0; i < mesh.vertices.size (); i += 3)
            mesh.vertices[i] += mesh.vertices[i + 2] / std::tan (radians);
        input->body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
    }
    double area = 0;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ a, b }, area, error)) << error;
    EXPECT_NEAR (area, 120 + 60 / std::sin (radians), 1e-5);
    auto duplicate = a;
    auto mesh = *a.body;
    for (size_t i = 0; i < mesh.triangles.size (); i += 6) {
        const auto p = mesh.triangles[i], q = mesh.triangles[i + 1], r = mesh.triangles[i + 2],
                   s = mesh.triangles[i + 5];
        mesh.triangles[i] = p;
        mesh.triangles[i + 1] = q;
        mesh.triangles[i + 2] = s;
        mesh.triangles[i + 3] = q;
        mesh.triangles[i + 4] = r;
        mesh.triangles[i + 5] = s;
    }
    duplicate.body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
    ASSERT_TRUE (ms::Facade ({ a, duplicate }, area, error)) << error;
    EXPECT_NEAR (area, 60 + 60 / std::sin (radians), 1e-5);
}

TEST (MassingFacade, OverBudgetSourcesAndEmptyBodiesDoNotReturnPartialArea)
{
    double area = 123;
    std::string error;
    EXPECT_FALSE (ms::Facade (std::vector<ms::Input> (129, SurfaceSlab ()), area, error));
    EXPECT_EQ (area, 123);
    auto input = SurfaceSlab ();
    input.body = std::make_shared<const geomsrv::Mesh> ();
    EXPECT_FALSE (ms::Facade ({ input }, area, error));
    EXPECT_EQ (area, 123);
}

TEST (MassingFacade, CountsFinalSeoCutoutWallsRatherThanOriginalSlabRecord)
{
    // Completed SEO body after a through-slot subtraction; record is still 10 x 10.
    auto input = SurfaceSlab (3);
    auto left = *SurfaceSlab (3, 0, 4).body;
    const auto right = SurfaceSlab (3, 6, 4).body;
    const auto offset = uint32_t (left.VertexCount ());
    left.vertices.insert (left.vertices.end (), right->vertices.begin (), right->vertices.end ());
    for (auto index : right->triangles)
        left.triangles.push_back (index + offset);
    input.body = std::make_shared<const geomsrv::Mesh> (std::move (left));
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    EXPECT_TRUE (result.hasFacade);
    EXPECT_NEAR (result.facadeArea, 2 * (2 * 4 + 2 * 10) * 3, 1e-5);
    EXPECT_NEAR (result.rawArea, 80, 1e-6);
}

TEST (MassingFacade, FacadeBodyMeasuresSurfacesWithoutChangingPrismFloorPreview)
{
    auto input = SurfaceSlab (9);
    input.body.reset ();
    input.facadeBody = SurfaceSlab (3).body;
    ms::Result result;
    std::string error;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    EXPECT_NEAR (result.facadeArea, 120, 1e-5);
    EXPECT_EQ (result.rows.size (), 3u);
}

TEST (MassingFacade, MixedPrismAndOperatedBodyKeepCourtyardAndSharedWallExclusion)
{
    auto courtyard = SurfaceSlab (9);
    courtyard.body.reset ();
    courtyard.slab.holes.push_back ({ { 4, 4, 6, 4, 6, 6, 4, 6 }, {} });
    auto plug = SurfaceSlab (3, 4, 2);
    auto mesh = *plug.body;
    for (size_t i = 0; i < mesh.vertices.size (); i += 3)
        mesh.vertices[i + 1] = 4 + mesh.vertices[i + 1] / 5;
    plug.body = std::make_shared<const geomsrv::Mesh> (std::move (mesh));
    double area = 0;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ courtyard, plug }, area, error)) << error;
    EXPECT_NEAR (area, 408, 1e-5);
    // Same courtyard supplied as the completed SEO surface, not a record hole.
    courtyard = OperatedCourtyard ();
    ASSERT_TRUE (ms::Facade ({ courtyard }, area, error)) << error;
    EXPECT_NEAR (area, 432, 1e-5);
    ASSERT_TRUE (ms::Facade ({ courtyard, plug }, area, error)) << error;
    EXPECT_NEAR (area, 408, 1e-5);
}

TEST (MassingFacade, SurveyPlacementAndInvalidBodyAreHandledWithoutPartialArea)
{
    auto input = SurfaceSlab (3);
    auto mesh = *input.body;
    for (size_t i = 0; i < mesh.vertices.size (); i += 3) {
        mesh.vertices[i] += 700000;
        mesh.vertices[i + 1] += 6000000;
        mesh.vertices[i + 2] += 120;
    }
    input.body = std::make_shared<const geomsrv::Mesh> (mesh);
    double area = 123;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ input, input }, area, error)) << error;
    EXPECT_NEAR (area, 120, 1e-5);
    area = 123;
    mesh.triangles[0] = 999999;
    input.body = std::make_shared<const geomsrv::Mesh> (mesh);
    EXPECT_FALSE (ms::Facade ({ input }, area, error));
    EXPECT_EQ (area, 123);
    mesh.triangles[0] = 0;
    mesh.vertices[0] = std::numeric_limits<double>::quiet_NaN ();
    input.body = std::make_shared<const geomsrv::Mesh> (mesh);
    EXPECT_FALSE (ms::Facade ({ input }, area, error));
    EXPECT_EQ (area, 123);
}

TEST (MassingFacade, TwoNodeCircularPrismsUseTheirTessellatedPerimeterWithoutRewritingSources)
{
    constexpr double pi = 3.14159265358979323846;
    for (double sign : { -1.0, 1.0 }) {
        ms::Input input;
        input.slab.guid = "circle:two-nodes";
        input.slab.outer = { { -1, 0, 1, 0 }, { sign * pi, sign * pi } };
        input.slab.bottom = 2;
        input.slab.top = 11;
        const auto before = input.slab.outer;
        double area = 123;
        std::string error;
        ASSERT_TRUE (ms::Facade ({ input }, area, error)) << error;
        EXPECT_NEAR (area, 9 * 252 * std::sin (pi / 126), 2e-5);
        EXPECT_TRUE (error.empty ());
        ASSERT_TRUE (ms::Facade ({ input, input }, area, error)) << error;
        EXPECT_NEAR (area, 9 * 252 * std::sin (pi / 126), 2e-5); // Duplicate sources count once.
        EXPECT_EQ (input.slab.outer.xy, before.xy);
        EXPECT_EQ (input.slab.outer.arcs, before.arcs);
        input.slab.outer.arcs = { sign * pi }; // A semicircle plus a straight closing diameter.
        ASSERT_TRUE (ms::Facade ({ input }, area, error)) << error;
        EXPECT_NEAR (area, 9 * (126 * std::sin (pi / 126) + 2), 2e-5);
    }
}

TEST (MassingFacade, TwoNodeCircularHolesContributeFacadeAndKeepMassingRows)
{
    constexpr double pi = 3.14159265358979323846;
    ms::Input input;
    input.slab.guid = "annulus:two-node-hole";
    input.slab.outer = { { 1, 0, 0, 1, -1, 0, 0, -1 }, { pi / 2, pi / 2, pi / 2, pi / 2 } };
    input.slab.holes = { { { -0.5, 0, 0.5, 0 }, { -pi, -pi } } };
    input.slab.top = 9;
    double area = 123;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ input }, area, error)) << error;
    const double expected = 9 * (256 * std::sin (pi / 128) + 64 * std::sin (pi / 64));
    EXPECT_NEAR (area, expected, 2e-5);
    ms::Result result;
    ASSERT_TRUE (ms::Build ({ input }, {}, nullptr, result, error)) << error;
    EXPECT_TRUE (result.hasFacade);
    EXPECT_NEAR (result.facadeArea, expected, 2e-5);
    EXPECT_EQ (result.note.find ("Facade area unavailable"), std::string::npos);
    ASSERT_EQ (result.rows.size (), 3u);
    EXPECT_NEAR (result.rawArea, 3 * (64 * std::sin (pi / 64) - 8 * std::sin (pi / 32)), 2e-5);
    EXPECT_EQ (input.slab.holes.front ().xy.size (), 4u);
    EXPECT_EQ (input.slab.holes.front ().arcs, (std::vector<double> { -pi, -pi }));
}

TEST (MassingFacade, TwoNodeArcPrismWorksBesideAnUnmodifiedOperatedBody)
{
    constexpr double pi = 3.14159265358979323846;
    ms::Input circle;
    circle.slab.guid = "circle:mixed";
    circle.slab.outer = { { -1, 0, 1, 0 }, { pi, pi } };
    circle.slab.top = 9;
    const auto body = SurfaceSlab (3, 10);
    double area = 123;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ circle, body }, area, error)) << error;
    EXPECT_NEAR (area, 120 + 9 * 252 * std::sin (pi / 126), 2e-5);
}

TEST (MassingFacade, ArcIngressRefusesMalformedOrCollapsedRingsWithoutPartialArea)
{
    constexpr double pi = 3.14159265358979323846;
    const auto nan = std::numeric_limits<double>::quiet_NaN ();
    const auto inf = std::numeric_limits<double>::infinity ();
    const std::vector<geomsrv::archviz::slabslices::Ring> invalid {
        { {}, {} },
        { { 0, 0 }, {} },
        { { 0, 0, 1 }, {} },
        { { -1, 0, 1, 0 }, {} },
        { { -1, 0, 1, 0 }, { 0, 0 } },
        { { 0, 0, 0, 0 }, { pi, pi } },
        { { -1, 0, 1, 0 }, { 1e-8 } }, // Inherited straight-sweep fallback cannot form a contour.
        { { -1, 0, 1, 0 }, { pi, pi, 0 } },
        { { -1, 0, 1, 0 }, { nan, pi } },
        { { -1, 0, 1, 0 }, { pi, inf } },
        { { -1, 0, 1, 0 }, { 2 * pi, 0 } },
        { { -1, 0, 1, 0 }, { -2 * pi, 0 } },
        { { nan, 0, 1, 0 }, { pi, pi } },
        { { -1e9, 0, 1e9, 0 }, { 2 * pi - 1e-5 } } // Derived circle, unlike stored nodes, leaves world bounds.
    };
    for (const auto& ring : invalid) {
        for (bool hole : { false, true }) {
            ms::Input input;
            input.slab.outer = { { -1, 0, 1, 0 }, { pi, pi } };
            input.slab.top = 9;
            if (hole)
                input.slab.holes = { ring };
            else
                input.slab.outer = ring;
            double area = 123;
            std::string error;
            EXPECT_FALSE (ms::Facade ({ input }, area, error));
            EXPECT_EQ (area, 123);
            EXPECT_FALSE (error.empty ());
        }
    }
}

TEST (MassingFacade, CappedLargeCircularPrismsRetainBudgetsAndRefuseOverflowAtomically)
{
    constexpr double pi = 3.14159265358979323846;
    constexpr double radius = 40000000;
    ms::Input input;
    input.slab.outer = { { -radius, 0, radius, 0 }, { pi, pi } };
    input.slab.top = 9;
    double area = 123;
    std::string error;
    ASSERT_TRUE (ms::Facade ({ input }, area, error)) << error;
    EXPECT_NEAR (area, 9 * 1024 * radius * std::sin (pi / 512), 1e-4);
    input.slab.holes.assign (40, input.slab.outer); // 41*512 tessellated points > existing 20000 budget.
    area = 123;
    EXPECT_FALSE (ms::Facade ({ input }, area, error));
    EXPECT_EQ (area, 123);
    EXPECT_NE (error.find ("budget"), std::string::npos);
    input.slab.holes.clear ();
    input.slab.top = std::numeric_limits<double>::max ();
    EXPECT_FALSE (ms::Facade ({ input }, area, error));
    EXPECT_EQ (area, 123);
}
