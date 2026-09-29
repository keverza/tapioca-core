// ArchViz/Dxgi/GuestShaderSources: an automatic halo follows the text as drawn, run on
// WARP (GuestKernel.hpp) through the shader's own HaloReach. The sizes are the ones asked
// for after the live run of 2026-09-29 (10:39), where a fixed 1.5 px halo was a blot
// round far and small labels: 0.5 px round a 24 px text, 2 px round a 72 px one.

#include "GuestKernel.hpp"

#include "ArchViz/OverlayText.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace text = geomsrv::archviz::overlaytext;
using guestkernel::Float4;

namespace {

// Input: x the vertex's haloPixels, y the em on screen in pixels. Output: the reach.
constexpr const char* kHaloKernel = R"hlsl(
StructuredBuffer<float4> Input : register (t8);
RWStructuredBuffer<float4> Output : register (u0);

[numthreads (1, 1, 1)]
void CSMain (uint3 id : SV_DispatchThreadID)
{
    float4 v = Input[id.x];
    float screenRange = v.y * Atlas.z / Atlas.w;
    Output[id.x] = float4 (HaloReach (v.x, screenRange), screenRange, 0.0, 0.0);
}
)hlsl";

std::vector<Float4> Reach (const std::vector<Float4>& input)
{
    guestkernel::Inputs inputs;
    inputs.variables = { { "Atlas", { 0.0f, 0.0f, text::Engine::DistanceRangePixels (), text::Engine::EmPixels () } } };
    std::string error;
    std::vector<Float4> out = guestkernel::Run (kHaloKernel, inputs, input, 1, error);
    EXPECT_EQ (out.size (), input.size ()) << error;
    return out;
}

} // namespace

TEST (GuestHalo, AnAutomaticHaloIsHalfAPixelAt24AndTwoAt72)
{
    const std::vector<Float4> out = Reach ({ { -1.0f, 24.0f, 0, 0 },
                                             { -1.0f, 72.0f, 0, 0 },
                                             { -1.0f, 8.0f, 0, 0 },
                                             { -1.0f, 12.0f, 0, 0 },
                                             { -0.5f, 72.0f, 0, 0 } });
    ASSERT_EQ (out.size (), 5u);
    EXPECT_NEAR (out[0][0], 0.5f, 1e-4f);
    EXPECT_NEAR (out[1][0], 2.0f, 1e-4f);
    EXPECT_NEAR (out[2][0], 0.0f, 1e-6f);   // none under 8 px
    EXPECT_NEAR (out[3][0], 0.125f, 1e-4f); // a small label keeps a trace
    EXPECT_NEAR (out[4][0], 1.0f, 1e-4f);   // haloScale 0.5
}

TEST (GuestHalo, AFixedHaloIsKeptUntilTheAtlasStopsKnowing)
{
    const std::vector<Float4> out = Reach ({ { 1.5f, 24.0f, 0, 0 }, { 8.0f, 11.0f, 0, 0 } });
    ASSERT_EQ (out.size (), 2u);
    EXPECT_NEAR (out[0][0], 1.5f, 1e-5f);
    // 11 px: the atlas records about 1.65 px beyond the edge; past that, a box per glyph.
    EXPECT_NEAR (out[1][0], 0.5f * out[1][1] - 0.75f, 1e-5f);
    EXPECT_LT (out[1][0], 1.0f);
}
