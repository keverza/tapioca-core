// Differential tests: the Rust sun-study core against the C++ implementation it
// replaces. The C++ side is the oracle; every case runs both on the same input
// and compares BIT FOR BIT, not within a tolerance -- the migration's exit
// criterion is every-bit equality, and a tolerance would hide a reordered sum.
//
// Fixtures are generated from a fixed-seed LCG so the corpus is reproducible
// and large enough to reach the branches (pole flip, zero sun, refusals) that a
// handful of literals would miss.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyAtlasReuse.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyRust.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <thread>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <limits>

namespace {

using namespace evp::sunstudy;
namespace rs = evp::sunstudy::rust;

// Numerical Recipes LCG; deterministic across compilers, unlike <random>'s
// distributions.
class Lcg {
  public:
    explicit Lcg (uint64_t seed) : state_ (seed)
    {
    }
    double Uniform (double lo, double hi)
    {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        const double unit = static_cast<double> (state_ >> 11) * (1.0 / 9007199254740992.0);
        return lo + (hi - lo) * unit;
    }
    int Int (int lo, int hi)
    {
        return lo + static_cast<int> (Uniform (0.0, 1.0) * (hi - lo + 1));
    }

  private:
    uint64_t state_;
};

::testing::AssertionResult SameBits (double expected, double actual)
{
    if (std::bit_cast<uint64_t> (expected) == std::bit_cast<uint64_t> (actual))
        return ::testing::AssertionSuccess ();
    return ::testing::AssertionFailure () << "C++ " << expected << " vs Rust " << actual << " (bits differ)";
}

template <size_t N>
void ExpectSameBits (const std::array<double, N>& expected, const std::array<double, N>& actual, const char* what)
{
    for (size_t i = 0; i < N; ++i)
        EXPECT_TRUE (SameBits (expected[i], actual[i])) << what << "[" << i << "]";
}

void ExpectSameBits (const std::vector<double>& expected, const std::vector<double>& actual, const char* what)
{
    ASSERT_EQ (expected.size (), actual.size ()) << what;
    for (size_t i = 0; i < expected.size (); ++i)
        ASSERT_TRUE (SameBits (expected[i], actual[i])) << what << "[" << i << "]";
}

std::vector<Vec3> SunCorpus ()
{
    std::vector<Vec3> suns = {
        { 0.0, 0.0, 1.0 },   { 0.0, 0.0, -1.0 },   { std::sqrt (0.5), 0.0, std::sqrt (0.5) },
        { 0.0, 0.0, 0.0 },   { 1.0, 0.0, 0.0 },    { 0.0, 1.0, 0.0 },
        { 3.0, -4.0, 12.0 }, { 1e-3, 0.0, 1.0 },   { 0.0447, 0.0, 0.999 }, // just past the pole-flip threshold
        { -0.0, -0.0, 1.0 }, { 1e-300, 0.0, 0.0 },
    };
    Lcg lcg (0x5eed5e1e5ull);
    for (int i = 0; i < 200; ++i)
        suns.push_back ({ lcg.Uniform (-1, 1), lcg.Uniform (-1, 1), lcg.Uniform (-0.2, 1) });
    return suns;
}

struct Box {
    Vec3 lo;
    Vec3 hi;
};

std::vector<Box> BoxCorpus ()
{
    std::vector<Box> boxes = {
        { { 100, 200, 0 }, { 120, 210, 30 } },
        { { -10, -10, 0 }, { 10, 10, 20 } },
        { { 0, 0, 0 }, { 10, 10, 10 } },
        { { 5, 5, 0 }, { 5, 5, 0 } },                                        // degenerate
        { { 10, 0, 0 }, { 0, 10, 10 } },                                     // inverted
        { { 624000.0, 6172000.0, 120.0 }, { 624210.5, 6172133.25, 168.0 } }, // survey grid
    };
    Lcg lcg (0xb0c5ull);
    for (int i = 0; i < 40; ++i) {
        const Vec3 lo { lcg.Uniform (-1e5, 1e5), lcg.Uniform (-1e5, 1e5), lcg.Uniform (-50, 50) };
        boxes.push_back (
            { lo, { lo[0] + lcg.Uniform (0, 300), lo[1] + lcg.Uniform (0, 300), lo[2] + lcg.Uniform (0, 120) } });
    }
    return boxes;
}

} // namespace

TEST (SunStudyRustParity, AbiVersionHandshake)
{
    uint32_t version = 0;
    std::string error = "stale";
    ASSERT_TRUE (rs::ReadAbiVersion (version, error)) << error;
    EXPECT_EQ (version, 2u); // v2: viewpoint modes and the sphere visibility view (#80)
    EXPECT_TRUE (error.empty ());
}

TEST (SunStudyRustParity, SunSeriesMatchesEveryBitIncludingTheVersion)
{
    Lcg lcg (0x5e41e5ull);
    size_t cases = 0;
    for (const int timestep : { 60, 30, 15, 1, 0, -5, 90 }) {
        for (const double floor : { 0.0, 3.0, -90.0, 45.0 }) {
            std::vector<SunStep> steps;
            const int count = lcg.Int (0, 60);
            for (int i = 0; i < count; ++i) {
                SunStep step;
                step.time = TimeOfDay { lcg.Int (0, 23), lcg.Int (0, 59) };
                step.altitudeDegrees = lcg.Uniform (-15, 85);
                step.direction[0] = lcg.Uniform (-2, 2);
                step.direction[1] = lcg.Uniform (-2, 2);
                step.direction[2] = lcg.Uniform (-2, 2);
                if (i % 11 == 3)
                    step.direction[0] = step.direction[1] = step.direction[2] = 0.0; // unresolved sun
                if (i % 13 == 5)
                    step.direction[1] = -0.0;
                steps.push_back (step);
            }
            const SunSeries expected = SunSeries::FromSteps (steps, timestep, floor);
            rs::SunSeriesResult actual;
            std::string error;
            ASSERT_TRUE (rs::BuildSunSeries (steps, timestep, floor, actual, error)) << error;
            ASSERT_EQ (actual.steps.size (), expected.StepCount ());
            EXPECT_EQ (actual.sourceStepCount, expected.SourceStepCount ());
            EXPECT_EQ (actual.version, expected.Version ());
            EXPECT_EQ (actual.timestepMinutes, expected.TimestepMinutes ());
            EXPECT_TRUE (SameBits (expected.HoursPerStep (), actual.hoursPerStep));
            EXPECT_TRUE (SameBits (expected.DaylightHours (), actual.daylightHours));
            for (size_t i = 0; i < expected.StepCount (); ++i) {
                const SunStep& e = expected.Step (i);
                const SunStep& a = actual.steps[i];
                EXPECT_EQ (a.time.hour, e.time.hour);
                EXPECT_EQ (a.time.minute, e.time.minute);
                EXPECT_TRUE (SameBits (e.altitudeDegrees, a.altitudeDegrees));
                for (int c = 0; c < 3; ++c)
                    EXPECT_TRUE (SameBits (e.direction[c], a.direction[c])) << "step " << i << " axis " << c;
            }
            ++cases;
        }
    }
    EXPECT_EQ (cases, 28u);
}

TEST (SunStudyRustParity, ShadowCameraMatchesEveryBit)
{
    size_t valid = 0;
    size_t refused = 0;
    for (const Box& box : BoxCorpus ()) {
        for (const Vec3& sun : SunCorpus ()) {
            for (const double margin : { 1.0, 0.0, 2.5 }) {
                const uint32_t resolution = (valid % 3 == 0) ? 2048u : (valid % 3 == 1 ? 4096u : 0u);
                const OrthoFrustum f = FitOrthoFrustum (box.lo, box.hi, sun, margin);
                rs::ShadowCameraResult r;
                std::string error;
                ASSERT_TRUE (rs::FitShadowCamera (box.lo, box.hi, sun, margin, resolution, kMinBiasMetres, r, error))
                    << error;
                ASSERT_EQ (r.frustum.valid, f.valid);
                if (!f.valid) {
                    ++refused;
                    continue;
                }
                ++valid;
                const double planes[] = { f.left,     f.right,      f.bottom,      f.top,       f.nearPlane,
                                          f.farPlane, f.worldWidth, f.worldHeight, f.worldDepth };
                const double rust[] = { r.frustum.left,       r.frustum.right,       r.frustum.bottom,
                                        r.frustum.top,        r.frustum.nearPlane,   r.frustum.farPlane,
                                        r.frustum.worldWidth, r.frustum.worldHeight, r.frustum.worldDepth };
                for (size_t i = 0; i < 9; ++i)
                    ASSERT_TRUE (SameBits (planes[i], rust[i])) << "plane " << i;
                ExpectSameBits (f.rightAxis, r.frustum.rightAxis, "right");
                ExpectSameBits (f.upAxis, r.frustum.upAxis, "up");
                ExpectSameBits (f.lightAxis, r.frustum.lightAxis, "light");
                ExpectSameBits (OrthoProjection (f), r.projection, "projection");
                ExpectSameBits (LightViewProjection (f), r.lightViewProjection, "lvp");
                const Vec3 origin = RtcOrigin (box.lo, box.hi);
                ExpectSameBits (origin, r.rtcOrigin, "rtc");
                ExpectSameBits (LightViewProjection (f, &origin), r.rtcLightViewProjection, "rtc lvp");
                EXPECT_TRUE (SameBits (TexelWorldSize (f, resolution), r.texelWorldSize));
                const double bias = DeriveBiasFromFrustum (f, resolution, kMinBiasMetres);
                EXPECT_TRUE (SameBits (bias, r.bias));
                EXPECT_TRUE (SameBits (DepthBiasNdc (bias, f.nearPlane, f.farPlane), r.depthBiasNdc));
            }
        }
    }
    EXPECT_GT (valid, 10000u);
    EXPECT_GT (refused, 0u) << "the corpus must reach the zero-sun and inverted-box refusals";
}

TEST (SunStudyRustParity, AccumLayoutMatches)
{
    for (const uint32_t maxWidth : { kMaxTextureSize, 8192u, 1u, 0u, 3u }) {
        for (const size_t n :
             { size_t { 0 }, size_t { 1 }, size_t { 2 }, size_t { 99 }, size_t { 100 }, size_t { 84052 },
               size_t { 268435456 }, size_t { 268435457 }, size_t { 9 }, size_t { 10 } }) {
            const AccumLayout e = evp::sunstudy::ComputeAccumLayout (n, maxWidth);
            AccumLayout a;
            std::string error;
            ASSERT_TRUE (rs::ComputeAccumLayout (n, maxWidth, a, error)) << error;
            EXPECT_EQ (a.valid, e.valid) << n << " @ " << maxWidth;
            if (e.valid) {
                EXPECT_EQ (a.width, e.width) << n << " @ " << maxWidth;
                EXPECT_EQ (a.height, e.height) << n << " @ " << maxWidth;
            }
        }
    }
}

TEST (SunStudyRustParity, GridLadderMatches)
{
    for (const double area : { 0.0, 100.0, 5000.0, 10000.0, 20000.0, 100000.0, 1e9 }) {
        for (const double target : { 1.0, 0.5, 0.0, -1.0, 3.0 }) {
            const std::vector<double> e = evp::sunstudy::GridLadder (area, target);
            std::vector<double> a;
            std::string error;
            ASSERT_TRUE (rs::GridLadder (area, target, 20000.0, 5000.0, a, error)) << error;
            ExpectSameBits (e, a, "ladder");
        }
    }
}

TEST (SunStudyRustParity, GroundGridMatchesEveryBit)
{
    size_t built = 0;
    for (const Box& box : BoxCorpus ()) {
        for (const double spacing : { 1.0, 3.0, 0.75, 0.0, -1.0, 25.0 }) {
            for (const double pad : { -1.0, 0.0, 5.0 }) {
                const GroundGrid e = evp::sunstudy::MakeGroundSampleGrid (box.lo, box.hi, spacing, pad, 0.10, 200000);
                GroundGrid a;
                std::string error;
                ASSERT_TRUE (rs::MakeGroundSampleGrid (box.lo, box.hi, spacing, pad, 0.10, 200000, a, error)) << error;
                ASSERT_EQ (a.valid, e.valid) << "spacing " << spacing << " pad " << pad;
                if (!e.valid)
                    continue;
                ++built;
                EXPECT_EQ (a.columns, e.columns);
                EXPECT_EQ (a.rows, e.rows);
                EXPECT_TRUE (SameBits (e.pad, a.pad));
                EXPECT_TRUE (SameBits (e.groundZ, a.groundZ));
                ExpectSameBits (e.positions, a.positions, "positions");
                ExpectSameBits (e.normals, a.normals, "normals");
            }
        }
    }
    EXPECT_GT (built, 20u);
}

namespace {

struct Soup {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
};

void AppendBox (Soup& soup, const Vec3& o, double s, bool inward, uint32_t group, bool shareVertices)
{
    const double corners[8][3] = {
        { o[0], o[1], o[2] },
        { o[0] + s, o[1], o[2] },
        { o[0] + s, o[1] + s, o[2] },
        { o[0], o[1] + s, o[2] },
        { o[0], o[1], o[2] + s },
        { o[0] + s, o[1], o[2] + s },
        { o[0] + s, o[1] + s, o[2] + s },
        { o[0], o[1] + s, o[2] + s },
    };
    const uint32_t faces[36] = { 0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7 };
    const uint32_t base = static_cast<uint32_t> (soup.vertices.size () / 3);
    if (shareVertices)
        for (const auto& c : corners)
            soup.vertices.insert (soup.vertices.end (), { c[0], c[1], c[2] });
    for (int f = 0; f < 12; ++f) {
        uint32_t tri[3] = { faces[f * 3], faces[f * 3 + 1], faces[f * 3 + 2] };
        if (inward)
            std::swap (tri[1], tri[2]);
        for (const uint32_t corner : tri) {
            if (shareVertices) {
                soup.triangles.push_back (base + corner);
            }
            else {
                soup.triangles.push_back (static_cast<uint32_t> (soup.vertices.size () / 3));
                soup.vertices.insert (soup.vertices.end (),
                                      { corners[corner][0], corners[corner][1], corners[corner][2] });
            }
        }
        soup.groups.push_back (group);
    }
}

void ExpectWindingParity (const Soup& soup, bool useGroups, double tolerance)
{
    WindingReport e;
    const std::vector<uint32_t> expected =
        OrientOutward (soup.vertices.data (), soup.vertices.size () / 3, soup.triangles.data (),
                       soup.triangles.size () / 3, useGroups ? soup.groups.data () : nullptr, e, tolerance);
    WindingReport a;
    std::vector<uint32_t> actual;
    std::string error;
    const std::vector<uint32_t> none;
    ASSERT_TRUE (
        rs::OrientOutward (soup.vertices, soup.triangles, useGroups ? soup.groups : none, tolerance, actual, a, error))
        << error;
    EXPECT_EQ (actual, expected);
    EXPECT_EQ (a.groups, e.groups);
    EXPECT_EQ (a.closed, e.closed);
    EXPECT_EQ (a.flipped, e.flipped);
    EXPECT_EQ (a.flippedTriangles, e.flippedTriangles);
}

} // namespace

TEST (SunStudyRustParity, WindingMatchesOnTheBoxCorpus)
{
    Lcg lcg (0x3141ull);
    for (int trial = 0; trial < 60; ++trial) {
        Soup soup;
        const int boxes = lcg.Int (1, 5);
        const bool survey = trial % 4 == 0;
        for (int b = 0; b < boxes; ++b) {
            const Vec3 origin { (survey ? 624000.0 : 0.0) + 10.0 * b, survey ? 6172000.0 : 0.0, lcg.Uniform (0, 5) };
            AppendBox (soup, origin, lcg.Uniform (0.5, 4.0), lcg.Int (0, 1) == 1, static_cast<uint32_t> (b * 7),
                       lcg.Int (0, 1) == 1);
        }
        if (trial % 5 == 1) { // open one element
            soup.triangles.resize (soup.triangles.size () - 3);
            soup.groups.pop_back ();
        }
        if (trial % 7 == 2) { // an out-of-range face
            soup.triangles.insert (soup.triangles.end (), { 999999u, 0u, 1u });
            soup.groups.push_back (0);
        }
        for (const double tolerance : { 1e-4, 0.0, 1e-2 }) {
            ExpectWindingParity (soup, true, tolerance);
            ExpectWindingParity (soup, false, tolerance);
        }
    }
}

TEST (SunStudyRustParity, WindingRefusesWhereTheOracleWouldOverread)
{
    Soup soup;
    AppendBox (soup, { 0, 0, 0 }, 2.0, true, 0, true);
    soup.groups.resize (3);
    std::vector<uint32_t> out = { 42 };
    WindingReport report;
    std::string error;
    EXPECT_FALSE (rs::OrientOutward (soup.vertices, soup.triangles, soup.groups, 1e-4, out, report, error));
    EXPECT_NE (error.find ("WindingGroupCount"), std::string::npos) << error;
    EXPECT_EQ (out, std::vector<uint32_t> { 42 }) << "a refusal leaves the output untouched";
}

TEST (SunStudyRustParity, AnalysisLimitsMatch)
{
    for (const bool gpuKnown : { false, true }) {
        for (const uint64_t gpu : { 0ull, 128ull << 20, 256ull << 20, 24ull << 30 }) {
            for (const uint32_t texture : { 0u, 2048u, 16384u }) {
                for (const uint64_t ram : { 0ull, 512ull << 20, 64ull << 30 }) {
                    for (const size_t steps :
                         { size_t { 0 }, size_t { 1 }, size_t { 49 }, size_t { 64 }, size_t { 65 }, size_t { 289 } }) {
                        for (const double occupancy : { kPatchAtlasOccupancy, kTriangleAtlasOccupancy, 0.0, 2.0 }) {
                            MachineResources machine;
                            machine.gpuKnown = gpuKnown;
                            machine.gpuMemoryBytes = gpu;
                            machine.maxTextureDimension = texture;
                            machine.availableRamBytes = ram;
                            const AnalysisLimits e = evp::sunstudy::ComputeAnalysisLimits (machine, steps, occupancy);
                            AnalysisLimits a;
                            double finest = 0.0;
                            std::string error;
                            ASSERT_TRUE (
                                rs::ComputeAnalysisLimits (machine, steps, occupancy, 12345.0, a, finest, error))
                                << error;
                            EXPECT_EQ (a.maxSamples, e.maxSamples);
                            EXPECT_EQ (a.maxAtlasDimension, e.maxAtlasDimension);
                            EXPECT_EQ (a.binding, e.binding);
                            EXPECT_EQ (a.maxRays, e.maxRays);
                            EXPECT_EQ (a.samplerCap, e.samplerCap);
                            EXPECT_EQ (a.ramCap, e.ramCap);
                            EXPECT_EQ (a.gpuCap, e.gpuCap);
                            EXPECT_EQ (a.textureCap, e.textureCap);
                            EXPECT_TRUE (SameBits (FinestGridSpacing (12345.0, e.maxSamples), finest));
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// P2a: roles, receivers, preset advice, selection binding
// ---------------------------------------------------------------------------

namespace {

// A GUID universe with spelling variants the host really produces: braced or
// bare, either case, stray C whitespace.
std::string Spell (const std::string& guid, Lcg& lcg)
{
    std::string out;
    for (const char c : guid)
        out.push_back (lcg.Int (0, 1) == 0 ? c : static_cast<char> (std::tolower (static_cast<unsigned char> (c))));
    switch (lcg.Int (0, 4)) {
        case 0:
            return "{" + out + "}";
        case 1:
            return " " + out + "\t";
        case 2:
            return "{" + out + "}\v";
        default:
            return out;
    }
}

std::vector<std::string> Pick (const std::vector<std::string>& universe, Lcg& lcg, int maxCount)
{
    std::vector<std::string> picked;
    const int count = lcg.Int (0, maxCount);
    for (int i = 0; i < count; ++i)
        picked.push_back (
            Spell (universe[static_cast<size_t> (lcg.Int (0, static_cast<int> (universe.size ()) - 1))], lcg));
    if (count > 0 && lcg.Int (0, 5) == 0)
        picked.push_back (lcg.Int (0, 1) == 0 ? "{}" : "  "); // blank picks
    return picked;
}

std::vector<std::string> Universe (Lcg& lcg, int count)
{
    std::vector<std::string> guids;
    for (int i = 0; i < count; ++i) {
        char buffer[40];
        std::snprintf (buffer, sizeof (buffer), "%08X-AB%02d-4C%02d", static_cast<unsigned> (lcg.Uniform (0, 4e9)), i,
                       lcg.Int (0, 99));
        guids.push_back (buffer);
    }
    return guids;
}

void ExpectSameRoles (const ElementRoles& e, const ElementRoles& a)
{
    EXPECT_EQ (a.roles, e.roles);
    EXPECT_EQ (a.analysis, e.analysis);
    EXPECT_EQ (a.context, e.context);
    EXPECT_EQ (a.ignored, e.ignored);
    EXPECT_EQ (a.unmatchedAnalysis, e.unmatchedAnalysis);
    EXPECT_EQ (a.unmatchedContext, e.unmatchedContext);
    EXPECT_EQ (a.unmatchedIgnored, e.unmatchedIgnored);
    EXPECT_EQ (a.analysisNamedButAbsent, e.analysisNamedButAbsent);
}

} // namespace

TEST (SunStudyRustParity, RolesMatchOverRandomPickCombinations)
{
    Lcg lcg (0x701e5ull);
    for (int trial = 0; trial < 400; ++trial) {
        const std::vector<std::string> universe = Universe (lcg, lcg.Int (1, 12));
        std::vector<std::string> elements;
        for (const std::string& guid : universe)
            if (lcg.Int (0, 4) != 0)
                elements.push_back (Spell (guid, lcg)); // some picks will be absent from the model
        const auto analysis = Pick (universe, lcg, trial % 4 == 0 ? 0 : 4);
        const auto context = Pick (universe, lcg, trial % 3 == 0 ? 0 : 4);
        const auto ignored = Pick (universe, lcg, trial % 2 == 0 ? 0 : 3);
        const ElementRoles expected = evp::sunstudy::ResolveElementRoles (elements, analysis, context, ignored);
        ElementRoles actual;
        std::string error;
        ASSERT_TRUE (rs::ResolveElementRoles (elements, analysis, context, ignored, actual, error)) << error;
        ExpectSameRoles (expected, actual);
    }
}

TEST (SunStudyRustParity, ReceiversMatchOverRandomMaterialPools)
{
    Lcg lcg (0x9a55ull);
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    for (int trial = 0; trial < 300; ++trial) {
        geomsrv::Snapshot snapshot;
        std::vector<std::string> guids;
        const int meshCount = lcg.Int (0, 6);
        for (int m = 0; m < meshCount; ++m) {
            geomsrv::Mesh mesh;
            mesh.guid = "M" + std::to_string (m);
            const int faces = lcg.Int (0, 14);
            mesh.triangles.assign (static_cast<size_t> (faces) * 3, 0u);
            const int materials = lcg.Int (0, 3) == 0 ? lcg.Int (0, faces) : faces; // sometimes short
            for (int f = 0; f < materials; ++f)
                mesh.triMaterial.push_back (lcg.Int (0, 5));
            guids.push_back (mesh.guid);
            snapshot.meshes.push_back (mesh);
        }
        std::map<int32_t, double> pool;
        for (int material = 0; material < 6; ++material)
            if (lcg.Int (0, 3) != 0)
                pool[material] = lcg.Int (0, 6) == 0 ? nan : lcg.Uniform (0, 1);
        const auto pick = [&] (int keep) {
            std::vector<std::string> out;
            for (const auto& g : guids)
                if (lcg.Int (0, keep) == 0)
                    out.push_back (g);
            return out;
        };
        const auto analysisPick = pick (3);
        const auto contextPick = pick (3);
        const auto ignoredPick = pick (5);
        const ElementRoles roles =
            evp::sunstudy::ResolveElementRoles (snapshot, analysisPick, contextPick, ignoredPick);
        for (const bool glassOnly : { false, true }) {
            for (const double threshold : { 0.4, 0.0, 1.0, nan }) {
                const SunStudyReceivers e =
                    evp::sunstudy::BuildSunStudyReceivers (snapshot, roles, glassOnly, pool, threshold);
                SunStudyReceivers a;
                std::string error;
                ASSERT_TRUE (rs::BuildSunStudyReceivers (snapshot, roles, glassOnly, pool, threshold, a, error))
                    << error;
                ExpectSameRoles (e.roles, a.roles);
                EXPECT_EQ (a.faces, e.faces);
                EXPECT_EQ (a.analysisFaces, e.analysisFaces);
                EXPECT_EQ (a.contextFaces, e.contextFaces);
                EXPECT_EQ (a.unknownMaterialFaces, e.unknownMaterialFaces);
            }
        }
    }
}

TEST (SunStudyRustParity, ReceiversWithMismatchedRolesStayUnchanged)
{
    geomsrv::Snapshot snapshot;
    snapshot.meshes.resize (2);
    const ElementRoles roles = evp::sunstudy::ResolveElementRoles (std::vector<std::string> { "a" }, {}, {}, {});
    const SunStudyReceivers e = evp::sunstudy::BuildSunStudyReceivers (snapshot, roles, true, {}, 0.4);
    SunStudyReceivers a;
    std::string error;
    ASSERT_TRUE (rs::BuildSunStudyReceivers (snapshot, roles, true, {}, 0.4, a, error)) << error;
    ExpectSameRoles (e.roles, a.roles);
    EXPECT_EQ (a.faces, e.faces);
}

TEST (SunStudyRustParity, PresetAdviceMatches)
{
    for (const size_t triangles : { size_t { 0 }, size_t { 1 }, size_t { 43959 }, size_t { 50000 }, size_t { 50001 },
                                    size_t { 100000 }, size_t { 249999 }, size_t { 250000 }, size_t { 818230 } }) {
        for (const std::string preset : { "early", "late", "", "Early", "custom" }) {
            std::string actual = "stale";
            std::string error;
            ASSERT_TRUE (rs::SunStudyPresetAdvice (triangles, preset, actual, error)) << error;
            EXPECT_EQ (actual, evp::sunstudy::SunStudyPresetAdvice (triangles, preset)) << triangles << " " << preset;
        }
    }
}

TEST (SunStudyRustParity, SelectionBindingMatchesOverScriptedRefreshes)
{
    Lcg lcg (0xb1dull);
    const std::vector<std::string> universe = Universe (lcg, 6);
    for (int trial = 0; trial < 300; ++trial) {
        SunStudySelectionBinding binding;
        if (lcg.Int (0, 4) != 0) {
            binding.generation = static_cast<uint64_t> (lcg.Int (1, 3));
            binding.contextSet = lcg.Int (0, 2) == 0 ? "" : "Context";
            binding.ignoredSet = lcg.Int (0, 2) == 0 ? "" : "Ignored";
            binding.analysisSet = lcg.Int (0, 2) == 0 ? "" : "Analysis";
            binding.revision = 10;
        }
        SunStudySelectionBinding rustBinding = binding;
        std::vector<std::string> context = Pick (universe, lcg, 3);
        std::vector<std::string> ignored = Pick (universe, lcg, 3);
        std::vector<std::string> analysis = Pick (universe, lcg, 3);
        std::vector<std::string> rustContext = context;
        std::vector<std::string> rustIgnored = ignored;
        std::vector<std::string> rustAnalysis = analysis;
        const bool withAnalysis = lcg.Int (0, 1) == 1;
        for (int step = 0; step < 4; ++step) {
            const uint64_t current = static_cast<uint64_t> (lcg.Int (1, 3));
            const auto liveC = lcg.Int (0, 1) == 0 ? context : Pick (universe, lcg, 3);
            const auto liveI = lcg.Int (0, 1) == 0 ? ignored : Pick (universe, lcg, 3);
            const auto liveA = lcg.Int (0, 1) == 0 ? analysis : Pick (universe, lcg, 3);
            const SelectionBindingRefresh e =
                binding.Refresh (current, liveC, liveI, context, ignored, liveA, withAnalysis ? &analysis : nullptr);
            SelectionBindingRefresh a = SelectionBindingRefresh::Inactive;
            std::string error;
            ASSERT_TRUE (rs::RefreshSelectionBinding (rustBinding, current, liveC, liveI, rustContext, rustIgnored,
                                                      liveA, withAnalysis ? &rustAnalysis : nullptr, a, error))
                << error;
            ASSERT_EQ (a, e) << "trial " << trial << " step " << step;
            EXPECT_EQ (rustContext, context);
            EXPECT_EQ (rustIgnored, ignored);
            EXPECT_EQ (rustAnalysis, analysis);
            EXPECT_EQ (rustBinding.generation, binding.generation);
            EXPECT_EQ (rustBinding.contextSet, binding.contextSet);
            EXPECT_EQ (rustBinding.ignoredSet, binding.ignoredSet);
            EXPECT_EQ (rustBinding.analysisSet, binding.analysisSet);
            EXPECT_EQ (rustBinding.revision, binding.revision);
        }
    }
}

// ---------------------------------------------------------------------------
// P2b: the triangle-domain sampler
// ---------------------------------------------------------------------------

namespace {

struct SamplerCase {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
};

// Random soups that reach every branch: in-plane grids on tilted and vertical
// faces, slivers below the grid, collinear and out-of-range faces, and survey
// coordinates.
SamplerCase RandomSoup (Lcg& lcg, bool survey)
{
    SamplerCase soup;
    const int faces = lcg.Int (1, 24);
    const double ox = survey ? 624000.0 : 0.0;
    const double oy = survey ? 6172000.0 : 0.0;
    for (int f = 0; f < faces; ++f) {
        const uint32_t base = static_cast<uint32_t> (soup.vertices.size () / 3);
        const double scale = lcg.Int (0, 4) == 0 ? lcg.Uniform (0.01, 0.4) : lcg.Uniform (1.0, 12.0);
        const double x = ox + lcg.Uniform (-20, 20), y = oy + lcg.Uniform (-20, 20), z = lcg.Uniform (0, 10);
        for (int corner = 0; corner < 3; ++corner) {
            soup.vertices.push_back (x + scale * lcg.Uniform (-1, 1));
            soup.vertices.push_back (y + scale * lcg.Uniform (-1, 1));
            soup.vertices.push_back (lcg.Int (0, 2) == 0 ? z : z + scale * lcg.Uniform (-1, 1));
        }
        if (lcg.Int (0, 9) == 0) // collinear
            for (int axis = 0; axis < 3; ++axis)
                soup.vertices[(base + 2) * 3 + axis] =
                    2.0 * soup.vertices[(base + 1) * 3 + axis] - soup.vertices[base * 3 + axis];
        soup.triangles.insert (soup.triangles.end (), { base, base + 1, base + 2 });
        if (lcg.Int (0, 19) == 0)
            soup.triangles.back () = 999999u; // out of range
        soup.groups.push_back (static_cast<uint32_t> (lcg.Int (0, 4)));
    }
    return soup;
}

void ExpectSameSampleGrids (const SampleGrid& e, const SampleGrid& a)
{
    ASSERT_EQ (a.valid, e.valid);
    EXPECT_EQ (a.degenerateFaces, e.degenerateFaces);
    EXPECT_EQ (a.excludedFaces, e.excludedFaces);
    EXPECT_EQ (a.undersizedFaces, e.undersizedFaces);
    if (!e.valid)
        return; // a refused C++ grid holds an unspecified partial prefix
    ExpectSameBits (e.positions, a.positions, "positions");
    ExpectSameBits (e.normals, a.normals, "normals");
    ExpectSameBits (e.areas, a.areas, "areas");
    EXPECT_EQ (a.faces, e.faces);
    EXPECT_EQ (a.groups, e.groups);
    EXPECT_EQ (a.cellColumns, e.cellColumns);
    EXPECT_EQ (a.cellRows, e.cellRows);
    ASSERT_EQ (a.layouts.size (), e.layouts.size ());
    for (size_t f = 0; f < e.layouts.size (); ++f) {
        const FaceLayout& x = e.layouts[f];
        const FaceLayout& y = a.layouts[f];
        for (int axis = 0; axis < 3; ++axis) {
            ASSERT_TRUE (SameBits (x.origin[axis], y.origin[axis])) << "face " << f;
            ASSERT_TRUE (SameBits (x.uAxis[axis], y.uAxis[axis])) << "face " << f;
            ASSERT_TRUE (SameBits (x.vAxis[axis], y.vAxis[axis])) << "face " << f;
        }
        EXPECT_EQ (y.uStart, x.uStart) << "face " << f;
        EXPECT_EQ (y.vStart, x.vStart) << "face " << f;
        EXPECT_EQ (y.columns, x.columns) << "face " << f;
        EXPECT_EQ (y.rows, x.rows) << "face " << f;
        EXPECT_EQ (y.gridded, x.gridded) << "face " << f;
    }
}

} // namespace

TEST (SunStudyRustParity, SampleGridMatchesEveryBit)
{
    Lcg lcg (0x5a3b1eull);
    size_t valid = 0;
    size_t refused = 0;
    size_t samples = 0;
    for (int trial = 0; trial < 160; ++trial) {
        const SamplerCase soup = RandomSoup (lcg, trial % 5 == 0);
        const std::vector<uint8_t> sampleGroup { 1, 0, 1 }; // group 1 context; 3, 4 past the end
        std::vector<uint8_t> sampleFace;
        for (size_t f = 0; f + 2 < soup.triangles.size () / 3; ++f) // deliberately short
            sampleFace.push_back (static_cast<uint8_t> (lcg.Int (0, 3) == 0 ? 0 : 1));
        for (const double spacing : { 1.0, 0.37, 2.5 }) {
            for (const double jitter : { 0.0, 0.8, 1.5 }) {
                SamplerOptions options;
                options.spacing = spacing;
                options.jitter = jitter;
                options.normalOffset = trial % 2 == 0 ? 0.01 : 0.2;
                options.jitterFaceOffset = static_cast<uint32_t> (trial * 977);
                options.maxSamples = trial % 7 == 3 ? 40 : 4000000;
                options.wantLayouts = trial % 3 != 0;
                options.sampleGroup = trial % 4 == 1 ? &sampleGroup : nullptr;
                options.sampleFace = trial % 4 == 2 ? &sampleFace : nullptr;
                const bool withGroups = trial % 6 != 5;
                const uint32_t* groups = withGroups ? soup.groups.data () : nullptr;
                const SampleGrid e = evp::sunstudy::BuildSampleGrid (soup.vertices.data (), soup.vertices.size () / 3,
                                                                     soup.triangles.data (), soup.triangles.size () / 3,
                                                                     groups, options);
                SampleGrid a;
                std::string error;
                ASSERT_TRUE (rs::BuildSampleGrid (soup.vertices.data (), soup.vertices.size () / 3,
                                                  soup.triangles.data (), soup.triangles.size () / 3, groups, options,
                                                  a, error))
                    << error;
                ExpectSameSampleGrids (e, a);
                if (::testing::Test::HasFatalFailure ())
                    return;
                (e.valid ? valid : refused) += 1;
                samples += e.valid ? e.Count () : 0;
            }
        }
    }
    EXPECT_GT (valid, 1000u);
    EXPECT_GT (refused, 0u);
    EXPECT_GT (samples, 100000u);
}

TEST (SunStudyRustParity, SampleGridRefusesEmptyInputAsTheOracleDoes)
{
    for (const double spacing : { 1.0, 0.0, -2.0 }) {
        SamplerOptions options;
        options.spacing = spacing;
        const std::vector<double> vertices { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        const std::vector<uint32_t> triangles { 0, 1, 2 };
        for (const bool empty : { false, true }) {
            const SampleGrid e = evp::sunstudy::BuildSampleGrid (empty ? nullptr : vertices.data (), 3,
                                                                 triangles.data (), empty ? 0 : 1, nullptr, options);
            SampleGrid a;
            std::string error;
            ASSERT_TRUE (rs::BuildSampleGrid (empty ? nullptr : vertices.data (), 3, triangles.data (), empty ? 0 : 1,
                                              nullptr, options, a, error))
                << error;
            ExpectSameSampleGrids (e, a);
        }
    }
}

// ---------------------------------------------------------------------------
// P2c: surface patches and the patch-domain sampler
// ---------------------------------------------------------------------------

namespace {

// A tessellated w x h rectangle in an arbitrary frame (origin, u, v).
void AppendRectangle (SamplerCase& soup, const Vec3& o, const Vec3& u, const Vec3& v, double w, double h, int strips,
                      uint32_t group, bool flip)
{
    const uint32_t base = static_cast<uint32_t> (soup.vertices.size () / 3);
    for (int i = 0; i <= strips; ++i) {
        const double x = w * i / strips;
        for (const double y : { 0.0, h })
            for (int axis = 0; axis < 3; ++axis)
                soup.vertices.push_back (o[axis] + u[axis] * x + v[axis] * y);
    }
    for (int i = 0; i < strips; ++i) {
        const uint32_t a = base + 2 * i, b = a + 2, c = a + 3, d = a + 1;
        if (flip)
            soup.triangles.insert (soup.triangles.end (), { a, c, b, a, d, c });
        else
            soup.triangles.insert (soup.triangles.end (), { a, b, c, a, c, d });
        soup.groups.insert (soup.groups.end (), { group, group });
    }
}

Vec3 Unit (const Vec3& v)
{
    const double length = std::sqrt (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return { v[0] / length, v[1] / length, v[2] / length };
}

SamplerCase PatchScene (Lcg& lcg, bool survey)
{
    SamplerCase soup;
    const double ox = survey ? 624000.0 : 0.0, oy = survey ? 6172000.0 : 0.0;
    const int pieces = lcg.Int (1, 6);
    for (int p = 0; p < pieces; ++p) {
        const Vec3 origin { ox + lcg.Uniform (-15, 15), oy + lcg.Uniform (-15, 15), lcg.Uniform (0, 6) };
        Vec3 u, v;
        switch (lcg.Int (0, 3)) {
            case 0: // floor
                u = { 1, 0, 0 };
                v = { 0, 1, 0 };
                break;
            case 1: // facade at an angle
                u = Unit ({ lcg.Uniform (-1, 1), lcg.Uniform (-1, 1), 0 });
                v = { 0, 0, 1 };
                break;
            case 2: // pitched roof
                u = Unit ({ lcg.Uniform (-1, 1), lcg.Uniform (-1, 1), 0 });
                v = Unit ({ -u[1], u[0], lcg.Uniform (0.2, 1.5) });
                break;
            default: // tiny mullion below the grid
                u = { 1, 0, 0 };
                v = { 0, 0, 1 };
                break;
        }
        const bool tiny = u[2] == 0 && v[2] == 1 && lcg.Int (0, 1) == 0;
        const double w = tiny ? lcg.Uniform (0.02, 0.3) : lcg.Uniform (0.5, 9);
        const double h = tiny ? lcg.Uniform (0.02, 0.3) : lcg.Uniform (0.5, 6);
        AppendRectangle (soup, origin, u, v, w, h, lcg.Int (1, 6), static_cast<uint32_t> (lcg.Int (0, 3)),
                         lcg.Int (0, 4) == 0);
        if (lcg.Int (0, 5) == 0) // an identical disconnected copy: same key, next occurrence
            AppendRectangle (soup, origin, u, v, w, h, 1, soup.groups.back (), false);
    }
    if (lcg.Int (0, 3) == 0) { // a faceted cylinder: near-coplanar strips must not merge
        const uint32_t group = 2;
        for (int s = 0; s < 8; ++s) {
            const double a0 = s * 0.01, a1 = (s + 1) * 0.01; // under one degree per facet
            const Vec3 p0 { ox + 30 * std::cos (a0), oy + 30 * std::sin (a0), 0 };
            const Vec3 p1 { ox + 30 * std::cos (a1), oy + 30 * std::sin (a1), 0 };
            const uint32_t base = static_cast<uint32_t> (soup.vertices.size () / 3);
            for (const Vec3& p : { p0, p1, Vec3 { p1[0], p1[1], 3 }, Vec3 { p0[0], p0[1], 3 } })
                soup.vertices.insert (soup.vertices.end (), { p[0], p[1], p[2] });
            soup.triangles.insert (soup.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
            soup.groups.insert (soup.groups.end (), { group, group });
        }
    }
    return soup;
}

void ExpectSamePatchGrids (const PatchSampleGrid& e, const PatchSampleGrid& a)
{
    ASSERT_EQ (a.valid, e.valid);
    ASSERT_EQ (a.sampleLimitExceeded, e.sampleLimitExceeded);
    EXPECT_EQ (a.degenerateFaces, e.degenerateFaces);
    EXPECT_EQ (a.centroidPatches, e.centroidPatches);
    EXPECT_EQ (a.excludedPatches, e.excludedPatches);
    if (e.sampleLimitExceeded)
        return; // refused; the C++ grid holds no samples either
    ExpectSameBits (e.positions, a.positions, "positions");
    ExpectSameBits (e.normals, a.normals, "normals");
    ExpectSameBits (e.areas, a.areas, "areas");
    EXPECT_EQ (a.cellColumns, e.cellColumns);
    EXPECT_EQ (a.cellRows, e.cellRows);
    EXPECT_EQ (a.spanOf, e.spanOf);
    EXPECT_EQ (a.patchOfTriangle, e.patchOfTriangle);
    ASSERT_EQ (a.spans.size (), e.spans.size ());
    for (size_t s = 0; s < e.spans.size (); ++s) {
        const PatchSampleSpan& x = e.spans[s];
        const PatchSampleSpan& y = a.spans[s];
        EXPECT_TRUE (y.key == x.key) << "span " << s << " key";
        EXPECT_EQ (y.first, x.first);
        EXPECT_EQ (y.count, x.count);
        EXPECT_EQ (y.columns, x.columns);
        EXPECT_EQ (y.rows, x.rows);
        EXPECT_TRUE (SameBits (x.area, y.area)) << "span " << s;
        for (int axis = 0; axis < 3; ++axis) {
            ASSERT_TRUE (SameBits (x.origin[axis], y.origin[axis])) << "span " << s;
            ASSERT_TRUE (SameBits (x.uAxis[axis], y.uAxis[axis])) << "span " << s;
            ASSERT_TRUE (SameBits (x.vAxis[axis], y.vAxis[axis])) << "span " << s;
            ASSERT_TRUE (SameBits (x.normal[axis], y.normal[axis])) << "span " << s;
            ASSERT_TRUE (SameBits (x.boundsMin[axis], y.boundsMin[axis])) << "span " << s;
            ASSERT_TRUE (SameBits (x.boundsMax[axis], y.boundsMax[axis])) << "span " << s;
        }
    }
}

} // namespace

TEST (SunStudyRustParity, PatchGridMatchesEveryBit)
{
    Lcg lcg (0xba7c4ull);
    size_t valid = 0, refused = 0, spans = 0, samples = 0;
    for (int trial = 0; trial < 120; ++trial) {
        const SamplerCase soup = PatchScene (lcg, trial % 4 == 0);
        const size_t faces = soup.triangles.size () / 3;
        const std::vector<std::string> elementOf =
            trial % 5 == 4 ? std::vector<std::string> { "{A}" } // short: groups past the end key on ""
                           : std::vector<std::string> { "{A}", "{B}", "{C}", "{D}" };
        const std::vector<uint8_t> sampleGroup { 1, 0, 1 };
        std::vector<uint8_t> sampleFace (faces);
        for (auto& flag : sampleFace)
            flag = static_cast<uint8_t> (lcg.Int (0, 4) == 0 ? 0 : 1);
        for (const double spacing : { 1.0, 0.45, 3.0 }) {
            PatchSamplerOptions options;
            options.spacing = spacing;
            options.normalOffset = trial % 2 == 0 ? 0.01 : 0.15;
            options.maxSamples = trial % 9 == 4 ? 30 : 4000000;
            options.sampleGroup = trial % 3 == 1 ? &sampleGroup : nullptr;
            options.sampleFace = trial % 3 == 2 ? &sampleFace : nullptr;
            if (trial % 7 == 3)
                options.patch.coplanarDegrees = 5.0; // merges the faceted cylinder
            const uint32_t* groups = trial % 6 == 5 ? nullptr : soup.groups.data ();
            const PatchSampleGrid e =
                evp::sunstudy::BuildPatchSampleGrid (soup.vertices.data (), soup.vertices.size () / 3,
                                                     soup.triangles.data (), faces, groups, elementOf, options);
            PatchSampleGrid a;
            std::string error;
            ASSERT_TRUE (rs::BuildPatchSampleGrid (soup.vertices.data (), soup.vertices.size () / 3,
                                                   soup.triangles.data (), faces, groups, elementOf, options, a, error))
                << error;
            ExpectSamePatchGrids (e, a);
            if (::testing::Test::HasFatalFailure ())
                return;
            (e.valid ? valid : refused) += 1;
            spans += e.spans.size ();
            samples += e.Count ();
        }
    }
    EXPECT_GT (valid, 200u);
    EXPECT_GT (refused, 0u);
    EXPECT_GT (spans, 1000u);
    EXPECT_GT (samples, 20000u);
}

// ---------------------------------------------------------------------------
// P2d: the atlas family
// ---------------------------------------------------------------------------

namespace {

void ExpectSameAtlas (const SunStudyAtlas& e, const SunStudyAtlas& a, const char* what)
{
    ASSERT_EQ (a.valid, e.valid) << what;
    EXPECT_EQ (a.width, e.width) << what;
    EXPECT_EQ (a.height, e.height) << what;
    EXPECT_EQ (a.placedFaces, e.placedFaces) << what;
    EXPECT_EQ (a.texels, e.texels) << what;
    ASSERT_EQ (a.tiles.size (), e.tiles.size ()) << what;
    for (size_t i = 0; i < e.tiles.size (); ++i) {
        EXPECT_EQ (a.tiles[i].x, e.tiles[i].x) << what << " tile " << i;
        EXPECT_EQ (a.tiles[i].y, e.tiles[i].y) << what << " tile " << i;
        EXPECT_EQ (a.tiles[i].width, e.tiles[i].width) << what << " tile " << i;
        EXPECT_EQ (a.tiles[i].height, e.tiles[i].height) << what << " tile " << i;
    }
}

void ExpectSameAllocations (const SunStudyPatchAtlas& e, const rs::PatchAtlasState& a, const char* what)
{
    EXPECT_EQ (a.width, e.Width ()) << what;
    EXPECT_EQ (a.height, e.Height ()) << what;
    ASSERT_EQ (a.allocations.size (), e.Allocations ().size ()) << what;
    auto x = e.Allocations ().begin ();
    for (const auto& [key, y] : a.allocations) {
        EXPECT_TRUE (key == x->first) << what << " key " << key.element;
        EXPECT_EQ (y.x, x->second.x) << what;
        EXPECT_EQ (y.y, x->second.y) << what;
        EXPECT_EQ (y.width, x->second.width) << what;
        EXPECT_EQ (y.height, x->second.height) << what;
        EXPECT_EQ (y.generation, x->second.generation) << what;
        ++x;
    }
}

SampleGrid LayoutGrid (Lcg& lcg)
{
    const SamplerCase soup = RandomSoup (lcg, false);
    SamplerOptions options;
    options.spacing = lcg.Int (0, 1) == 0 ? 0.5 : 1.3;
    options.wantLayouts = true;
    return evp::sunstudy::BuildSampleGrid (soup.vertices.data (), soup.vertices.size () / 3, soup.triangles.data (),
                                           soup.triangles.size () / 3, nullptr, options);
}

PatchSampleGrid SpanGrid (Lcg& lcg)
{
    // Spans only matter for their keys and lattice sizes; vary both.
    PatchSampleGrid grid;
    const int count = lcg.Int (0, 30);
    for (int i = 0; i < count; ++i) {
        PatchSampleSpan span;
        span.key.element = std::string (1, static_cast<char> ('A' + lcg.Int (0, 4)));
        span.key.plane = static_cast<uint64_t> (lcg.Int (0, 12));
        span.key.occurrence = static_cast<uint32_t> (lcg.Int (0, 1));
        span.columns = static_cast<uint32_t> (lcg.Int (0, 3) == 0 ? lcg.Int (1, 40) : lcg.Int (1, 8));
        span.rows = static_cast<uint32_t> (lcg.Int (1, 8));
        bool duplicate = false;
        for (const auto& existing : grid.spans)
            duplicate = duplicate || existing.key == span.key;
        if (!duplicate)
            grid.spans.push_back (span);
    }
    return grid;
}

} // namespace

TEST (SunStudyRustParity, TriangleAtlasMatches)
{
    Lcg lcg (0xa71a5ull);
    size_t placed = 0;
    for (int trial = 0; trial < 120; ++trial) {
        SampleGrid grid = LayoutGrid (lcg);
        if (trial % 9 == 4)
            grid.layouts.clear (); // no layouts: no atlas
        AtlasOptions options;
        options.gutter = static_cast<uint32_t> (lcg.Int (0, 3));
        options.maxDimension = trial % 7 == 3 ? 16u : 8192u;
        const SunStudyAtlas e = evp::sunstudy::BuildSunStudyAtlas (grid, options);
        SunStudyAtlas a;
        std::string error;
        ASSERT_TRUE (rs::BuildSunStudyAtlas (grid, options, a, error)) << error;
        ExpectSameAtlas (e, a, "triangle atlas");
        placed += e.placedFaces;
    }
    EXPECT_GT (placed, 500u);
}

TEST (SunStudyRustParity, PatchAtlasFitsMatchInLockstep)
{
    Lcg lcg (0xf17ull);
    size_t retained = 0, resized = 0;
    for (int trial = 0; trial < 40; ++trial) {
        SunStudyPatchAtlas expected;
        rs::PatchAtlasState actual;
        const uint32_t gutter = static_cast<uint32_t> (lcg.Int (0, 2));
        const uint32_t maxDimension = trial % 5 == 2 ? 64u : 8192u;
        PatchSampleGrid grid = SpanGrid (lcg);
        for (int step = 0; step < 6; ++step) {
            if (step > 0 && lcg.Int (0, 2) != 0) { // edit: resize, drop or add some spans
                for (auto& span : grid.spans)
                    if (lcg.Int (0, 5) == 0)
                        span.columns += 1;
                if (!grid.spans.empty () && lcg.Int (0, 3) == 0)
                    grid.spans.erase (grid.spans.begin ());
                const PatchSampleGrid more = SpanGrid (lcg);
                for (const auto& span : more.spans) {
                    bool duplicate = false;
                    for (const auto& existing : grid.spans)
                        duplicate = duplicate || existing.key == span.key;
                    if (!duplicate && lcg.Int (0, 3) == 0)
                        grid.spans.push_back (span);
                }
            }
            const PatchAtlasUpdate e = expected.Fit (grid, gutter, maxDimension);
            PatchAtlasUpdate a;
            std::string error;
            ASSERT_TRUE (rs::FitPatchAtlas (actual, grid, gutter, maxDimension, a, error)) << error;
            EXPECT_EQ (a.retained, e.retained);
            EXPECT_EQ (a.reused, e.reused);
            EXPECT_EQ (a.moved, e.moved);
            EXPECT_EQ (a.added, e.added);
            EXPECT_EQ (a.removed, e.removed);
            EXPECT_EQ (a.resized, e.resized);
            ExpectSameAllocations (expected, actual, "fit");
            if (::testing::Test::HasFatalFailure ())
                return;
            retained += e.retained;
            resized += e.resized ? 1 : 0;
        }
    }
    EXPECT_GT (retained, 100u);
    EXPECT_GT (resized, 10u);
}

TEST (SunStudyRustParity, StableTriangleAtlasMatchesAcrossEdits)
{
    Lcg lcg (0x57ab1eull);
    for (int trial = 0; trial < 40; ++trial) {
        geomsrv::Snapshot snapshot;
        SamplerCase soup;
        const int meshes = lcg.Int (1, 4);
        for (int m = 0; m < meshes; ++m) {
            geomsrv::Mesh mesh;
            mesh.guid = m == 0 && trial % 6 == 5 ? "{dup}" : "{M" + std::to_string (m) + "}";
            if (m == 1 && trial % 6 == 5)
                mesh.guid = "DUP"; // canonical duplicate: falls back
            const SamplerCase part = RandomSoup (lcg, false);
            mesh.vertices = part.vertices;
            mesh.triangles = part.triangles;
            for (uint32_t& index : mesh.triangles)
                if (index >= mesh.vertices.size () / 3)
                    index = 0;
            const uint32_t base = static_cast<uint32_t> (soup.vertices.size () / 3);
            soup.vertices.insert (soup.vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
            for (const uint32_t index : mesh.triangles)
                soup.triangles.push_back (base + index);
            snapshot.meshes.push_back (mesh);
        }
        SamplerOptions sampling;
        sampling.spacing = 0.8;
        sampling.wantLayouts = true;
        const SampleGrid grid =
            evp::sunstudy::BuildSampleGrid (soup.vertices.data (), soup.vertices.size () / 3, soup.triangles.data (),
                                            soup.triangles.size () / 3, nullptr, sampling);
        AtlasOptions options;
        options.maxDimension = trial % 8 == 7 ? 32u : 8192u;
        SunStudyPatchAtlas expectedFirst, expectedSecond;
        rs::PatchAtlasState actualFirst, actualSecond;
        std::string error;
        const SunStudyAtlas e1 =
            evp::sunstudy::BuildStableTriangleAtlas (grid, snapshot, expectedFirst, nullptr, options);
        SunStudyAtlas a1;
        ASSERT_TRUE (rs::BuildStableTriangleAtlas (grid, snapshot, actualFirst, nullptr, options, a1, error)) << error;
        ExpectSameAtlas (e1, a1, "first");
        ExpectSameAllocations (expectedFirst, actualFirst, "first allocations");
        // Rebuild seeded from the first study, as the store does after an edit.
        const SunStudyAtlas e2 =
            evp::sunstudy::BuildStableTriangleAtlas (grid, snapshot, expectedSecond, &expectedFirst, options);
        SunStudyAtlas a2;
        ASSERT_TRUE (rs::BuildStableTriangleAtlas (grid, snapshot, actualSecond, &actualFirst, options, a2, error))
            << error;
        ExpectSameAtlas (e2, a2, "second");
        ExpectSameAllocations (expectedSecond, actualSecond, "second allocations");
        if (::testing::Test::HasFatalFailure ())
            return;
    }
}

TEST (SunStudyRustParity, StepMasksMatch)
{
    Lcg lcg (0x57e9ull);
    for (int trial = 0; trial < 60; ++trial) {
        const size_t samples = static_cast<size_t> (lcg.Int (0, 40));
        const size_t steps = static_cast<size_t> (lcg.Int (0, 100));
        std::vector<uint8_t> bits ((samples * steps + 7) / 8);
        for (auto& byte : bits)
            byte = static_cast<uint8_t> (lcg.Int (0, 255));
        const uint32_t width = static_cast<uint32_t> (lcg.Int (1, 9)), height = static_cast<uint32_t> (lcg.Int (1, 9));
        std::vector<int64_t> texels (samples + (trial % 3 == 0 ? 0 : 3)); // sometimes longer than the samples
        for (auto& texel : texels)
            texel = lcg.Int (0, 5) == 0 ? -1 : lcg.Int (0, static_cast<int> (width * height) + 2);
        const auto lit = [&] (size_t sample, size_t step) {
            const size_t bit = sample * steps + step;
            return ((bits[bit >> 3] >> (bit & 7)) & 1) != 0;
        };
        const StepMaskAtlas e = evp::sunstudy::PackStepMasks (samples, steps, lit, texels, width, height);
        StepMaskAtlas a;
        std::string error;
        ASSERT_TRUE (rs::PackStepMasks (samples, steps, bits, texels, width, height, a, error)) << error;
        EXPECT_EQ (a.width, e.width);
        EXPECT_EQ (a.height, e.height);
        EXPECT_EQ (a.words, e.words);
        EXPECT_EQ (a.steps, e.steps);
        EXPECT_EQ (a.masks, e.masks);
    }
}

TEST (SunStudyRustParity, TextureDeltasMatch)
{
    Lcg lcg (0xde17aull);
    for (int trial = 0; trial < 120; ++trial) {
        const uint32_t width = static_cast<uint32_t> (lcg.Int (1, 24)),
                       height = static_cast<uint32_t> (lcg.Int (1, 16)),
                       layers = static_cast<uint32_t> (lcg.Int (1, 3));
        const size_t count = static_cast<size_t> (width) * height * layers;
        std::vector<float> before (count), after;
        for (auto& texel : before)
            texel = lcg.Int (0, 4) == 0 ? -1.0f : static_cast<float> (lcg.Int (0, 12));
        after = before;
        const int edits = lcg.Int (0, 60);
        for (int i = 0; i < edits; ++i) {
            const size_t at = static_cast<size_t> (lcg.Uniform (0, static_cast<double> (count)));
            after[std::min (at, count - 1)] = lcg.Int (0, 3) == 0 ? -0.0f : static_cast<float> (lcg.Int (0, 12));
        }
        if (trial % 10 == 9)
            after.pop_back (); // inconsistent: full upload
        const size_t maxRegions = trial % 4 == 0 ? 3 : 512;
        const auto e = evp::sunstudy::AtlasChangedRegions (width, height, layers, before, after, maxRegions);
        std::vector<AtlasRegion> a;
        std::string error;
        ASSERT_TRUE (rs::AtlasChangedRegions (width, height, layers, before, after, maxRegions, a, error)) << error;
        ASSERT_EQ (a.size (), e.size ());
        for (size_t i = 0; i < e.size (); ++i) {
            EXPECT_EQ (a[i].x, e[i].x);
            EXPECT_EQ (a[i].y, e[i].y);
            EXPECT_EQ (a[i].width, e[i].width);
            EXPECT_EQ (a[i].height, e[i].height);
            EXPECT_EQ (a[i].layer, e[i].layer);
        }
    }
}

TEST (SunStudyRustParity, StepScheduleMatches)
{
    Lcg lcg (0x5c4edull);
    for (int trial = 0; trial < 60; ++trial) {
        std::vector<SunStep> steps;
        const int timestep = trial % 3 == 0 ? 15 : (trial % 3 == 1 ? 30 : 60);
        for (const TimeOfDay& time : EnumerateTimesteps (timestep)) {
            SunStep step;
            step.time = time;
            step.altitudeDegrees = 60.0 - std::abs (time.hour * 60 + time.minute - lcg.Uniform (600, 900)) / 8.0;
            steps.push_back (step);
        }
        const SunSeries series = SunSeries::FromSteps (steps, timestep);
        for (const double interval : { 60.0, 30.0, 0.0, 45.0, 120.0 }) {
            std::vector<uint16_t> minutes;
            std::vector<uint32_t> fan;
            uint32_t noon = 99;
            std::string error;
            ASSERT_TRUE (rs::StepSchedule (series, interval, minutes, fan, noon, error)) << error;
            EXPECT_EQ (minutes, evp::sunstudy::StepMinutes (series));
            EXPECT_EQ (fan, evp::sunstudy::FanSteps (evp::sunstudy::StepMinutes (series), interval));
            EXPECT_EQ (noon, evp::sunstudy::SolarNoonStep (series));
        }
    }
}

// ---------------------------------------------------------------------------
// P3a: the occlusion accumulator over the real CPU traversal
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<const geomsrv::Snapshot> BoxCity (Lcg& lcg)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 7;
    const int boxes = lcg.Int (1, 8);
    for (int b = 0; b < boxes; ++b) {
        SamplerCase soup;
        const double x = lcg.Uniform (-15, 15), y = lcg.Uniform (-15, 15);
        const double w = lcg.Uniform (1, 6), d = lcg.Uniform (1, 6), h = lcg.Uniform (2, 20);
        AppendRectangle (soup, { x, y, h }, { 1, 0, 0 }, { 0, 1, 0 }, w, d, 1, 0, false);     // roof
        AppendRectangle (soup, { x, y, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, w, h, 1, 0, true);      // south
        AppendRectangle (soup, { x, y + d, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, w, h, 1, 0, false); // north
        AppendRectangle (soup, { x, y, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, d, h, 1, 0, false);     // west
        AppendRectangle (soup, { x + w, y, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, d, h, 1, 0, true);  // east
        geomsrv::Mesh mesh;
        mesh.guid = "B" + std::to_string (b);
        mesh.vertices = soup.vertices;
        mesh.triangles = soup.triangles;
        snapshot->meshes.push_back (std::move (mesh));
    }
    return snapshot;
}

struct OcclusionScene {
    std::shared_ptr<CpuTraversal> traversal;
    std::vector<double> positions;
    std::vector<double> normals;
    std::vector<SunStep> steps;
    SunSeries series;
};

OcclusionScene MakeOcclusionScene (Lcg& lcg, size_t samples, size_t stepCount)
{
    OcclusionScene scene;
    scene.traversal = std::make_shared<CpuTraversal> (std::make_shared<geomsrv::QueryEngine> (BoxCity (lcg)));
    for (size_t s = 0; s < samples; ++s) {
        scene.positions.insert (scene.positions.end (),
                                { lcg.Uniform (-25, 25), lcg.Uniform (-25, 25), lcg.Uniform (0.05, 8) });
        Vec3 n { lcg.Uniform (-1, 1), lcg.Uniform (-1, 1), lcg.Uniform (-1, 1) };
        if (s % 17 == 3)
            n = { std::numeric_limits<double>::quiet_NaN (), 0, 0 }; // traced, not culled
        scene.normals.insert (scene.normals.end (), { n[0], n[1], n[2] });
    }
    for (size_t i = 0; i < stepCount; ++i) {
        SunStep step;
        step.time = TimeOfDay { static_cast<int> (i / 4), static_cast<int> ((i % 4) * 15) };
        const double azimuth = lcg.Uniform (0, 6.283), altitude = lcg.Uniform (0.05, 1.4);
        step.direction[0] = std::cos (altitude) * std::cos (azimuth);
        step.direction[1] = std::cos (altitude) * std::sin (azimuth);
        step.direction[2] = std::sin (altitude);
        step.altitudeDegrees = altitude * 57.29;
        scene.steps.push_back (step);
    }
    scene.series = SunSeries::FromSteps (scene.steps, 15);
    return scene;
}

void ExpectSameOcclusion (const OcclusionAccumulator& e, const rs::RustOcclusionAccumulator& a, const char* what)
{
    ASSERT_TRUE (a.LastError ().empty ()) << what << ": " << a.LastError ();
    EXPECT_EQ (a.ResolvedStepCount (), e.ResolvedStepCount ()) << what;
    EXPECT_EQ (a.Complete (), e.Complete ()) << what;
    EXPECT_EQ (a.ActiveSampleCount (), e.ActiveSampleCount ()) << what;
    EXPECT_EQ (a.LastRayCount (), e.LastRayCount ()) << what;
    EXPECT_EQ (a.Bits (), e.Bits ()) << what;
    const std::vector<uint8_t> flags = a.StepsResolved ();
    for (size_t step = 0; step < e.StepCount (); ++step)
        ASSERT_EQ (flags[step] != 0, e.StepResolved (step)) << what << " step " << step;
    ExpectSameBits (e.SunHours (0.25), a.SunHours (0.25), what);
}

} // namespace

TEST (SunStudyRustParity, OcclusionMatchesOverSlicedRanges)
{
    Lcg lcg (0x0cc1ull);
    size_t rays = 0;
    for (int trial = 0; trial < 24; ++trial) {
        const size_t steps = static_cast<size_t> (lcg.Int (1, 140));
        OcclusionScene scene = MakeOcclusionScene (lcg, static_cast<size_t> (lcg.Int (0, 400)), steps);
        SampleSet samples;
        samples.positions = scene.positions.data ();
        samples.normals = trial % 3 == 0 ? nullptr : scene.normals.data ();
        samples.count = scene.positions.size () / 3;
        OcclusionAccumulator expected (samples.count, scene.series.StepCount ());
        rs::RustOcclusionAccumulator actual (samples.count, scene.series.StepCount ());
        ASSERT_TRUE (actual.Valid ()) << actual.LastError ();
        const size_t parallel = trial % 2 == 0 ? 1 : 0;
        size_t cursor = 0;
        while (cursor < scene.series.StepCount ()) {
            const size_t slice = static_cast<size_t> (lcg.Int (1, 9));
            const size_t e =
                expected.AccumulateRange (*scene.traversal, samples, scene.series, cursor, slice, 0.001, 0.0, parallel);
            const size_t a =
                actual.AccumulateRange (*scene.traversal, samples, scene.series, cursor, slice, 0.001, 0.0, parallel);
            ASSERT_EQ (a, e);
            cursor += e;
            rays += expected.LastRayCount ();
            ExpectSameOcclusion (expected, actual, "slice");
            if (::testing::Test::HasFatalFailure ())
                return;
        }
        // Re-resolving an early step with a different sun replaces its bits.
        if (scene.series.StepCount () > 1) {
            const double* other = scene.series.Step (scene.series.StepCount () - 1).direction;
            EXPECT_EQ (actual.AccumulateStep (*scene.traversal, samples, 0, other),
                       expected.AccumulateStep (*scene.traversal, samples, 0, other));
            ExpectSameOcclusion (expected, actual, "re-resolve");
        }
        // Out-of-range and mismatched calls change nothing on either side.
        EXPECT_FALSE (actual.AccumulateStep (*scene.traversal, samples, steps + 5, scene.steps[0].direction));
        SampleSet wrong = samples;
        wrong.count = samples.count + 1;
        EXPECT_EQ (actual.AccumulateStep (*scene.traversal, wrong, 0, scene.steps[0].direction),
                   expected.AccumulateStep (*scene.traversal, wrong, 0, scene.steps[0].direction));
        ExpectSameOcclusion (expected, actual, "refusals");
    }
    EXPECT_GT (rays, 10000u);
}

TEST (SunStudyRustParity, OcclusionCancellationCommitsNothingOnEitherSide)
{
    Lcg lcg (0xca9cull);
    OcclusionScene scene = MakeOcclusionScene (lcg, 300, 20);
    SampleSet samples { scene.positions.data (), scene.normals.data (), scene.positions.size () / 3 };
    OcclusionAccumulator expected (samples.count, scene.series.StepCount ());
    rs::RustOcclusionAccumulator actual (samples.count, scene.series.StepCount ());
    bool cancel = false;
    const std::function<bool ()> isCancelled = [&cancel] { return cancel; };
    for (size_t step = 0; step < scene.series.StepCount (); ++step) {
        cancel = step % 3 == 1; // cancelled before anything is traced
        const double* sun = scene.series.Step (step).direction;
        EXPECT_EQ (actual.AccumulateStep (*scene.traversal, samples, step, sun, 0.001, 0.0, 0, isCancelled),
                   expected.AccumulateStep (*scene.traversal, samples, step, sun, 0.001, 0.0, 0, isCancelled));
    }
    ExpectSameOcclusion (expected, actual, "cancellation");
    EXPECT_FALSE (expected.Complete ());
}

TEST (SunStudyRustParity, OcclusionSeededReuseMatches)
{
    Lcg lcg (0x5eedull);
    for (int trial = 0; trial < 30; ++trial) {
        const size_t steps = static_cast<size_t> (lcg.Int (1, 130));
        OcclusionScene scene = MakeOcclusionScene (lcg, static_cast<size_t> (lcg.Int (1, 200)), steps);
        SampleSet samples { scene.positions.data (), scene.normals.data (), scene.positions.size () / 3 };
        const size_t count = samples.count, stepCount = scene.series.StepCount ();
        OcclusionAccumulator expectedSource (count, stepCount);
        rs::RustOcclusionAccumulator actualSource (count, stepCount);
        expectedSource.AccumulateRange (*scene.traversal, samples, scene.series, 0, stepCount);
        actualSource.AccumulateRange (*scene.traversal, samples, scene.series, 0, stepCount);
        std::vector<size_t> map (count);
        for (size_t s = 0; s < count; ++s)
            map[s] = lcg.Int (0, 6) == 0 ? OcclusionAccumulator::kNoReuse
                                         : static_cast<size_t> (lcg.Int (0, int (count) - 1));
        if (trial % 7 == 3 && count > 0)
            map[0] = count + 4; // out of range: refused
        const size_t words = (stepCount + 63) / 64;
        std::vector<uint64_t> dirty;
        if (trial % 2 == 1) {
            dirty.assign (count * words, 0);
            for (size_t s = 0; s < count; ++s) {
                for (size_t w = 0; w < words; ++w) {
                    const size_t remaining = stepCount - w * 64;
                    const uint64_t valid = remaining >= 64 ? ~0ull : (1ull << remaining) - 1ull;
                    uint64_t bits =
                        map[s] == OcclusionAccumulator::kNoReuse ? valid : (lcg.Int (0, 3) == 0 ? valid : 0ull);
                    if (map[s] != OcclusionAccumulator::kNoReuse && lcg.Int (0, 4) == 0)
                        bits = (static_cast<uint64_t> (lcg.Uniform (0, 4e9)) << 20) & valid;
                    dirty[s * words + w] = bits;
                }
            }
            if (trial % 11 == 5 && !dirty.empty ())
                dirty[0] |= ~0ull; // beyond the steps when stepCount % 64 != 0: refused
        }
        OcclusionAccumulator expected (count, stepCount);
        rs::RustOcclusionAccumulator actual (count, stepCount);
        ASSERT_EQ (actual.SeedReusable (actualSource, map, dirty), expected.SeedReusable (expectedSource, map, dirty))
            << "trial " << trial;
        ExpectSameOcclusion (expected, actual, "seeded");
        expected.AccumulateRange (*scene.traversal, samples, scene.series, 0, stepCount);
        actual.AccumulateRange (*scene.traversal, samples, scene.series, 0, stepCount);
        ExpectSameOcclusion (expected, actual, "after seeded trace");
        if (::testing::Test::HasFatalFailure ())
            return;
    }
}

// ---------------------------------------------------------------------------
// P3b: the study session
// ---------------------------------------------------------------------------

namespace {

void ExpectSameSession (const SunStudySession& e, const rs::RustSunStudySession& a, const char* what)
{
    ASSERT_TRUE (a.LastError ().empty ()) << what << ": " << a.LastError ();
    const StudyProgress x = e.Progress (), y = a.Progress ();
    EXPECT_EQ (y.generation, x.generation) << what;
    EXPECT_EQ (y.resolvedSteps, x.resolvedSteps) << what;
    EXPECT_EQ (y.totalSteps, x.totalSteps) << what;
    EXPECT_EQ (y.sampleCount, x.sampleCount) << what;
    EXPECT_EQ (y.converged, x.converged) << what;
    EXPECT_EQ (y.empty, x.empty) << what;
    EXPECT_EQ (a.Bits (), e.Accumulator ().Bits ()) << what;
    ExpectSameBits (e.SunHours (), a.SunHours (), what);
}

} // namespace

TEST (SunStudyRustParity, SessionMatchesAcrossSyncsSlicesAndSeeds)
{
    Lcg lcg (0x5e55ull);
    for (int trial = 0; trial < 16; ++trial) {
        OcclusionScene scene = MakeOcclusionScene (lcg, 240, static_cast<size_t> (lcg.Int (2, 90)));
        const SunSeries otherDay = SunSeries::FromSteps (
            std::vector<SunStep> (scene.steps.begin (), scene.steps.begin () + scene.steps.size () / 2), 30);
        SunStudySession expected;
        rs::RustSunStudySession actual;
        ASSERT_TRUE (actual.Valid ()) << actual.LastError ();
        StudyInputs inputs { 1, 1, 1 };
        size_t count = 200;
        const SunSeries* day = &scene.series;
        for (int round = 0; round < 12; ++round) {
            switch (lcg.Int (0, 6)) { // occasionally invalidate something
                case 0:
                    ++inputs.geometryVersion;
                    break;
                case 1:
                    ++inputs.sunVersion;
                    day = day == &scene.series ? &otherDay : &scene.series;
                    break;
                case 2:
                    ++inputs.gridVersion;
                    break;
                case 3:
                    count = static_cast<size_t> (lcg.Int (0, 240)); // count change without a version bump
                    break;
                default:
                    break; // unchanged inputs: the cheap path
            }
            SampleSet samples { scene.positions.data (), trial % 2 == 0 ? scene.normals.data () : nullptr, count };
            expected.Sync (inputs, *day, samples);
            actual.Sync (inputs, *day, samples);
            const size_t budget = static_cast<size_t> (lcg.Int (0, 12));
            EXPECT_EQ (actual.Advance (*scene.traversal, budget), expected.Advance (*scene.traversal, budget));
            ExpectSameSession (expected, actual, "round");
            if (::testing::Test::HasFatalFailure ())
                return;
        }
        // Seed a fresh study from the converged one (both sides).
        expected.Advance (*scene.traversal, 1000);
        actual.Advance (*scene.traversal, 1000);
        SunStudySession expectedSeeded;
        rs::RustSunStudySession actualSeeded;
        SampleSet samples { scene.positions.data (), nullptr, count };
        StudyInputs fresh = inputs;
        ++fresh.geometryVersion;
        expectedSeeded.Sync (fresh, *day, samples);
        actualSeeded.Sync (fresh, *day, samples);
        std::vector<size_t> map (count);
        for (size_t s = 0; s < count; ++s)
            map[s] = lcg.Int (0, 4) == 0 ? OcclusionAccumulator::kNoReuse : s;
        EXPECT_EQ (actualSeeded.SeedReusable (actual, map), expectedSeeded.SeedReusable (expected.Accumulator (), map));
        EXPECT_FALSE (actualSeeded.SeedReusable (actualSeeded, map)) << "a session cannot seed from itself";
        EXPECT_EQ (actualSeeded.Advance (*scene.traversal, 1000), expectedSeeded.Advance (*scene.traversal, 1000));
        ExpectSameSession (expectedSeeded, actualSeeded, "seeded");
    }
}

TEST (SunStudyRustParity, SessionObserverReportsTheSameSteps)
{
    Lcg lcg (0x0b5eull);
    OcclusionScene scene = MakeOcclusionScene (lcg, 150, 12);
    SampleSet samples { scene.positions.data (), scene.normals.data (), 150 };
    SunStudySession expected;
    rs::RustSunStudySession actual;
    std::vector<std::pair<size_t, size_t>> seenExpected, seenActual;
    expected.SetStepObserver ([&] (size_t step, const OcclusionAccumulator& accumulator, double) {
        seenExpected.emplace_back (step, accumulator.LastRayCount ());
    });
    actual.SetStepObserver ([&] (size_t step, const TapiocaOcclusionState& state, double) {
        seenActual.emplace_back (step, state.lastRayCount);
    });
    expected.Sync ({ 1, 1, 1 }, scene.series, samples);
    actual.Sync ({ 1, 1, 1 }, scene.series, samples);
    expected.Advance (*scene.traversal, 7);
    actual.Advance (*scene.traversal, 7);
    EXPECT_EQ (seenActual, seenExpected);
    EXPECT_EQ (seenActual.size (), 7u);
}

namespace {

// A traversal that is slow enough for two advances to overlap.
class SlowTraversal final : public ITraversal {
  public:
    explicit SlowTraversal (const ITraversal& inner) : inner_ (inner)
    {
    }
    void OccludeDirectional (const double* origins, size_t count, const double dir[3], double tmin, double tmax,
                             uint8_t* out, size_t maxParallel) const override
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (15));
        inner_.OccludeDirectional (origins, count, dir, tmin, tmax, out, maxParallel);
    }
    void OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out, size_t maxParallel) const override
    {
        inner_.OccludeRays (rays, count, out, maxParallel);
    }
    uint64_t SceneVersion () const override
    {
        return inner_.SceneVersion ();
    }

  private:
    const ITraversal& inner_;
};

} // namespace

TEST (SunStudyRustParity, ConcurrentSessionAdvancesThroughTheAbiNeverDoubleClaim)
{
    Lcg lcg (0xc0c0ull);
    OcclusionScene scene = MakeOcclusionScene (lcg, 100, 10);
    SampleSet samples { scene.positions.data (), nullptr, 100 };
    rs::RustSunStudySession session;
    session.Sync ({ 1, 1, 1 }, scene.series, samples);
    const SlowTraversal slow (*scene.traversal);
    std::atomic<size_t> first { 0 }, second { 0 };
    std::thread a ([&] { first = session.Advance (slow, 4); });
    std::this_thread::sleep_for (std::chrono::milliseconds (5));
    std::thread b ([&] { second = session.Advance (slow, 4); });
    a.join ();
    b.join ();
    EXPECT_EQ (first + second, 4u) << "the loser is refused, not queued";
    EXPECT_EQ (session.Progress ().resolvedSteps, 4u);
    EXPECT_FALSE (session.Advancing ());
    // The result equals an uncontended run of the same steps.
    SunStudySession reference;
    reference.Sync ({ 1, 1, 1 }, scene.series, samples);
    reference.Advance (*scene.traversal, 4);
    EXPECT_EQ (session.Bits (), reference.Accumulator ().Bits ());
}

// ---------------------------------------------------------------------------
// P3c: reuse planning, patch sample maps, result summary
// ---------------------------------------------------------------------------

namespace {

geomsrv::Mesh ReuseBox (const std::string& guid, double x, double y, double w, double d, double h)
{
    SamplerCase soup;
    AppendRectangle (soup, { x, y, h }, { 1, 0, 0 }, { 0, 1, 0 }, w, d, 2, 0, false);
    AppendRectangle (soup, { x, y, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, w, h, 1, 0, true);
    AppendRectangle (soup, { x, y + d, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, w, h, 1, 0, false);
    AppendRectangle (soup, { x, y, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, d, h, 1, 0, false);
    AppendRectangle (soup, { x + w, y, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, d, h, 1, 0, true);
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = soup.vertices;
    mesh.triangles = soup.triangles;
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
    return mesh;
}

geomsrv::Mesh ReuseFloor (const std::string& guid, double x, double w)
{
    SamplerCase soup;
    AppendRectangle (soup, { x, -20, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, w, 40, 3, 0, false);
    geomsrv::Mesh mesh;
    mesh.guid = guid;
    mesh.vertices = soup.vertices;
    mesh.triangles = soup.triangles;
    mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
    return mesh;
}

SunSeries ReuseDay (size_t count)
{
    std::vector<SunStep> steps;
    for (size_t i = 0; i < count; ++i) {
        SunStep step;
        step.time = { 7 + static_cast<int> (i / 4), static_cast<int> ((i % 4) * 15) };
        const double azimuth = 0.3 + 0.21 * static_cast<double> (i),
                     altitude = 0.25 + 0.05 * static_cast<double> (i % 9);
        step.direction[0] = std::cos (altitude) * std::cos (azimuth);
        step.direction[1] = std::cos (altitude) * std::sin (azimuth);
        step.direction[2] = std::sin (altitude);
        step.altitudeDegrees = altitude * 57.29;
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, 15);
}

std::unique_ptr<StudyRecord> ReuseRecord (std::shared_ptr<const geomsrv::Snapshot> snapshot, bool patch, size_t steps,
                                          const std::vector<std::string>& context)
{
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->series = ReuseDay (steps);
    record->gridSpacing = 1.5;
    const auto roles = evp::sunstudy::ResolveElementRoles (*snapshot, {}, context, {});
    record->traversal = std::make_shared<CpuTraversal> (std::make_shared<geomsrv::QueryEngine> (snapshot));
    for (const auto role : roles.roles)
        record->elementRoles.push_back (static_cast<uint8_t> (role));
    const auto sampleMask = roles.SampleMask ();
    std::vector<double> vertices;
    std::vector<uint32_t> triangles, groups;
    std::vector<std::string> elements;
    for (size_t m = 0; m < snapshot->meshes.size (); ++m) {
        const auto& mesh = snapshot->meshes[m];
        const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
        for (const uint32_t index : mesh.triangles)
            triangles.push_back (base + index);
        groups.resize (triangles.size () / 3, static_cast<uint32_t> (m));
        elements.push_back (mesh.guid);
    }
    if (patch) {
        record->domain = SamplingDomain::SurfacePatch;
        PatchSamplerOptions options;
        options.spacing = record->gridSpacing;
        options.sampleGroup = &sampleMask;
        record->patchGrid =
            evp::sunstudy::BuildPatchSampleGrid (vertices.data (), vertices.size () / 3, triangles.data (),
                                                 triangles.size () / 3, groups.data (), elements, options);
        record->positions = record->patchGrid.positions;
        record->normals = record->patchGrid.normals;
    }
    else {
        SamplerOptions options;
        options.spacing = record->gridSpacing;
        options.sampleGroup = &sampleMask;
        record->sampleGrid = evp::sunstudy::BuildSampleGrid (vertices.data (), vertices.size () / 3, triangles.data (),
                                                             triangles.size () / 3, groups.data (), options);
        record->positions = record->sampleGrid.positions;
        record->normals = record->sampleGrid.normals;
    }
    evp::sunstudy::SetSampleMeshes (*record);
    record->session.Sync ({ snapshot->id, record->series.Version (), 1 }, record->series, record->Samples ());
    return record;
}

std::shared_ptr<geomsrv::Snapshot> ReuseScene (Lcg& lcg, int edit, const geomsrv::Snapshot* base)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    if (base == nullptr) {
        snapshot->id = 1;
        snapshot->meshes = { ReuseFloor ("near", -10, 30), ReuseBox ("tower", 2, -2, 4, 4, 9),
                             ReuseBox ("shed", 12, 6, 3, 2, 3), ReuseFloor ("far", 80, 20) };
        return snapshot;
    }
    *snapshot = *base;
    snapshot->id = base->id + 1;
    switch (edit) {
        case 0: // move the tower
            snapshot->meshes[1] = ReuseBox ("tower", 2 + lcg.Uniform (0.5, 3), -2, 4, 4, 9);
            break;
        case 1: // add a box
            snapshot->meshes.push_back (ReuseBox ("new", lcg.Uniform (-5, 15), lcg.Uniform (-10, 10), 2, 2, 5));
            break;
        case 2: // delete the shed
            snapshot->meshes.erase (snapshot->meshes.begin () + 2);
            break;
        case 3: // reorder without changing geometry
            std::swap (snapshot->meshes[0], snapshot->meshes[3]);
            break;
        default: // unchanged geometry (role change happens on the record)
            break;
    }
    return snapshot;
}

void ExpectSameReuse (StudyRecord& expected, StudyRecord& actual, size_t e, size_t a, const char* what)
{
    EXPECT_EQ (a, e) << what;
    EXPECT_EQ (actual.reusedSamples, expected.reusedSamples) << what;
    EXPECT_EQ (actual.reusedSampleSteps, expected.reusedSampleSteps) << what;
    EXPECT_EQ (actual.session.Accumulator ().ActiveSampleCount (), expected.session.Accumulator ().ActiveSampleCount ())
        << what;
    EXPECT_EQ (actual.session.Accumulator ().Bits (), expected.session.Accumulator ().Bits ()) << what;
    expected.session.Advance (*expected.traversal, 1000, 0.001, 0.0, 1);
    actual.session.Advance (*actual.traversal, 1000, 0.001, 0.0, 1);
    EXPECT_EQ (actual.session.Accumulator ().Bits (), expected.session.Accumulator ().Bits ()) << what << " traced";
}

} // namespace

TEST (SunStudyRustParity, ReusePlanningMatchesAcrossEditsDomainsAndRoles)
{
    Lcg lcg (0x4e05eull);
    size_t reusedTotal = 0, cases = 0;
    for (const bool patch : { false, true }) {
        for (int edit = 0; edit < 6; ++edit) {
            for (const size_t steps : { size_t { 5 }, size_t { 70 } }) {
                const auto base = ReuseScene (lcg, -1, nullptr);
                auto source = ReuseRecord (base, patch, steps, {});
                source->session.Advance (*source->traversal, 1000, 0.001, 0.0, 1);
                ASSERT_TRUE (source->session.Progress ().converged);
                const auto edited = ReuseScene (lcg, edit, base.get ());
                const std::vector<std::string> context =
                    edit == 5 ? std::vector<std::string> { "shed" } : std::vector<std::string> {};
                auto expected = ReuseRecord (edited, patch, steps, context);
                auto actual = ReuseRecord (edited, patch, steps, context);
                const std::atomic<bool> notCancelled { false };
                const size_t e = evp::sunstudy::ReuseUnaffectedSamples (*source, *expected, notCancelled);
                std::string error;
                const size_t a = rs::ReuseUnaffectedSamples (*source, *actual, notCancelled, error);
                ASSERT_TRUE (error.empty ()) << error;
                ExpectSameReuse (*expected, *actual, e, a, patch ? "patch" : "triangle");
                if (::testing::Test::HasFatalFailure ())
                    return;
                reusedTotal += e;
                ++cases;
            }
        }
    }
    EXPECT_EQ (cases, 24u);
    EXPECT_GT (reusedTotal, 100u) << "the corpus must actually reuse something";
}

TEST (SunStudyRustParity, ReuseRefusalsMatch)
{
    Lcg lcg (0x4ef5ull);
    const auto base = ReuseScene (lcg, -1, nullptr);
    auto source = ReuseRecord (base, false, 6, {});
    auto unfinished = ReuseRecord (base, false, 6, {});
    source->session.Advance (*source->traversal, 1000);
    const std::atomic<bool> cancelled { true }, running { false };
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto expected = ReuseRecord (base, false, scenario == 2 ? 7 : 6, {});
        auto actual = ReuseRecord (base, false, scenario == 2 ? 7 : 6, {});
        const StudyRecord& from = scenario == 0 ? *unfinished : *source;
        const std::atomic<bool>& stop = scenario == 1 ? cancelled : running;
        if (scenario == 3) { // ambiguous identity
            auto ambiguous = std::make_shared<geomsrv::Snapshot> (*base);
            ambiguous->meshes[3].guid = "{NEAR}";
            expected = ReuseRecord (ambiguous, false, 6, {});
            actual = ReuseRecord (ambiguous, false, 6, {});
        }
        std::string error;
        EXPECT_EQ (rs::ReuseUnaffectedSamples (from, *actual, stop, error),
                   evp::sunstudy::ReuseUnaffectedSamples (from, *expected, stop))
            << "scenario " << scenario;
        EXPECT_EQ (actual->session.Accumulator ().Bits (), expected->session.Accumulator ().Bits ());
    }
}

TEST (SunStudyRustParity, PatchSampleMeshesMatch)
{
    Lcg lcg (0x5a3eull);
    for (int trial = 0; trial < 6; ++trial) {
        const auto base = ReuseScene (lcg, -1, nullptr);
        auto snapshot = std::make_shared<geomsrv::Snapshot> (*base);
        auto expected = ReuseRecord (snapshot, true, 3, {});
        StudyRecord actual;
        actual.snapshot = expected->snapshot;
        actual.domain = expected->domain;
        actual.patchGrid = expected->patchGrid;
        actual.positions = expected->positions;
        if (trial == 1)
            actual.patchGrid.spans[0].key.element = "gone"; // bad span: cleared
        if (trial == 2) {                                   // ambiguous meshes: untouched
            auto dup = std::make_shared<geomsrv::Snapshot> (*snapshot);
            dup->meshes[1].guid = "NEAR";
            actual.snapshot = dup;
        }
        StudyRecord oracle;
        oracle.snapshot = actual.snapshot;
        oracle.domain = actual.domain;
        oracle.patchGrid = actual.patchGrid;
        oracle.positions = actual.positions;
        actual.sampleMeshes = { 7, 7 };
        oracle.sampleMeshes = { 7, 7 };
        evp::sunstudy::SetSampleMeshes (oracle);
        std::string error;
        ASSERT_TRUE (rs::SetSampleMeshes (actual, error)) << error;
        EXPECT_EQ (actual.sampleMeshes, oracle.sampleMeshes) << "trial " << trial;
    }
}

TEST (SunStudyRustParity, ResultSummaryMatchesEveryBit)
{
    Lcg lcg (0x50aaull);
    for (int trial = 0; trial < 50; ++trial) {
        std::vector<double> hours (static_cast<size_t> (lcg.Int (0, 300)));
        const double daylight = lcg.Uniform (0, 16);
        for (auto& h : hours)
            h = lcg.Int (0, 5) == 0 ? daylight : (lcg.Int (0, 5) == 0 ? 0.0 : lcg.Uniform (0, daylight));
        const SunStudyResultSummary e = evp::sunstudy::SummarizeSunHours (hours, daylight);
        SunStudyResultSummary a;
        std::string error;
        ASSERT_TRUE (rs::SummarizeSunHours (hours, daylight, a, error)) << error;
        EXPECT_EQ (a.count, e.count);
        EXPECT_EQ (a.fullyLit, e.fullyLit);
        EXPECT_EQ (a.fullyShaded, e.fullyShaded);
        EXPECT_TRUE (SameBits (e.minHours, a.minHours));
        EXPECT_TRUE (SameBits (e.meanHours, a.meanHours));
        EXPECT_TRUE (SameBits (e.maxHours, a.maxHours));
        EXPECT_TRUE (SameBits (e.daylightHours, a.daylightHours));
    }
}

namespace {

void ExpectSameSurface (const SurfaceSamplingResult& e, const SurfaceSamplingResult& a, const std::string& what)
{
    SCOPED_TRACE (what);
    ASSERT_EQ (a.valid, e.valid);
    EXPECT_EQ (a.reusedMeshes, e.reusedMeshes);
    EXPECT_EQ (a.rebuiltMeshes, e.rebuiltMeshes);
    EXPECT_EQ (a.reusedSamples, e.reusedSamples);
    EXPECT_EQ (a.generatedSamples, e.generatedSamples);
    EXPECT_EQ (a.winding.groups, e.winding.groups);
    EXPECT_EQ (a.winding.closed, e.winding.closed);
    EXPECT_EQ (a.winding.flipped, e.winding.flipped);
    EXPECT_EQ (a.winding.flippedTriangles, e.winding.flippedTriangles);
    const SurfaceSamplingLayout &x = e.layout, &y = a.layout;
    ASSERT_EQ (y.valid, x.valid);
    EXPECT_EQ (y.options.domain, x.options.domain);
    EXPECT_TRUE (SameBits (y.options.spacing, x.options.spacing));
    EXPECT_TRUE (SameBits (y.options.normalOffset, x.options.normalOffset));
    EXPECT_TRUE (SameBits (y.options.jitter, x.options.jitter));
    EXPECT_EQ (y.options.maxSamples, x.options.maxSamples);
    ASSERT_EQ (y.meshes.size (), x.meshes.size ());
    for (size_t m = 0; m < x.meshes.size (); ++m) {
        const SurfaceMeshSampling &p = x.meshes[m], &q = y.meshes[m];
        EXPECT_EQ (q.firstFace, p.firstFace) << "mesh " << m;
        EXPECT_EQ (q.faceCount, p.faceCount) << "mesh " << m;
        EXPECT_EQ (q.firstSample, p.firstSample) << "mesh " << m;
        EXPECT_EQ (q.sampleCount, p.sampleCount) << "mesh " << m;
        EXPECT_EQ (q.firstPatch, p.firstPatch) << "mesh " << m;
        EXPECT_EQ (q.patchCount, p.patchCount) << "mesh " << m;
        EXPECT_EQ (q.degenerate, p.degenerate) << "mesh " << m;
        EXPECT_EQ (q.undersized, p.undersized) << "mesh " << m;
        EXPECT_EQ (q.excluded, p.excluded) << "mesh " << m;
        EXPECT_EQ (q.sampled, p.sampled) << "mesh " << m;
        EXPECT_EQ (q.sampleFaces, p.sampleFaces) << "mesh " << m;
        EXPECT_EQ (q.winding.groups, p.winding.groups) << "mesh " << m;
        EXPECT_EQ (q.winding.closed, p.winding.closed) << "mesh " << m;
        EXPECT_EQ (q.winding.flipped, p.winding.flipped) << "mesh " << m;
        EXPECT_EQ (q.winding.flippedTriangles, p.winding.flippedTriangles) << "mesh " << m;
    }
    if (!e.valid)
        return; // a refused build publishes no grid on either side
    ExpectSameSampleGrids (e.triangles, a.triangles);
    ExpectSamePatchGrids (e.patches, a.patches);
}

// The fields BuildSurfaceSampling reads from an earlier study.
std::unique_ptr<StudyRecord> SampledRecord (std::shared_ptr<const geomsrv::Snapshot> snapshot,
                                            const SurfaceSamplingResult& result, SamplingDomain domain)
{
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = std::move (snapshot);
    record->domain = domain;
    record->samplingLayout = result.layout;
    record->sampleGrid = result.triangles;
    record->patchGrid = result.patches;
    return record;
}

struct SurfaceCase {
    std::vector<uint8_t> sampleMask;
    SurfaceSamplingOptions options;
    std::vector<std::vector<uint8_t>> faceMasks;
};

void ExpectSurfaceParity (const geomsrv::Snapshot& snapshot, const SurfaceCase& c, const StudyRecord* previous,
                          const std::function<bool ()>& isCancelled, const std::string& what)
{
    const auto expected =
        BuildSurfaceSampling (snapshot, c.sampleMask, c.options, previous, isCancelled, c.faceMasks);
    SurfaceSamplingResult actual;
    std::string error;
    ASSERT_TRUE (
        rs::BuildSurfaceSampling (snapshot, c.sampleMask, c.options, previous, isCancelled, c.faceMasks, actual, error))
        << error;
    ExpectSameSurface (expected, actual, what);
}

SurfaceCase CaseFor (const geomsrv::Snapshot& snapshot, SamplingDomain domain, double jitter)
{
    SurfaceCase c;
    c.sampleMask.assign (snapshot.meshes.size (), 1);
    c.options.domain = domain;
    c.options.spacing = 1.5;
    c.options.jitter = jitter;
    return c;
}

} // namespace

TEST (SunStudyRustParity, SurfaceSamplingMatchesColdAndReusedAcrossEdits)
{
    Lcg lcg (0x5a3d17);
    for (const SamplingDomain domain : { SamplingDomain::TriangleLegacy, SamplingDomain::SurfacePatch }) {
        for (const double jitter : { 0.0, 0.35 }) {
            if (domain == SamplingDomain::SurfacePatch && jitter != 0.0)
                continue; // the patch sampler has no jitter
            const std::shared_ptr<const geomsrv::Snapshot> base = ReuseScene (lcg, -1, nullptr);
            const SurfaceCase cold = CaseFor (*base, domain, jitter);
            const std::string label = std::string (domain == SamplingDomain::SurfacePatch ? "patch" : "triangle") +
                                      " jitter " + std::to_string (jitter);
            ExpectSurfaceParity (*base, cold, nullptr, {}, label + " cold");
            const auto first = BuildSurfaceSampling (*base, cold.sampleMask, cold.options);
            ASSERT_TRUE (first.valid);
            const auto previous = SampledRecord (base, first, domain);
            for (int edit = 0; edit <= 4; ++edit) {
                const std::shared_ptr<const geomsrv::Snapshot> edited = ReuseScene (lcg, edit, base.get ());
                const SurfaceCase c = CaseFor (*edited, domain, jitter);
                ExpectSurfaceParity (*edited, c, previous.get (), {}, label + " edit " + std::to_string (edit));
                if (edit == 4) // unchanged geometry must take the reuse path, or it tests nothing
                    EXPECT_EQ (BuildSurfaceSampling (*edited, c.sampleMask, c.options, previous.get ()).reusedMeshes,
                               edited->meshes.size ());
            }
            SurfaceCase finer = cold;
            finer.options.spacing = 1.25;
            ExpectSurfaceParity (*base, finer, previous.get (), {}, label + " settings change");
            SurfaceCase context = cold;
            context.sampleMask[2] = 0;
            ExpectSurfaceParity (*base, context, previous.get (), {}, label + " context mesh");
            SurfaceCase masked = cold;
            masked.faceMasks.resize (base->meshes.size ());
            masked.faceMasks[1].assign (base->meshes[1].triangles.size () / 3, 1);
            for (size_t f = 0; f < masked.faceMasks[1].size (); f += 3)
                masked.faceMasks[1][f] = 0;
            ExpectSurfaceParity (*base, masked, nullptr, {}, label + " face mask cold");
            ExpectSurfaceParity (*base, masked, previous.get (), {}, label + " face mask reused");
            const auto maskedFirst =
                BuildSurfaceSampling (*base, masked.sampleMask, masked.options, nullptr, {}, masked.faceMasks);
            const auto maskedPrevious = SampledRecord (base, maskedFirst, domain);
            ExpectSurfaceParity (*base, masked, maskedPrevious.get (), {}, label + " face mask unchanged");
            ExpectSurfaceParity (*base, cold, maskedPrevious.get (), {}, label + " face mask removed");
        }
    }
}

TEST (SunStudyRustParity, SurfaceSamplingRefusalsAndFallbacksMatch)
{
    Lcg lcg (0x77e1);
    for (const SamplingDomain domain : { SamplingDomain::TriangleLegacy, SamplingDomain::SurfacePatch }) {
        const std::shared_ptr<const geomsrv::Snapshot> base = ReuseScene (lcg, -1, nullptr);
        const SurfaceCase cold = CaseFor (*base, domain, 0.0);
        const auto first = BuildSurfaceSampling (*base, cold.sampleMask, cold.options);
        const auto previous = SampledRecord (base, first, domain);

        auto ambiguous = std::make_shared<geomsrv::Snapshot> (*base);
        ambiguous->meshes[3].guid = ambiguous->meshes[0].guid;
        ExpectSurfaceParity (*ambiguous, cold, nullptr, {}, "ambiguous cold");
        ExpectSurfaceParity (*ambiguous, cold, previous.get (), {}, "ambiguous with previous");
        const auto ambiguousFirst = BuildSurfaceSampling (*ambiguous, cold.sampleMask, cold.options);
        const auto ambiguousPrevious = SampledRecord (ambiguous, ambiguousFirst, domain);
        ExpectSurfaceParity (*base, cold, ambiguousPrevious.get (), {}, "ambiguous previous");

        SurfaceCase capped = cold;
        capped.options.maxSamples = 20;
        ExpectSurfaceParity (*base, capped, nullptr, {}, "sample cap");
        SurfaceCase shortMask = cold;
        shortMask.sampleMask.pop_back ();
        ExpectSurfaceParity (*base, shortMask, nullptr, {}, "short sample mask");
        SurfaceCase badSpacing = cold;
        badSpacing.options.spacing = std::numeric_limits<double>::quiet_NaN ();
        ExpectSurfaceParity (*base, badSpacing, nullptr, {}, "NaN spacing");
        SurfaceCase shortFaces = cold;
        shortFaces.faceMasks.resize (base->meshes.size () - 1);
        ExpectSurfaceParity (*base, shortFaces, nullptr, {}, "short face masks");

        auto otherDomain = SampledRecord (base, first,
                                          domain == SamplingDomain::SurfacePatch ? SamplingDomain::TriangleLegacy
                                                                                 : SamplingDomain::SurfacePatch);
        ExpectSurfaceParity (*base, cold, otherDomain.get (), {}, "domain mismatch");
        StudyRecord noSnapshot;
        ExpectSurfaceParity (*base, cold, &noSnapshot, {}, "previous without snapshot");

        ExpectSurfaceParity (*base, cold, nullptr, [] { return true; }, "cancelled cold");
        ExpectSurfaceParity (*base, cold, previous.get (), [] { return true; }, "cancelled reuse");
        for (int after = 1; after <= 6; ++after) {
            const std::array<const StudyRecord*, 2> sources { nullptr, previous.get () };
            for (const StudyRecord* source : sources) {
                // Each side gets its own counter: the polls must fall at the
                // same points for the outcomes to agree.
                int expectedPolls = 0, actualPolls = 0;
                const auto expected = BuildSurfaceSampling (*base, cold.sampleMask, cold.options, source,
                                                            [&] { return ++expectedPolls > after; });
                SurfaceSamplingResult actual;
                std::string error;
                ASSERT_TRUE (rs::BuildSurfaceSampling (*base, cold.sampleMask, cold.options, source,
                                                       [&] { return ++actualPolls > after; }, {}, actual, error))
                    << error;
                ExpectSameSurface (expected, actual, "cancel after " + std::to_string (after));
                EXPECT_EQ (actualPolls, expectedPolls) << "cancel after " << after;
            }
        }
        // A throwing poll is a cancellation in Rust, never an escaped exception.
        SurfaceSamplingResult thrown;
        std::string error;
        ASSERT_TRUE (rs::BuildSurfaceSampling (
            *base, cold.sampleMask, cold.options, nullptr, [] () -> bool { throw std::runtime_error ("poll"); }, {},
            thrown, error))
            << error;
        EXPECT_FALSE (thrown.valid);
    }
}
