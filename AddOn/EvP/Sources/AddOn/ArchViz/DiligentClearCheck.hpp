#ifndef EVP_ARCHVIZ_DILIGENTCLEARCHECK_HPP
#define EVP_ARCHVIZ_DILIGENTCLEARCHECK_HPP

// ArchViz/DiligentClearCheck — "is that pixel the colour we asked for?".
//
// PLAT-RE22 stalled on a readback that reported 0,0,0,0 and a log line that
// could say nothing else about it. Deciding whether a back-buffer byte matches
// the float the clear was given is NOT trivial, and every part of it is a place
// the answer can be wrong while the renderer is fine:
//
//   * Diligent's default swap chain is TEX_FORMAT_RGBA8_UNORM_SRGB. The flip
//     model cannot present an sRGB buffer, so SwapChainD3DBase creates the
//     TEXTURE as R8G8B8A8_UNORM and the RTV as R8G8B8A8_UNORM_SRGB. A clear of
//     linear 0.72 therefore STORES 221, not 184. Comparing against 184 reports
//     a fault that does not exist.
//   * The texture may be BGRA, in which case bytes[0] is blue. A channel-order
//     mistake turns cyan into orange and reads like a shader bug.
//
// So this file is deliberately free of D3D11, Diligent, DG and the DevKit: it
// takes the raw bytes plus the two format codes and returns a verdict, which is
// the only part of the black-viewport diagnosis that can be checked offline
// (tests/cpp/test_diligentclear.cpp) rather than by opening Archicad.

#include <cstdint>
#include <string>

namespace geomsrv::archviz {

enum class ChannelOrder { Unknown, Rgba, Bgra };

struct BackBufferFormatInfo {
    ChannelOrder order = ChannelOrder::Unknown;
    bool byteAddressable = false;   // false => bytes[] cannot be read as 4 x uint8
    bool srgb = false;
    const char* name = "UNKNOWN";
};

// dxgiFormat is a raw DXGI_FORMAT value. Taking the number rather than the enum
// is what keeps this translation unit free of <d3d11.h>.
BackBufferFormatInfo DescribeDxgiFormat (uint32_t dxgiFormat);

// One 1x1 readback of a back buffer, exactly as the GPU side reports it.
struct ClearReadback {
    bool mapped = false;            // false => the copy or the Map failed
    std::string failure;            // why, when !mapped
    uint32_t textureFormat = 0;     // DXGI format of the resource copied FROM
    uint32_t viewFormat = 0;        // DXGI format of the RTV that was cleared
    uint8_t bytes[4] = {0, 0, 0, 0};// memory order, not channel order
};

struct ClearVerdict {
    bool decoded = false;           // the format was understood
    bool matched = false;           // observed == expected within tolerance
    bool fullyBlack = false;        // all four bytes zero: the PLAT-RE22 signature
    uint8_t observed[4] = {0, 0, 0, 0};   // R, G, B, A
    uint8_t expected[4] = {0, 0, 0, 0};   // R, G, B, A
    int maxDelta = 0;
    int worstChannel = -1;          // 0=R 1=G 2=B 3=A, -1 when nothing compared
    std::string message;            // always populated; one log line
};

// Linear float -> stored byte, for a plain UNORM target.
uint8_t EncodeUnormByte (float value);
// Linear float -> stored byte, for an _SRGB target (the D3D11 clear path
// applies the sRGB transfer function on write).
uint8_t EncodeSrgbByte (float linear);

// expectedLinearRgba is the four floats handed to ClearRenderTarget.
// tolerance is the per-channel byte allowance (sRGB rounding needs at least 1).
ClearVerdict EvaluateClear (const ClearReadback& readback,
                            const float expectedLinearRgba[4],
                            int tolerance);

}   // namespace geomsrv::archviz

#endif
