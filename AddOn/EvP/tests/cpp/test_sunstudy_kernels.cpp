// The kernel facade production code calls (SunStudy/SunStudyKernels, P7b):
// every entry returns what the C++ kernel returns. In a build with the Rust core
// the facade runs the Rust kernels, so this pins the facade's own glue -- the
// restored series, the patch-atlas state round trip, the snapshot's GUIDs for
// the roles, the binding's outputs -- on top of the kernel parity in
// test_sunstudy_rust_parity.cpp.

#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyStepAtlas.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace {

using namespace evp::sunstudy;

template <typename T> bool SameBits (const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size () == b.size () && (a.empty () || std::memcmp (a.data (), b.data (), a.size () * sizeof (T)) == 0);
}

void AppendQuad (geomsrv::Mesh& mesh, const double o[3], const double u[3], const double v[3])
{
    const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
    for (int corner = 0; corner < 4; ++corner) {
        const double a = corner == 1 || corner == 2 ? 1.0 : 0.0, b = corner >= 2 ? 1.0 : 0.0;
        for (int axis = 0; axis < 3; ++axis)
            mesh.vertices.push_back (o[axis] + a * u[axis] + b * v[axis]);
    }
    mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
}

geomsrv::Mesh Box (const std::string& guid, double x, double y, double w, double d, double h)
{
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    const double top[3] = { x, y, h }, ex[3] = { w, 0, 0 }, ey[3] = { 0, d, 0 }, ez[3] = { 0, 0, h };
    const double south[3] = { x, y, 0 }, north[3] = { x + w, y + d, 0 }, wx[3] = { -w, 0, 0 }, dy[3] = { 0, -d, 0 };
    AppendQuad (mesh, top, ex, ey);
    AppendQuad (mesh, south, ex, ez);
    AppendQuad (mesh, north, wx, ez);
    AppendQuad (mesh, south, ez, ey);
    AppendQuad (mesh, north, ez, dy);
    return mesh;
}

const char* kSlab = "{0C000000-0000-0000-0000-000000000001}";
const char* kTower = "{0C000000-0000-0000-0000-000000000002}";
const char* kShed = "{0C000000-0000-0000-0000-000000000003}";

std::shared_ptr<geomsrv::Snapshot> Scene ()
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 77;
    snapshot->meshes = { Box (kSlab, -10, -10, 30, 25, 0.3), Box (kTower, 2, 0, 4, 4, 9), Box (kShed, 12, 6, 3, 2, 3) };
    return snapshot;
}

// The snapshot as one mesh, one group per element.
struct Flat {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles, groups;
    std::vector<std::string> elementOf;
};

Flat Flatten (const geomsrv::Snapshot& snapshot)
{
    Flat flat;
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        const auto& mesh = snapshot.meshes[m];
        const uint32_t base = static_cast<uint32_t> (flat.vertices.size () / 3);
        flat.vertices.insert (flat.vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
        for (const uint32_t index : mesh.triangles)
            flat.triangles.push_back (base + index);
        flat.groups.resize (flat.triangles.size () / 3, static_cast<uint32_t> (m));
        flat.elementOf.push_back (mesh.guid);
    }
    return flat;
}

void ExpectSameState (const PatchAtlasState& a, const PatchAtlasState& e)
{
    EXPECT_EQ (a.width, e.width);
    EXPECT_EQ (a.height, e.height);
    EXPECT_EQ (a.penX, e.penX);
    EXPECT_EQ (a.penY, e.penY);
    EXPECT_EQ (a.shelfHeight, e.shelfHeight);
    EXPECT_EQ (a.nextGeneration, e.nextGeneration);
    EXPECT_EQ (a.allocations.size (), e.allocations.size ());
}

void ExpectSameAtlas (const SunStudyAtlas& a, const SunStudyAtlas& e)
{
    EXPECT_EQ (a.width, e.width);
    EXPECT_EQ (a.height, e.height);
    EXPECT_EQ (a.placedFaces, e.placedFaces);
    EXPECT_EQ (a.tiles.size (), e.tiles.size ());
    EXPECT_EQ (a.texels, e.texels);
}

} // namespace

TEST (SunStudyKernels, SeriesIsRestoredWithItsSourceCountAndHash)
{
    std::vector<SunStep> steps;
    for (int i = 0; i < 24; ++i) {
        SunStep step;
        step.time.hour = i;
        step.altitudeDegrees = 60.0 * std::sin ((i - 6) * 3.14159265358979323846 / 12.0);
        step.direction[0] = std::cos (i * 0.26);
        step.direction[1] = std::sin (i * 0.26);
        step.direction[2] = std::sin (step.altitudeDegrees * 3.14159265358979323846 / 180.0);
        steps.push_back (step);
    }
    for (const double floor : { 0.0, 5.0 }) {
        const SunSeries e = SunSeries::FromSteps (steps, 60, floor);
        std::string error;
        const SunSeries a = kernels::SeriesFromSteps (steps, 60, floor, &error);
        EXPECT_TRUE (error.empty ()) << error;
        ASSERT_EQ (a.StepCount (), e.StepCount ());
        EXPECT_GT (a.StepCount (), 0u);
        EXPECT_EQ (a.SourceStepCount (), e.SourceStepCount ());
        EXPECT_EQ (a.Version (), e.Version ());
        EXPECT_EQ (a.TimestepMinutes (), e.TimestepMinutes ());
        EXPECT_EQ (a.DaylightHours (), e.DaylightHours ());
        for (size_t i = 0; i < e.StepCount (); ++i)
            EXPECT_EQ (std::memcmp (a.Step (i).direction, e.Step (i).direction, sizeof (double) * 3), 0) << i;
    }
}

TEST (SunStudyKernels, LimitsAdviceAndGroundGridMatchTheKernels)
{
    MachineResources machine;
    machine.gpuKnown = true;
    machine.gpuMemoryBytes = 6ull << 30;
    machine.maxTextureDimension = 16384;
    machine.availableRamBytes = 9ull << 30;
    for (const double occupancy : { kPatchAtlasOccupancy, kTriangleAtlasOccupancy }) {
        const AnalysisLimits e = ComputeAnalysisLimits (machine, 49, occupancy);
        const AnalysisLimits a = kernels::ComputeAnalysisLimits (machine, 49, occupancy);
        EXPECT_EQ (a.maxSamples, e.maxSamples);
        EXPECT_EQ (a.maxAtlasDimension, e.maxAtlasDimension);
        EXPECT_EQ (a.binding, e.binding);
        EXPECT_EQ (a.maxRays, e.maxRays);
        EXPECT_EQ (a.textureCap, e.textureCap);
    }
    for (const size_t triangles : { size_t (0), size_t (5000), size_t (900000), size_t (9000000) })
        for (const std::string preset : { "", "early", "late" })
            EXPECT_EQ (kernels::SunStudyPresetAdvice (triangles, preset), SunStudyPresetAdvice (triangles, preset));
    const GroundGrid e = MakeGroundSampleGrid ({ -10, -10, 0 }, { 20, 15, 9 }, 1.5, -1.0, 0.1);
    const GroundGrid a = kernels::MakeGroundSampleGrid ({ -10, -10, 0 }, { 20, 15, 9 }, 1.5, -1.0, 0.1);
    EXPECT_TRUE (a.valid);
    EXPECT_EQ (a.columns, e.columns);
    EXPECT_EQ (a.rows, e.rows);
    EXPECT_TRUE (SameBits (a.positions, e.positions));
}

TEST (SunStudyKernels, RolesReceiversAndBindingMatchTheKernels)
{
    const auto snapshot = Scene ();
    const std::vector<std::string> analysis { kTower }, context { kShed }, ignored {};
    const ElementRoles e = ResolveElementRoles (*snapshot, analysis, context, ignored);
    const ElementRoles a = kernels::ResolveElementRoles (*snapshot, analysis, context, ignored);
    EXPECT_EQ (a.roles, e.roles);
    EXPECT_EQ (a.analysis, e.analysis);
    EXPECT_EQ (a.context, e.context);
    EXPECT_EQ (a.SampleMask (), e.SampleMask ());
    const auto absent = kernels::ResolveElementRoles (*snapshot, { "{0C000000-0000-0000-0000-0000000000FF}" }, {}, {});
    EXPECT_EQ (
        absent.analysisNamedButAbsent,
        ResolveElementRoles (*snapshot, { "{0C000000-0000-0000-0000-0000000000FF}" }, {}, {}).analysisNamedButAbsent);

    const SunStudyReceivers re = BuildSunStudyReceivers (*snapshot, e, false, {}, 0.5);
    const SunStudyReceivers ra = kernels::BuildSunStudyReceivers (*snapshot, e, false, {}, 0.5);
    EXPECT_EQ (ra.roles.roles, re.roles.roles);
    EXPECT_EQ (ra.faces, re.faces);
    EXPECT_EQ (ra.analysisFaces, re.analysisFaces);

    SunStudySelectionBinding expected, actual;
    for (uint64_t generation : { 0ull, 3ull, 3ull, 4ull }) {
        std::vector<std::string> ec, ei, ea, ac, ai, aa;
        const auto er = expected.Refresh (generation, context, ignored, ec, ei, analysis, &ea);
        const auto ar = kernels::RefreshSelectionBinding (actual, generation, context, ignored, ac, ai, analysis, &aa);
        EXPECT_EQ (ar, er) << generation;
        EXPECT_EQ (ac, ec);
        EXPECT_EQ (ai, ei);
        EXPECT_EQ (aa, ea);
        EXPECT_EQ (actual.generation, expected.generation);
        EXPECT_EQ (actual.revision, expected.revision);
    }
}

TEST (SunStudyKernels, GridsAndAtlasesMatchTheKernelsAndKeepTheirAllocations)
{
    const auto snapshot = Scene ();
    const Flat flat = Flatten (*snapshot);
    const size_t vertexCount = flat.vertices.size () / 3, faceCount = flat.triangles.size () / 3;

    SamplerOptions legacy;
    legacy.spacing = 0.8;
    legacy.wantLayouts = true;
    const SampleGrid es = BuildSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (), faceCount,
                                           flat.groups.data (), legacy);
    const SampleGrid as = kernels::BuildSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (),
                                                    faceCount, flat.groups.data (), legacy);
    ASSERT_GT (es.Count (), 0u);
    EXPECT_TRUE (SameBits (as.positions, es.positions));
    EXPECT_TRUE (SameBits (as.normals, es.normals));
    ExpectSameAtlas (kernels::BuildSunStudyAtlas (as), BuildSunStudyAtlas (es));

    PatchSamplerOptions patchOptions;
    patchOptions.spacing = 0.8;
    const PatchSampleGrid ep = BuildPatchSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (),
                                                     faceCount, flat.groups.data (), flat.elementOf, patchOptions);
    const PatchSampleGrid ap =
        kernels::BuildPatchSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (), faceCount,
                                       flat.groups.data (), flat.elementOf, patchOptions);
    ASSERT_GT (ep.Count (), 0u);
    EXPECT_TRUE (SameBits (ap.positions, ep.positions));
    EXPECT_EQ (ap.spans.size (), ep.spans.size ());

    // Fit twice: the second fit must start from the first one's allocations.
    SunStudyPatchAtlas expected, actual;
    for (int round = 0; round < 2; ++round) {
        const PatchAtlasUpdate e = expected.Fit (ep, 1, 4096);
        const PatchAtlasUpdate a = kernels::FitPatchAtlas (actual, ap, 1, 4096);
        EXPECT_EQ (a.retained, e.retained) << round;
        EXPECT_EQ (a.added, e.added) << round;
        EXPECT_EQ (a.resized, e.resized) << round;
        ExpectSameState (actual.State (), expected.State ());
    }

    SunStudyPatchAtlas eAllocations, aAllocations;
    ExpectSameAtlas (kernels::BuildStableTriangleAtlas (as, *snapshot, aAllocations),
                     BuildStableTriangleAtlas (es, *snapshot, eAllocations));
    ExpectSameState (aAllocations.State (), eAllocations.State ());
    SunStudyPatchAtlas eNext, aNext;
    ExpectSameAtlas (kernels::BuildStableTriangleAtlas (as, *snapshot, aNext, &aAllocations),
                     BuildStableTriangleAtlas (es, *snapshot, eNext, &eAllocations));
    ExpectSameState (aNext.State (), eNext.State ());
}

TEST (SunStudyKernels, SurfaceSamplingMatchesTheKernelInBothDomains)
{
    const auto snapshot = Scene ();
    const std::vector<uint8_t> mask { 1, 1, 0 };
    for (const auto domain : { SamplingDomain::TriangleLegacy, SamplingDomain::SurfacePatch }) {
        SurfaceSamplingOptions options;
        options.domain = domain;
        options.spacing = 0.9;
        const SurfaceSamplingResult e = BuildSurfaceSampling (*snapshot, mask, options);
        std::string error;
        const SurfaceSamplingResult a =
            kernels::BuildSurfaceSampling (*snapshot, mask, options, nullptr, {}, {}, &error);
        EXPECT_TRUE (error.empty ()) << error;
        ASSERT_TRUE (e.valid);
        EXPECT_EQ (a.valid, e.valid);
        EXPECT_TRUE (SameBits (a.triangles.positions, e.triangles.positions));
        EXPECT_TRUE (SameBits (a.patches.positions, e.patches.positions));
        EXPECT_EQ (a.winding.flipped, e.winding.flipped);
    }
}

TEST (SunStudyKernels, ChangedRegionsMatchTheKernelAndHonourAnEarlyCancel)
{
    const uint32_t width = 64, height = 32;
    std::vector<float> before (size_t (width) * height, 0.0f), after = before;
    for (uint32_t y = 4; y < 9; ++y)
        for (uint32_t x = 10; x < 30; ++x)
            after[size_t (y) * width + x] = 1.0f;
    after[size_t (20) * width + 50] = 2.0f;
    const auto e = AtlasChangedRegions (width, height, 1, before, after);
    const auto a = kernels::AtlasChangedRegions (width, height, 1, before, after);
    ASSERT_EQ (a.size (), e.size ());
    for (size_t i = 0; i < e.size (); ++i) {
        EXPECT_EQ (a[i].x, e[i].x);
        EXPECT_EQ (a[i].y, e[i].y);
        EXPECT_EQ (a[i].width, e[i].width);
        EXPECT_EQ (a[i].height, e[i].height);
    }
    std::vector<uint32_t> words (size_t (width) * height * 2, 0u), changed = words;
    changed[5] = 7u;
    EXPECT_EQ (kernels::AtlasChangedRegions (width, height, 2, words, changed).size (),
               AtlasChangedRegions (width, height, 2, words, changed).size ());
    EXPECT_TRUE (kernels::AtlasChangedRegions (width, height, 1, before, after, 512, [] { return true; }).empty ());
}

TEST (SunStudyKernels, TimestepsFanScatterAndFinestGridMatchTheKernels)
{
    for (const int timestep : { 1, 7, 15, 60, 90 })
        for (const auto& [from, to] : { std::pair { 0, 24 }, std::pair { 6, 18 }, std::pair { 13, 13 } }) {
            const auto e = EnumerateTimesteps (timestep, from, to);
            const auto a = kernels::EnumerateTimesteps (timestep, from, to);
            ASSERT_EQ (a.size (), e.size ()) << timestep << " " << from << "-" << to;
            for (size_t i = 0; i < e.size (); ++i) {
                EXPECT_EQ (a[i].hour, e[i].hour);
                EXPECT_EQ (a[i].minute, e[i].minute);
            }
        }
    std::vector<uint16_t> minutes;
    for (uint16_t m = 6 * 60 + 7; m < 19 * 60; m += 15)
        minutes.push_back (m);
    for (const double interval : { 0.0, 15.0, 30.0, 60.0, 120.0 })
        EXPECT_EQ (kernels::FanSteps (minutes, interval), FanSteps (minutes, interval)) << interval;

    SunStudyAtlas atlas;
    atlas.width = 8;
    atlas.height = 4;
    atlas.valid = true;
    atlas.texels = { 0, 9, 31, 40, 5 };
    const std::vector<double> values { 1.0, 2.25, 3.5, 4.0 };
    EXPECT_TRUE (SameBits (kernels::ScatterToAtlas (atlas, values, -1.0f), ScatterToAtlas (atlas, values, -1.0f)));
    atlas.valid = false;
    EXPECT_TRUE (kernels::ScatterToAtlas (atlas, values).empty ());

    MachineResources machine;
    machine.gpuKnown = true;
    machine.gpuMemoryBytes = 4ull << 30;
    machine.maxTextureDimension = 16384;
    machine.availableRamBytes = 8ull << 30;
    for (const double area : { 0.0, 850.0, 125000.0 }) {
        const auto [limits, finest] = kernels::ReportAnalysisLimits (machine, 49, kPatchAtlasOccupancy, area);
        EXPECT_EQ (limits.maxSamples, ComputeAnalysisLimits (machine, 49, kPatchAtlasOccupancy).maxSamples);
        EXPECT_EQ (finest, FinestGridSpacing (area, limits.maxSamples)) << area;
    }
}

// The analysis cylinder reaches the Rust sampler (#152): clipped patch grids
// and surface samplings equal the C++ ones, and the circle survives the
// layout round trip that a rerun's reuse check reads.
TEST (SunStudyKernels, TheAnalysisCylinderClipsTheSameCellsInBothSamplers)
{
    const auto snapshot = Scene ();
    const Flat flat = Flatten (*snapshot);
    PatchSamplerOptions patchOptions;
    patchOptions.spacing = 0.8;
    patchOptions.clipX = 4.0;
    patchOptions.clipY = 2.0;
    patchOptions.clipRadius = 6.0;
    const size_t vertexCount = flat.vertices.size () / 3, faceCount = flat.triangles.size () / 3;
    const PatchSampleGrid e = BuildPatchSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (),
                                                    faceCount, flat.groups.data (), flat.elementOf, patchOptions);
    const PatchSampleGrid a =
        kernels::BuildPatchSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (), faceCount,
                                       flat.groups.data (), flat.elementOf, patchOptions);
    ASSERT_TRUE (e.valid);
    EXPECT_TRUE (SameBits (a.positions, e.positions));
    EXPECT_EQ (a.spans.size (), e.spans.size ());
    EXPECT_EQ (a.patchOfTriangle, e.patchOfTriangle);
    patchOptions.clipRadius = 0.0;
    EXPECT_GT (kernels::BuildPatchSampleGrid (flat.vertices.data (), vertexCount, flat.triangles.data (), faceCount,
                                              flat.groups.data (), flat.elementOf, patchOptions)
                   .Count (),
               a.Count ())
        << "without the circle every cell is sampled";

    SurfaceSamplingOptions options;
    options.domain = SamplingDomain::SurfacePatch;
    options.spacing = 0.8;
    options.clipX = 4.0;
    options.clipY = 2.0;
    options.clipRadius = 6.0;
    const std::vector<uint8_t> mask { 1, 1, 1 };
    const SurfaceSamplingResult es = BuildSurfaceSampling (*snapshot, mask, options);
    const SurfaceSamplingResult as = kernels::BuildSurfaceSampling (*snapshot, mask, options);
    ASSERT_TRUE (as.valid);
    EXPECT_TRUE (SameBits (as.patches.positions, es.patches.positions));
    EXPECT_EQ (as.layout.options.clipX, 4.0);
    EXPECT_EQ (as.layout.options.clipY, 2.0);
    EXPECT_EQ (as.layout.options.clipRadius, 6.0);
}
