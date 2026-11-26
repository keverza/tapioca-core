// Sky patches, sky masks and the climate run across the Rust ABI (#269,
// #155 P0/F3). The overhang case is #155 P0's acceptance: a vertical plane
// under a long overhang against the analytic view factor of an infinitely
// long one, sin(atan(H/D)) / 2, with D shortened by the origin's lift.

#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyRustClimate.hpp"
#include "SunStudy/SunStudySkyMask.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

using namespace evp::sunstudy;
using evp::sunstudy::rust::ClimateInterval;
using evp::sunstudy::rust::ClimateSetup;
using evp::sunstudy::rust::ClimateTotals;
using evp::sunstudy::rust::RustClimate;
using evp::sunstudy::rust::SkyCell;
using evp::sunstudy::rust::SkyCells;
using evp::sunstudy::rust::SkyPatch;
using evp::sunstudy::rust::SkyPatches;
using evp::sunstudy::rust::SkyPatchSet;
using geomsrv::Mesh;
using geomsrv::QueryEngine;
using geomsrv::Snapshot;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A horizontal quad x in [-half, half], y in [y0, y1] at height z.
void AddQuad (Mesh& mesh, double half, double y0, double y1, double z)
{
    const auto base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    mesh.vertices.insert (mesh.vertices.end (), { -half, y0, z, half, y0, z, half, y1, z, -half, y1, z });
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

// An overhang `depth` deep in front of a wall facing -y, `height` above the
// sample, 400 m long. With `depth` 0 the scene holds only a far-away speck.
std::unique_ptr<CpuTraversal> Scene (double height, double depth)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 11;
    Mesh mesh;
    mesh.guid = "overhang";
    if (depth > 0.0)
        AddQuad (mesh, 200.0, -depth, 0.0, height);
    else
        AddQuad (mesh, 0.01, 5000.0, 5000.01, -50.0); // below every patch, out of the way
    snap->meshes.push_back (std::move (mesh));
    return std::make_unique<CpuTraversal> (std::make_shared<QueryEngine> (snap));
}

ClimateTotals Read (const std::vector<double>& normals, const std::vector<uint8_t>& open, SkyPatchSet set)
{
    ClimateSetup setup;
    setup.set = set;
    setup.normals = normals;
    setup.open = open;
    std::string error;
    auto run = RustClimate::Create (setup, error);
    EXPECT_NE (run, nullptr) << error;
    ClimateTotals totals;
    if (run)
        EXPECT_TRUE (run->Read (totals, error)) << error;
    return totals;
}

size_t PatchCount (SkyPatchSet set)
{
    return set == SkyPatchSet::Tregenza ? 145u : 577u;
}

std::vector<uint8_t> Masks (const CpuTraversal& scene, const std::vector<double>& positions,
                            const std::vector<double>& normals, SkyPatchSet set, uint32_t perSide = 4)
{
    std::vector<SkyCell> cells;
    std::string error;
    EXPECT_TRUE (SkyCells (set, 0.0, perSide, cells, error)) << error;
    std::vector<uint8_t> open;
    EXPECT_TRUE (TraceSkyMasks (scene, positions, normals, cells, PatchCount (set), {}, {}, open, error)) << error;
    EXPECT_EQ (open.size (), positions.size () / 3 * PatchCount (set));
    return open;
}

} // namespace

TEST (SunStudyClimate, SkyPatchesCloseTheHemisphereInBothSets)
{
    for (const auto [set, count] : { std::pair { SkyPatchSet::Tregenza, 145u }, { SkyPatchSet::Reinhart, 577u } }) {
        std::vector<SkyPatch> patches;
        std::string error;
        ASSERT_TRUE (SkyPatches (set, 0.3, patches, error)) << error;
        ASSERT_EQ (patches.size (), count);
        double omega = 0.0, projected = 0.0;
        for (const SkyPatch& p : patches) {
            omega += p.solidAngle;
            projected += p.moment[2];
        }
        EXPECT_NEAR (omega, 2.0 * kPi, 1e-12);
        EXPECT_NEAR (projected, kPi, 1e-12);
    }
}

TEST (SunStudyClimate, AnOpenSceneGivesTheTextbookSkyViewFactors)
{
    const auto scene = Scene (0.0, 0.0);
    const std::vector<double> positions { 0, 0, 0, 3, 0, 1 };
    const std::vector<double> normals { 0, 0, 1, 0, -1, 0 };
    for (const SkyPatchSet set : { SkyPatchSet::Tregenza, SkyPatchSet::Reinhart }) {
        const ClimateTotals totals = Read (normals, Masks (*scene, positions, normals, set), set);
        ASSERT_EQ (totals.skyView.size (), 2u);
        EXPECT_NEAR (totals.skyView[0], 1.0, 1e-12);
        // Patches the plane cuts are seen in part; a share is quantised to
        // 1/255 of its patch.
        EXPECT_NEAR (totals.skyView[1], 0.5, 2e-3);
    }
}

TEST (SunStudyClimate, AVerticalPlaneUnderAnOverhangMatchesTheAnalyticViewFactor)
{
    // H = 1 m above, D = 2 m deep; the origin sits 2 cm out from the wall,
    // so the overhang reaches 1.98 m past it: sin(atan(1/1.98)) / 2 = 0.2254.
    const double height = 1.0, depth = 2.0;
    const double exact = 0.5 * height / std::hypot (height, depth - SkyMaskOptions {}.normalOffset);
    const auto scene = Scene (height, depth);
    const std::vector<double> positions { 0, 0, 0 };
    const std::vector<double> normals { 0, -1, 0 };
    for (const SkyPatchSet set : { SkyPatchSet::Tregenza, SkyPatchSet::Reinhart }) {
        // #155 P0: within 2 % of the analytic value, with 4 x 4 cells a patch.
        const ClimateTotals cells = Read (normals, Masks (*scene, positions, normals, set), set);
        ASSERT_EQ (cells.skyView.size (), 1u);
        EXPECT_NEAR (cells.skyView[0], exact, 0.02 * exact) << static_cast<int> (set);
        // One ray a patch misreads the patches the edge cuts: on Reinhart 577
        // it is about 4 % low, which is why the pass splits patches.
        const ClimateTotals single = Read (normals, Masks (*scene, positions, normals, set, 1), set);
        if (set == SkyPatchSet::Reinhart)
            EXPECT_GT (std::abs (single.skyView[0] - exact), 0.03 * exact);
    }
}

TEST (SunStudyClimate, MasksRepeatBitForBitAcrossThreadCounts)
{
    const auto scene = Scene (1.0, 2.0);
    std::vector<double> positions, normals;
    for (int i = 0; i < 40; ++i) {
        positions.insert (positions.end (), { 0.1 * i, -0.05 * i, 0.03 * i });
        normals.insert (normals.end (), { 0.0, -1.0, 0.0 });
    }
    std::vector<SkyCell> cells;
    std::string error;
    ASSERT_TRUE (SkyCells (SkyPatchSet::Tregenza, 0.0, 2, cells, error)) << error;
    std::vector<uint8_t> one, many;
    SkyMaskOptions serial;
    serial.maxParallel = 1;
    serial.raysPerBatch = 1500; // two samples a batch
    ASSERT_TRUE (TraceSkyMasks (*scene, positions, normals, cells, 145, serial, {}, one, error)) << error;
    ASSERT_TRUE (TraceSkyMasks (*scene, positions, normals, cells, 145, {}, {}, many, error)) << error;
    EXPECT_EQ (one, many);
}

TEST (SunStudyClimate, ACancelledMaskPassReturnsNothing)
{
    const auto scene = Scene (1.0, 2.0);
    std::vector<SkyCell> cells;
    std::string error;
    ASSERT_TRUE (SkyCells (SkyPatchSet::Tregenza, 0.0, 1, cells, error)) << error;
    std::vector<uint8_t> open { 9 };
    EXPECT_FALSE (TraceSkyMasks (*scene, { 0, 0, 0 }, { 0, -1, 0 }, cells, 145, {}, [] { return true; }, open, error));
    EXPECT_EQ (error, "Cancelled");
    EXPECT_TRUE (open.empty ());
    EXPECT_FALSE (TraceSkyMasks (*scene, { 0, 0 }, { 0, -1, 0 }, cells, 145, {}, {}, open, error));
    EXPECT_FALSE (TraceSkyMasks (*scene, { 0, 0, 0 }, { 0, -1, 0 }, cells, 100, {}, {}, open, error))
        << "a cell beyond the dome";
}

TEST (SunStudyClimate, TheBeamIsBlockedUnderTheOverhangOnlyWhenTheSunIsHigh)
{
    const auto scene = Scene (1.0, 2.0);
    const std::vector<double> positions { 0, 0, 0 };
    const std::vector<double> normals { 0, -1, 0 };
    std::vector<uint8_t> beam;
    std::string error;
    const auto sunAt = [] (double altitudeDegrees) {
        const double a = altitudeDegrees * kPi / 180.0;
        return std::array<double, 3> { 0.0, -std::cos (a), std::sin (a) };
    };
    ASSERT_TRUE (TraceBeam (*scene, positions, normals, sunAt (60.0), {}, beam, error)) << error;
    EXPECT_EQ (beam, std::vector<uint8_t> { 0 });
    ASSERT_TRUE (TraceBeam (*scene, positions, normals, sunAt (15.0), {}, beam, error)) << error;
    EXPECT_EQ (beam, std::vector<uint8_t> { 2 });
    // Behind the wall: blocked whatever is in the way.
    ASSERT_TRUE (TraceBeam (*scene, positions, normals, { 0.0, 0.6, 0.8 }, {}, beam, error)) << error;
    EXPECT_EQ (beam, std::vector<uint8_t> { 0 });
}

TEST (SunStudyClimate, AnOpenHorizontalSampleReceivesTheHorizontalTotal)
{
    const auto scene = Scene (0.0, 0.0);
    const std::vector<double> positions { 0, 0, 0 };
    const std::vector<double> normals { 0, 0, 1 };
    ClimateSetup setup;
    setup.normals = normals;
    setup.open = Masks (*scene, positions, normals, SkyPatchSet::Tregenza);
    std::string error;
    auto run = RustClimate::Create (setup, error);
    ASSERT_NE (run, nullptr) << error;
    ClimateInterval interval;
    const double a = 40.0 * kPi / 180.0;
    interval.sun = { 0.0, -std::cos (a), std::sin (a) };
    interval.dni = 600.0;
    interval.dhi = 120.0;
    interval.dewPointC = std::nan ("");
    interval.dayOfYear = 160;
    interval.month = 5;
    std::vector<uint8_t> beam;
    ASSERT_TRUE (TraceBeam (*scene, positions, normals, interval.sun, {}, beam, error)) << error;
    ASSERT_TRUE (run->Add (interval, beam, error)) << error;
    EXPECT_FALSE (run->Add (interval, {}, error)) << "a sun-up interval needs its beam";
    ClimateTotals totals;
    ASSERT_TRUE (run->Read (totals, error)) << error;
    const double open = 600.0 * std::sin (a) + 120.0;
    EXPECT_NEAR (totals.report.openHorizontal[5], open, 1e-9);
    EXPECT_NEAR (totals.Total (0, 5), open, 1e-9 * open);
    EXPECT_EQ (totals.report.validHours[5], 1.0);
}
