// Differential tests: the Rust sun-study core against the C++ implementation it
// replaces. The C++ side is the oracle; every case runs both on the same input
// and compares BIT FOR BIT, not within a tolerance -- the migration's exit
// criterion is every-bit equality, and a tolerance would hide a reordered sum.
//
// Fixtures are generated from a fixed-seed LCG so the corpus is reproducible
// and large enough to reach the branches (pole flip, zero sun, refusals) that a
// handful of literals would miss.

#include "SunStudy/SunStudyRust.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
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
    EXPECT_EQ (version, 1u);
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
