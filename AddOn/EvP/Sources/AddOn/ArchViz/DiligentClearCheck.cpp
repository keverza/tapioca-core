#include "ArchViz/DiligentClearCheck.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz {

namespace {

// Raw DXGI_FORMAT values. Spelled out rather than included so this TU stays
// free of <d3d11.h> and can build in the offline test project.
constexpr uint32_t kR16G16B16A16Float  = 10;
constexpr uint32_t kR10G10B10A2Unorm   = 24;
constexpr uint32_t kR8G8B8A8Typeless   = 27;
constexpr uint32_t kR8G8B8A8Unorm      = 28;
constexpr uint32_t kR8G8B8A8UnormSrgb  = 29;
constexpr uint32_t kB8G8R8A8Unorm      = 87;
constexpr uint32_t kB8G8R8A8Typeless   = 90;
constexpr uint32_t kB8G8R8A8UnormSrgb  = 91;

std::string Join4 (const uint8_t v[4])
{
    return std::to_string (v[0]) + "," + std::to_string (v[1]) + "," +
           std::to_string (v[2]) + "," + std::to_string (v[3]);
}

}   // namespace

BackBufferFormatInfo DescribeDxgiFormat (uint32_t dxgiFormat)
{
    switch (dxgiFormat) {
        case kR8G8B8A8Typeless:  return {ChannelOrder::Rgba, true,  false, "R8G8B8A8_TYPELESS"};
        case kR8G8B8A8Unorm:     return {ChannelOrder::Rgba, true,  false, "R8G8B8A8_UNORM"};
        case kR8G8B8A8UnormSrgb: return {ChannelOrder::Rgba, true,  true,  "R8G8B8A8_UNORM_SRGB"};
        case kB8G8R8A8Typeless:  return {ChannelOrder::Bgra, true,  false, "B8G8R8A8_TYPELESS"};
        case kB8G8R8A8Unorm:     return {ChannelOrder::Bgra, true,  false, "B8G8R8A8_UNORM"};
        case kB8G8R8A8UnormSrgb: return {ChannelOrder::Bgra, true,  true,  "B8G8R8A8_UNORM_SRGB"};
        // Known, but not four bytes: naming them keeps the log honest instead of
        // decoding 10-bit or half-float memory as if it were RGBA8.
        case kR10G10B10A2Unorm:  return {ChannelOrder::Unknown, false, false, "R10G10B10A2_UNORM"};
        case kR16G16B16A16Float: return {ChannelOrder::Unknown, false, false, "R16G16B16A16_FLOAT"};
        default:                 return {ChannelOrder::Unknown, false, false, "UNKNOWN"};
    }
}

uint8_t EncodeUnormByte (float value)
{
    const float clamped = std::min (1.0f, std::max (0.0f, value));
    return static_cast<uint8_t> (std::lround (clamped * 255.0f));
}

uint8_t EncodeSrgbByte (float linear)
{
    const float clamped = std::min (1.0f, std::max (0.0f, linear));
    const float encoded = clamped <= 0.0031308f
                              ? clamped * 12.92f
                              : 1.055f * std::pow (clamped, 1.0f / 2.4f) - 0.055f;
    return EncodeUnormByte (encoded);
}

ClearVerdict EvaluateClear (const ClearReadback& readback,
                            const float expectedLinearRgba[4],
                            int tolerance)
{
    const BackBufferFormatInfo texture = DescribeDxgiFormat (readback.textureFormat);
    const BackBufferFormatInfo view    = DescribeDxgiFormat (readback.viewFormat);
    const std::string formats = " texture=" + std::string (texture.name) +
                                "(" + std::to_string (readback.textureFormat) + ")" +
                                " view=" + std::string (view.name) +
                                "(" + std::to_string (readback.viewFormat) + ")";

    ClearVerdict verdict;
    if (!readback.mapped) {
        verdict.message = "READBACK FAILED: " +
                          (readback.failure.empty () ? std::string ("no reason reported")
                                                     : readback.failure) +
                          formats;
        return verdict;
    }

    verdict.fullyBlack = readback.bytes[0] == 0 && readback.bytes[1] == 0 &&
                         readback.bytes[2] == 0 && readback.bytes[3] == 0;

    if (!texture.byteAddressable || texture.order == ChannelOrder::Unknown) {
        verdict.message = "UNDECODED: raw=" + Join4 (readback.bytes) +
                          " cannot be read as 4 x uint8" + formats;
        return verdict;
    }

    verdict.decoded = true;
    if (texture.order == ChannelOrder::Bgra) {
        verdict.observed[0] = readback.bytes[2];
        verdict.observed[1] = readback.bytes[1];
        verdict.observed[2] = readback.bytes[0];
    } else {
        verdict.observed[0] = readback.bytes[0];
        verdict.observed[1] = readback.bytes[1];
        verdict.observed[2] = readback.bytes[2];
    }
    verdict.observed[3] = readback.bytes[3];

    // The clear runs THROUGH the view, so the view's transfer function is what
    // decides the stored byte — not the texture's. When the view format is
    // unknown, fall back to the texture's: a same-family typeless/UNORM pair is
    // the common case and guessing linear there is better than refusing.
    const bool srgb = view.byteAddressable ? view.srgb : texture.srgb;
    for (int i = 0; i < 3; ++i)
        verdict.expected[i] = srgb ? EncodeSrgbByte (expectedLinearRgba[i])
                                   : EncodeUnormByte (expectedLinearRgba[i]);
    verdict.expected[3] = EncodeUnormByte (expectedLinearRgba[3]);

    for (int i = 0; i < 4; ++i) {
        const int delta = std::abs (int (verdict.observed[i]) - int (verdict.expected[i]));
        if (delta > verdict.maxDelta) {
            verdict.maxDelta = delta;
            verdict.worstChannel = i;
        }
    }
    if (verdict.worstChannel < 0)
        verdict.worstChannel = 0;
    verdict.matched = verdict.maxDelta <= std::max (0, tolerance);

    verdict.message = std::string (verdict.matched ? "PASS" : "FAIL") +
                      " observed=" + Join4 (verdict.observed) +
                      " expected=" + Join4 (verdict.expected) +
                      " maxDelta=" + std::to_string (verdict.maxDelta) +
                      " (tolerance " + std::to_string (std::max (0, tolerance)) + ")" +
                      " transfer=" + (srgb ? "sRGB" : "linear") + formats;
    if (verdict.fullyBlack)
        verdict.message += " -- all four bytes are zero (the PLAT-RE22 signature)";
    return verdict;
}

}   // namespace geomsrv::archviz
