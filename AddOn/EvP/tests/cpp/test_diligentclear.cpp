// ArchViz/DiligentClearCheck — the offline half of the PLAT-RE22 diagnosis.
//
// ⚠️ WHY OFFLINE: the Diligent migration's first live gate is "did the viewport
// go the colour we asked for", and three runs inside Archicad could not answer
// it because the READBACK VERDICT was as suspect as the renderer. Every wrong
// answer here looks exactly like a broken renderer from inside Archicad:
//
//   * comparing a linear byte against an sRGB-encoded store reports a fault in
//     a viewport that is rendering perfectly (Diligent's default swap chain is
//     RGBA8_UNORM_SRGB, so the FLIP-model texture is UNORM and the RTV is SRGB);
//   * reading BGRA memory as RGBA turns cyan into orange;
//   * a failed Map that is reported as a zero pixel is indistinguishable from a
//     black surface, which is precisely how the first live run stalled.
//
// None of that needs a GPU to check, so none of it should cost an Archicad run.
// What this file does NOT cover: whether Diligent, D3D11 or the driver actually
// puts the right bytes in the back buffer. That is the in-Archicad A/B in
// DiligentViewport.cpp → RunClearAB, and it stays a live test.

#include "ArchViz/DiligentClearCheck.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

// Raw DXGI_FORMAT values, repeated here on purpose: a test that imported the
// module's own constants could not catch the module renumbering them.
constexpr uint32_t kRgba8Unorm     = 28;
constexpr uint32_t kRgba8UnormSrgb = 29;
constexpr uint32_t kBgra8Unorm     = 87;
constexpr uint32_t kBgra8UnormSrgb = 91;
constexpr uint32_t kRgb10A2Unorm   = 24;

// The colour DiligentViewport clears with, and what it must STORE. Both byte
// sets are worked out from the sRGB transfer function by hand rather than from
// EncodeSrgbByte, so an error in the encoder cannot hide behind itself:
//   0.02 -> 1.055*0.02^(1/2.4)-0.055 = 0.15165 -> 38.67 -> 39
//   0.72 -> 1.055*0.72^(1/2.4)-0.055 = 0.86505 -> 220.59 -> 221
constexpr float kCyan[4]       = {0.02f, 0.72f, 1.0f, 1.0f};
constexpr uint8_t kCyanSrgb[4] = {39, 221, 255, 255};
// The same colour if the store were linear: 0.02*255 = 5.1, 0.72*255 = 183.6.
constexpr uint8_t kCyanLinear[4] = {5, 184, 255, 255};

ClearReadback Mapped (uint32_t textureFormat, uint32_t viewFormat, const uint8_t bytes[4])
{
    ClearReadback readback;
    readback.mapped = true;
    readback.textureFormat = textureFormat;
    readback.viewFormat = viewFormat;
    for (int i = 0; i < 4; ++i)
        readback.bytes[i] = bytes[i];
    return readback;
}

}   // namespace

// --- the sRGB trap ----------------------------------------------------------

// This is the exact configuration Diligent gives us: SwapChainDesc defaults to
// TEX_FORMAT_RGBA8_UNORM_SRGB, and SwapChainD3DBase creates the flip-model
// buffer as plain UNORM with an _SRGB view over it.
TEST (DiligentClearCheck, SrgbViewOverUnormTextureMatchesEncodedBytes)
{
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, kCyanSrgb), kCyan, 2);
    EXPECT_TRUE (v.decoded);
    EXPECT_TRUE (v.matched) << v.message;
    EXPECT_EQ (v.maxDelta, 0);
    EXPECT_FALSE (v.fullyBlack);
    EXPECT_NE (v.message.find ("transfer=sRGB"), std::string::npos) << v.message;
}

// The regression that would send someone hunting a renderer bug that is not
// there: the stored bytes are correct for a LINEAR target and wrong here.
TEST (DiligentClearCheck, SrgbViewRejectsLinearBytes)
{
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, kCyanLinear), kCyan, 2);
    ASSERT_TRUE (v.decoded);
    EXPECT_FALSE (v.matched);
    EXPECT_EQ (v.worstChannel, 1);      // green: 184 stored vs 221 expected
    EXPECT_EQ (v.maxDelta, 37);
}

TEST (DiligentClearCheck, LinearViewComparesAgainstLinearBytes)
{
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8Unorm, kRgba8Unorm, kCyanLinear), kCyan, 2);
    EXPECT_TRUE (v.matched) << v.message;
    EXPECT_NE (v.message.find ("transfer=linear"), std::string::npos) << v.message;
}

// --- channel order ----------------------------------------------------------

TEST (DiligentClearCheck, BgraMemoryIsDecodedInChannelOrder)
{
    // Memory order B,G,R,A for the same cyan.
    const uint8_t bytes[4] = {kCyanSrgb[2], kCyanSrgb[1], kCyanSrgb[0], kCyanSrgb[3]};
    const ClearVerdict v = EvaluateClear (Mapped (kBgra8Unorm, kBgra8UnormSrgb, bytes), kCyan, 2);
    ASSERT_TRUE (v.decoded);
    EXPECT_TRUE (v.matched) << v.message;
    EXPECT_EQ (v.observed[0], kCyanSrgb[0]);
    EXPECT_EQ (v.observed[2], kCyanSrgb[2]);
}

// Reading BGRA memory as RGBA is the mistake this guards: it must FAIL, and it
// must fail on red and blue rather than quietly passing.
TEST (DiligentClearCheck, RgbaDecodeOfBgraBytesIsCaught)
{
    const uint8_t bytes[4] = {kCyanSrgb[2], kCyanSrgb[1], kCyanSrgb[0], kCyanSrgb[3]};
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, bytes), kCyan, 2);
    ASSERT_TRUE (v.decoded);
    EXPECT_FALSE (v.matched);
    EXPECT_EQ (v.observed[0], 255);
    EXPECT_EQ (v.observed[2], 39);
}

// --- the failure modes that all look like "black" ---------------------------

TEST (DiligentClearCheck, AllZeroBytesAreNamedAsThePlatRe22Signature)
{
    const uint8_t bytes[4] = {0, 0, 0, 0};
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, bytes), kCyan, 2);
    EXPECT_TRUE (v.decoded);
    EXPECT_FALSE (v.matched);
    EXPECT_TRUE (v.fullyBlack);
    EXPECT_NE (v.message.find ("PLAT-RE22"), std::string::npos) << v.message;
}

// A Map that never succeeded is NOT a black pixel, and the report must not let
// the two be confused -- the first live diagnostic did exactly that.
TEST (DiligentClearCheck, FailedReadbackIsNotReportedAsAColour)
{
    ClearReadback readback;
    readback.mapped = false;
    readback.failure = "failed to map the cleared back buffer, HRESULT=2289696773";
    readback.textureFormat = kRgba8Unorm;
    readback.viewFormat = kRgba8UnormSrgb;

    const ClearVerdict v = EvaluateClear (readback, kCyan, 2);
    EXPECT_FALSE (v.decoded);
    EXPECT_FALSE (v.matched);
    EXPECT_FALSE (v.fullyBlack);
    EXPECT_EQ (v.message.rfind ("READBACK FAILED", 0), 0u) << v.message;
    EXPECT_NE (v.message.find ("HRESULT=2289696773"), std::string::npos) << v.message;
}

TEST (DiligentClearCheck, FailedReadbackWithoutAReasonStillSaysSo)
{
    ClearReadback readback;
    readback.mapped = false;
    const ClearVerdict v = EvaluateClear (readback, kCyan, 2);
    EXPECT_NE (v.message.find ("no reason reported"), std::string::npos) << v.message;
}

// A format we cannot read as four bytes must refuse to decode rather than
// invent a colour out of 10-bit or half-float memory.
TEST (DiligentClearCheck, UndecodableFormatRefusesToGuess)
{
    const uint8_t bytes[4] = {1, 2, 3, 4};
    const ClearVerdict v = EvaluateClear (Mapped (kRgb10A2Unorm, kRgb10A2Unorm, bytes), kCyan, 2);
    EXPECT_FALSE (v.decoded);
    EXPECT_FALSE (v.matched);
    EXPECT_NE (v.message.find ("UNDECODED"), std::string::npos) << v.message;
    EXPECT_NE (v.message.find ("R10G10B10A2_UNORM"), std::string::npos) << v.message;
}

// An unrecognised view format falls back to the texture's transfer function
// instead of silently assuming linear.
TEST (DiligentClearCheck, UnknownViewFormatFallsBackToTheTexture)
{
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8UnormSrgb, 0, kCyanSrgb), kCyan, 2);
    EXPECT_TRUE (v.matched) << v.message;
    EXPECT_NE (v.message.find ("transfer=sRGB"), std::string::npos) << v.message;
}

// --- tolerance --------------------------------------------------------------

TEST (DiligentClearCheck, ToleranceIsInclusiveAndReportsTheWorstChannel)
{
    uint8_t within[4] = {kCyanSrgb[0], uint8_t (kCyanSrgb[1] - 2), kCyanSrgb[2], kCyanSrgb[3]};
    EXPECT_TRUE (EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, within), kCyan, 2).matched);

    uint8_t beyond[4] = {kCyanSrgb[0], uint8_t (kCyanSrgb[1] - 3), kCyanSrgb[2], kCyanSrgb[3]};
    const ClearVerdict v = EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, beyond), kCyan, 2);
    EXPECT_FALSE (v.matched);
    EXPECT_EQ (v.maxDelta, 3);
    EXPECT_EQ (v.worstChannel, 1);
}

TEST (DiligentClearCheck, NegativeToleranceIsTreatedAsExact)
{
    uint8_t off[4] = {kCyanSrgb[0], uint8_t (kCyanSrgb[1] - 1), kCyanSrgb[2], kCyanSrgb[3]};
    EXPECT_FALSE (EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, off), kCyan, -5).matched);
    EXPECT_TRUE (EvaluateClear (Mapped (kRgba8Unorm, kRgba8UnormSrgb, kCyanSrgb), kCyan, -5).matched);
}

// --- the encoders -----------------------------------------------------------

TEST (DiligentClearCheck, EncodersClampAndRound)
{
    EXPECT_EQ (EncodeUnormByte (-1.0f), 0);
    EXPECT_EQ (EncodeUnormByte (0.0f), 0);
    EXPECT_EQ (EncodeUnormByte (1.0f), 255);
    EXPECT_EQ (EncodeUnormByte (2.0f), 255);
    EXPECT_EQ (EncodeUnormByte (0.5f), 128);      // 127.5 rounds away from zero

    EXPECT_EQ (EncodeSrgbByte (0.0f), 0);
    EXPECT_EQ (EncodeSrgbByte (1.0f), 255);
    EXPECT_EQ (EncodeSrgbByte (-1.0f), 0);
    // Below the 0.0031308 knee the curve is the linear 12.92x segment.
    EXPECT_EQ (EncodeSrgbByte (0.001f), EncodeUnormByte (0.001f * 12.92f));
    EXPECT_EQ (EncodeSrgbByte (0.02f), 39);
    EXPECT_EQ (EncodeSrgbByte (0.72f), 221);
}

TEST (DiligentClearCheck, FormatTableKnowsOrderAndTransfer)
{
    EXPECT_EQ (DescribeDxgiFormat (kRgba8Unorm).order, ChannelOrder::Rgba);
    EXPECT_FALSE (DescribeDxgiFormat (kRgba8Unorm).srgb);
    EXPECT_TRUE (DescribeDxgiFormat (kRgba8UnormSrgb).srgb);
    EXPECT_EQ (DescribeDxgiFormat (kBgra8Unorm).order, ChannelOrder::Bgra);
    EXPECT_TRUE (DescribeDxgiFormat (kBgra8UnormSrgb).srgb);
    EXPECT_TRUE (DescribeDxgiFormat (kRgba8Unorm).byteAddressable);
    EXPECT_FALSE (DescribeDxgiFormat (kRgb10A2Unorm).byteAddressable);
    EXPECT_FALSE (DescribeDxgiFormat (999999).byteAddressable);
    EXPECT_STREQ (DescribeDxgiFormat (999999).name, "UNKNOWN");
}
