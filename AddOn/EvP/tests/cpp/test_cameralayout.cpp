// ArchViz/Dxgi/CameraLayout -- what Archicad's `b2` holds, and the pair every
// CPU scorer reads.
//
// WHAT THESE TESTS ARE FOR. Archicad's model draws carry `b2 = view x projection`;
// only the 3D editing plane's own draws carry `b2 = projection`, and only while its
// display is on. The census accepted nothing but a projection, so it could lock
// onto the plane alone -- and on 2026-09-26 the display was turned off and the
// camera never locked again. The data below is REAL: the last census decode of the
// session that locked (the plane's draw), and the draw recorder's full-precision
// readback of the model draw from a session with the plane hidden, with Archicad's
// own ModelToScreen pixels for the model's corners. A test built from the same
// assumption as the code could not have caught this; these pin what Archicad wrote.

#include "ArchViz/Dxgi/CameraLayout.hpp"
#include "ArchViz/Dxgi/CameraShaderSource.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

namespace cl = geomsrv::archviz::dxgi::cameralayout;
namespace cs = geomsrv::archviz::dxgi::camerashader;

namespace {

// 2026-09-26 18:01:37, the last session that locked: census group g1 as logged
// (four decimals -- the projection's zeros are exact at any precision). Almost
// certainly an editing-plane draw: those are the only draws measured with a
// projection in `b2`.
constexpr float kSeparateView[16] = { -0.6636f, 0.2865f,  -0.6911f, 0.0f, -0.7481f, -0.2541f,  0.6130f,   0.0f,
                                      0.0f,     0.9238f,  0.3829f,  0.0f, 14.8200f, -2.0943f,  -64.5671f, 1.0f };
constexpr float kSeparateProjection[16] = { 1.3034f, 0.0f, 0.0f, 0.0f,     0.0f, 2.6023f, 0.0f,     0.0f,
                                            0.0f,    0.0f, -1.0002f, -1.0f, 0.0f, 0.0f,    -0.0378f, 0.0f };

// 2026-09-27 12:05:59, the draw recorder: the 1512-index model draw of a still
// view, 3432x1803 target, viewport the whole target, window 2288x1202.
constexpr float kCombinedView[16] = { -0.348751128f, 0.132683977f,  -0.927775681f, 0.0f,
                                      -0.937215447f, -0.049373586f, 0.345238447f,  0.0f,
                                      0.0f,          0.989927948f,  0.14157255f,   0.0f,
                                      6.41576767f,   0.632916451f,  -66.4316025f,  1.0f };
constexpr float kCombinedB2[16] = { -0.454540998f, 0.32920149f,   0.930297494f,  0.927775621f,
                                    -1.22150958f,  -0.122500531f, -0.346176893f, -0.345238447f,
                                    0.0f,          2.45610499f,   -0.141957358f, -0.141572535f,
                                    8.3619194f,    1.57032335f,   66.5119019f,   66.4316025f };
// ... and the same draw's `b0`: a rotation-only view x projection, never a census input.
constexpr float kRotationOnlyB0[16] = { -0.454541087f, 0.329201519f,  0.965644062f,   0.927775681f,
                                        -1.22150981f,  -0.122500546f, -0.35932982f,   -0.345238447f,
                                        0.0f,          2.45610523f,   -0.147351012f,  -0.14157255f,
                                        0.0f,          0.0f,          -0.204081625f, 0.0f };

// The model's bounding-box corners and centre, and where Archicad's own
// ModelToScreen put them in that 2288x1202 window.
constexpr float kPoints[9][3] = {
    { -15.246232f, -15.965847f, 0.0f }, { -15.246232f, -15.965847f, 3.0f }, { -15.246232f, 41.089344f, 0.0f },
    { -15.246232f, 41.089344f, 3.0f },  { 47.940990f, -15.965847f, 0.0f },  { 47.940990f, -15.965847f, 3.0f },
    { 47.940990f, 41.089344f, 0.0f },   { 47.940990f, 41.089344f, 3.0f },   { 16.347379f, 12.561749f, 1.5f },
};
constexpr float kPixels[9][2] = { { 1833.0f, 617.0f }, { 1838.0f, 539.0f }, { 96.0f, 735.0f },
                                  { 84.0f, 619.0f },   { 1204.0f, 501.0f }, { 1204.0f, 463.0f },
                                  { 392.0f, 524.0f },  { 388.0f, 478.0f },  { 930.0f, 530.0f } };
constexpr float kWindowWidth = 2288.0f;
constexpr float kWindowHeight = 1202.0f;

// 12:06:04, a steep view of the same session: |b2[11]| = 0.84, which the census's
// old test (`|m[11]| > 0.5`) read as a projection.
constexpr float kSteepView[16] = { -0.961572886f, 0.230146095f,  -0.149701521f, 0.0f,
                                   -0.27455014f,  -0.806054115f, 0.524308205f,  0.0f,
                                   -3.7e-08f,     0.545261145f,  0.838266253f,  0.0f,
                                   8.10955715f,   9.2118988f,    -82.445549f,   1.0f };
constexpr float kSteepB2[16] = { -1.25325549f, 0.571014285f, 0.149970859f,  0.149701536f,
                                 -0.357831985f, -1.99989641f, -0.525251567f, -0.524308264f,
                                 -1.5e-08f,    1.3528446f,   -0.83977437f,  -0.838266253f,
                                 10.5695066f,  22.8555851f,  82.4937058f,   82.4455566f };

// 2026-09-26 20:07:24, census g18 while orbiting (four decimals): `b2` is a whole
// camera and `b1` belongs to a different one. Archicad positions with `b2` alone.
constexpr float kMovingView[16] = { -0.4612f, 0.6621f,  -0.5907f, 0.0f, -0.8873f, -0.3441f, 0.3070f,   0.0f,
                                    0.0f,     0.6657f,  0.7462f,  0.0f, 21.0279f, -2.5832f, -84.2880f, 1.0f };
constexpr float kMovingB2[16] = { -1.1528f, 0.7645f,  0.3859f,  0.3625f,  -0.6082f, -1.4491f, -0.7314f, -0.6872f,
                                  0.0f,     2.0218f,  -0.6701f, -0.6296f, 20.5904f, 14.3621f, 38.6282f, 40.4945f };

// A pixel-to-NDC screen map as the census logs it, and a parallel projection.
constexpr float kScreenMap[16] = { 0.0012f, 0.0f, 0.0f, 0.0f, 0.0f,  -0.0024f, 0.0f, 0.0f,
                                   0.0f,    0.0f, -1.0f, 0.0f, -1.0f, 1.0f,     0.0f, 1.0f };
constexpr float kParallel[16] = { 0.05f, 0.0f, 0.0f,   0.0f, 0.0f, 0.1f, 0.0f,  0.0f,
                                  0.0f,  0.0f, -0.01f, 0.0f, 0.0f, 0.0f, -0.5f, 1.0f };

void Multiply (const float a[16], const float b[16], float out[16])
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = sum;
        }
    }
}

// Where `p * view * projection` puts a model point, as a fraction of the viewport
// (which covers the whole target in the capture). False behind the eye.
bool Fraction (const float point[3], const float view[16], const float projection[16], float& u, float& v)
{
    float m[16];
    Multiply (view, projection, m);
    float clip[4];
    for (int c = 0; c < 4; ++c)
        clip[c] = point[0] * m[c] + point[1] * m[4 + c] + point[2] * m[8 + c] + m[12 + c];
    if (!(clip[3] > 1e-6f))
        return false;
    u = (clip[0] / clip[3] + 1.0f) * 0.5f;
    v = (1.0f - clip[1] / clip[3]) * 0.5f;
    return true;
}

// The worst distance from Archicad's own pixels, as a fraction of the window.
float WorstError (const float view[16], const float projection[16])
{
    float worst = 0.0f;
    for (int i = 0; i < 9; ++i) {
        float u = 0.0f, v = 0.0f;
        if (!Fraction (kPoints[i], view, projection, u, v))
            return 1e9f;
        worst = std::fmax (worst, std::fabs (u - kPixels[i][0] / kWindowWidth));
        worst = std::fmax (worst, std::fabs (v - kPixels[i][1] / kWindowHeight));
    }
    return worst;
}

} // namespace

TEST (CameraLayout, TheLastSessionThatLockedWasSeparate)
{
    EXPECT_EQ (cl::Classify (kSeparateProjection), cl::Layout::Separate);
    float decoded[16];
    EXPECT_EQ (cl::Decode (kSeparateView, kSeparateProjection, decoded), cl::Layout::Separate);
    EXPECT_EQ (std::memcmp (decoded, kSeparateProjection, sizeof (decoded)), 0) << "a projection is passed through";
}

TEST (CameraLayout, TheFirstSessionThatFailedWasCombined)
{
    EXPECT_FALSE (cl::IsProjection (kCombinedB2)) << "b2 is not a projection";
    EXPECT_EQ (cl::Classify (kCombinedB2), cl::Layout::Combined);
}

TEST (CameraLayout, ASteepViewIsNotMistakenForAProjection)
{
    ASSERT_GT (std::fabs (kSteepB2[11]), 0.5f) << "the case the old census test got wrong";
    EXPECT_EQ (cl::Classify (kSteepB2), cl::Layout::Combined);
}

// ⚠️ THE ACCEPTANCE TEST OF THE WHOLE DECODE: the pair the census scores must put
// the model where Archicad does, and the reading the census used before must not.
TEST (CameraLayout, TheDecodedPairReproducesArchicadsOwnPixels)
{
    float decoded[16];
    ASSERT_EQ (cl::Decode (kCombinedView, kCombinedB2, decoded), cl::Layout::Combined);
    EXPECT_LT (WorstError (kCombinedView, decoded), 0.002f) << "within 0.2% of the window at nine model points";
    EXPECT_GT (WorstError (kCombinedView, kCombinedB2), 0.05f) << "p * b1 * b2 is the reading that never locked";
}

TEST (CameraLayout, TheDecodedProjectionIsTheCamerasProjection)
{
    float decoded[16];
    ASSERT_EQ (cl::Decode (kCombinedView, kCombinedB2, decoded), cl::Layout::Combined);
    EXPECT_TRUE (cl::IsProjection (decoded)) << "b1 and b2 of a still view belong to one camera";
    EXPECT_NEAR (decoded[0], 1.3033f, 1e-3f);
    EXPECT_NEAR (decoded[5], 2.4811f, 1e-3f); // 2288 / 1202 x 1.3033: the window's aspect
    EXPECT_NEAR (decoded[11], -1.0f, 1e-4f);
    EXPECT_NEAR (decoded[10], float (-cl::kDepthA), 1e-4f) << "the depth is ours";
    EXPECT_NEAR (decoded[14], float (cl::kDepthB), 1e-4f);
}

// ⚠️ WHY THE COMBINED LAYOUT CARRIES OUR DEPTH: with the editing plane hidden,
// Archicad's own put every model corner beyond its far plane, so the census
// anchor was refused and our draws would have been clipped.
TEST (CameraLayout, ArchicadsOwnDepthPutTheModelBeyondItsFarPlane)
{
    const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    for (int i = 0; i < 9; ++i) {
        float m[16];
        Multiply (identity, kCombinedB2, m);
        const float w = kPoints[i][0] * m[3] + kPoints[i][1] * m[7] + kPoints[i][2] * m[11] + m[15];
        const float z = kPoints[i][0] * m[2] + kPoints[i][1] * m[6] + kPoints[i][2] * m[10] + m[14];
        EXPECT_GT (z / w, 1.0f) << "point " << i << " at " << w << " m";
    }
}

TEST (CameraLayout, OurDepthEnclosesTheModel)
{
    float decoded[16];
    ASSERT_EQ (cl::Decode (kCombinedView, kCombinedB2, decoded), cl::Layout::Combined);
    float m[16];
    Multiply (kCombinedView, decoded, m);
    for (int i = 0; i < 9; ++i) {
        const float w = kPoints[i][0] * m[3] + kPoints[i][1] * m[7] + kPoints[i][2] * m[11] + m[15];
        const float z = kPoints[i][0] * m[2] + kPoints[i][1] * m[6] + kPoints[i][2] * m[10] + m[14];
        EXPECT_GT (z / w, 0.0f) << "point " << i;
        EXPECT_LT (z / w, 1.0f) << "point " << i;
        EXPECT_NEAR (z / w, float (cl::kDepthA + cl::kDepthB / w), 1e-5f) << "point " << i;
    }
}

TEST (CameraLayout, AMovingSampleIsJudgedOnB2Alone)
{
    EXPECT_EQ (cl::Classify (kMovingB2), cl::Layout::Combined);
    float decoded[16];
    ASSERT_EQ (cl::Decode (kMovingView, kMovingB2, decoded), cl::Layout::Combined);
    EXPECT_FALSE (cl::IsProjection (decoded)) << "b1 is another camera's view";
    float product[16];
    Multiply (kMovingView, decoded, product);
    double drawn[16];
    cl::DecodedDepth (kMovingB2, drawn); // b2 itself, at our depth
    for (int i = 0; i < 16; ++i)
        EXPECT_NEAR (product[i], float (drawn[i]), 1e-3f * (1.0f + std::fabs (float (drawn[i])))) << "element " << i;
    for (int i : { 0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15 })
        EXPECT_EQ (float (drawn[i]), kMovingB2[i]) << "x, y and w are Archicad's: element " << i;
}

TEST (CameraLayout, NothingThatIsNotACameraDecodes)
{
    EXPECT_EQ (cl::Classify (kScreenMap), cl::Layout::Neither);
    EXPECT_EQ (cl::Classify (kParallel), cl::Layout::Neither) << "parallel projections stay unsupported";
    float decoded[16];
    EXPECT_EQ (cl::Decode (kCombinedView, kScreenMap, decoded), cl::Layout::Neither);
    EXPECT_EQ (std::memcmp (decoded, kScreenMap, sizeof (decoded)), 0) << "passed through unchanged";
    // `b0` has the shape too; the census never reads it, and this is why that matters.
    EXPECT_EQ (cl::Classify (kRotationOnlyB0), cl::Layout::Combined);
}

TEST (CameraLayout, ACombinedB2OverASingularViewIsNotDecoded)
{
    const float singular[16] = {};
    float decoded[16];
    EXPECT_EQ (cl::Decode (singular, kCombinedB2, decoded), cl::Layout::Neither);
}

TEST (CameraLayout, TheGateWantsEverySampleDecodedInOneLayout)
{
    EXPECT_TRUE (cl::Decodes (32, 32, 0));   // separate
    EXPECT_TRUE (cl::Decodes (32, 32, 32));  // combined
    EXPECT_FALSE (cl::Decodes (32, 32, 5));  // Archicad switched while this group was watched
    EXPECT_FALSE (cl::Decodes (32, 31, 0));  // one screen map among the samples
    EXPECT_FALSE (cl::Decodes (0, 0, 0));
}

TEST (CameraLayout, TheLayoutRidesInTheInterpretation)
{
    EXPECT_EQ (cl::Interpretation (0, false), 0u);
    EXPECT_EQ (cl::Interpretation (2, true), 2u | cl::kCombined);
    EXPECT_TRUE (cl::IsCombined (cl::Interpretation (0, true)));
    EXPECT_FALSE (cl::IsCombined (0xffffffffu)) << "no interpretation is not a layout";
}

TEST (CameraShaderSource, EverySlotRoundTrips)
{
    for (uint32_t slot = 0; slot < cs::kShaderSlots; ++slot) {
        const uint32_t interpretation = cs::InterpretationOfSlot (slot);
        EXPECT_TRUE (cs::Declarable (interpretation));
        EXPECT_EQ (cs::SlotOf (interpretation), slot);
    }
    for (uint32_t reversed = 4; reversed < 8; ++reversed)
        EXPECT_FALSE (cs::Declarable (reversed)) << "a reversed multiplication order has no declaration";
    EXPECT_FALSE (cs::Declarable (0xffffffffu));
}

TEST (CameraShaderSource, AnUndeclarableOrMissingShaderFallsBackToVariantZero)
{
    int dummy = 0;
    int* shaders[cs::kShaderSlots] = {};
    shaders[0] = &dummy;
    shaders[cs::SlotOf (1u | cl::kCombined)] = &dummy;
    EXPECT_EQ (cs::SlotToBind (1u | cl::kCombined, shaders), cs::SlotOf (1u | cl::kCombined));
    EXPECT_EQ (cs::SlotToBind (2u, shaders), 0u) << "missing shader";
    EXPECT_EQ (cs::SlotToBind (5u, shaders), 0u) << "reversed order";
    EXPECT_EQ (cs::SlotToBind (0xffffffffu, shaders), 0u) << "nothing selected";
}

TEST (CameraShaderSource, EachLayoutComposesItsOwnClip)
{
    char separate[cs::kMaxSource] = {};
    char combined[cs::kMaxSource] = {};
    ASSERT_TRUE (cs::Compose (0u, "BODY", separate, sizeof (separate)));
    ASSERT_TRUE (cs::Compose (cl::kCombined, "BODY", combined, sizeof (combined)));
    const std::string a (separate), b (combined);
    EXPECT_NE (a.find ("return mul (mul (world, View), Projection);"), std::string::npos);
    EXPECT_NE (b.find ("float4 c = mul (world, Projection); c.z = "), std::string::npos) << "our depth";
    EXPECT_EQ (b.find ("mul (world, View)"), std::string::npos) << "the view is already in b2";
    EXPECT_EQ (a.substr (a.size () - 4), "BODY");
    EXPECT_FALSE (cs::Compose (4u, "BODY", separate, sizeof (separate))) << "reversed order refused";
}
