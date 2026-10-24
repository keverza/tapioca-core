// Differential tests: the Rust sun-study core against the C++ implementation it
// replaces. The C++ side is the oracle; every case runs both on the same input
// and compares BIT FOR BIT, not within a tolerance -- the migration's exit
// criterion is every-bit equality, and a tolerance would hide a reordered sum.
//
// Fixtures are generated from a fixed-seed LCG so the corpus is reproducible
// and large enough to reach the branches (pole flip, zero sun, refusals) that a
// handful of literals would miss.

#include "SunStudy/SunStudyAtlasReuse.hpp"
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
