// ArchViz/Dxgi/GuestShaderSources: a legend pointed at shows its band of the heatmap and
// dims the rest (OverlayHud.hpp `Layout::Highlight`), run on WARP (GuestKernel.hpp)
// through the shader's own `Highlighted`. PSFill itself takes a screen derivative, which
// a compute kernel cannot, so the band's rule is a function of its own and pinned here.

#include "GuestKernel.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using guestkernel::Float4;

namespace {

// Input: the colour's rgb and the value in w. Output: the colour Highlighted gives it.
constexpr const char* kKernel = R"hlsl(
StructuredBuffer<float4> Input : register (t8);
RWStructuredBuffer<float4> Output : register (u0);

[numthreads (1, 1, 1)]
void CSMain (uint3 id : SV_DispatchThreadID)
{
    float4 v = Input[id.x];
    Output[id.x] = Highlighted (float4 (v.rgb, 0.8), v.w);
}
)hlsl";

Float4 Shade (float value, float low, float high, float active)
{
    guestkernel::Inputs inputs;
    inputs.variables = { { "Highlight", { low, high, active, 0.0f } } };
    std::string error;
    const std::vector<Float4> out = guestkernel::Run (kKernel, inputs, { { 0.9f, 0.2f, 0.1f, value } }, 1, error);
    EXPECT_GE (out.size (), 1u) << error;
    return out.empty () ? Float4 {} : out[0];
}

} // namespace

// Inside the band the heatmap is exactly as it was; its ends are inside.
TEST (GuestHighlight, TheBandKeepsItsColour)
{
    for (const float value : { 4.0f, 5.0f, 6.0f }) {
        const Float4 c = Shade (value, 4.0f, 6.0f, 1.0f);
        EXPECT_NEAR (c[0], 0.9f, 1e-6f) << value;
        EXPECT_NEAR (c[1], 0.2f, 1e-6f);
        EXPECT_NEAR (c[2], 0.1f, 1e-6f);
        EXPECT_NEAR (c[3], 0.8f, 1e-6f);
    }
}

// Outside it, mostly grey and faint -- on either side.
TEST (GuestHighlight, TheRestIsDimmed)
{
    for (const float value : { 3.9f, 6.1f, -100.0f }) {
        const Float4 c = Shade (value, 4.0f, 6.0f, 1.0f);
        const float grey = 0.299f * 0.9f + 0.587f * 0.2f + 0.114f * 0.1f;
        EXPECT_NEAR (c[0], 0.9f + (grey - 0.9f) * 0.75f, 1e-5f) << value;
        EXPECT_NEAR (c[1], 0.2f + (grey - 0.2f) * 0.75f, 1e-5f);
        EXPECT_NEAR (c[3], 0.8f * 0.35f, 1e-6f);
    }
}

// With nothing pointed at, every value keeps its colour.
TEST (GuestHighlight, NoBandDimsNothing)
{
    const Float4 c = Shade (100.0f, 4.0f, 6.0f, 0.0f);
    EXPECT_NEAR (c[0], 0.9f, 1e-6f);
    EXPECT_NEAR (c[3], 0.8f, 1e-6f);
}
