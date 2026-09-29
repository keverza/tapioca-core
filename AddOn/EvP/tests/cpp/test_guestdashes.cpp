// ArchViz/Dxgi/GuestShaderSources: a dash pattern belongs to the model, run on WARP
// (GuestKernel.hpp) through the shader's own functions. The screen-space dash it replaced
// slid along the line with every orbit and read as an animation (the user, 2026-09-29).
//
// DashCoverage: in a dash, in a gap, half at an edge, and an even average once a period is
// finer than a few pixels. VSLine: the metres it hands the rasterizer, interpolated as the
// rasterizer does, give a model point the same metres from two different cameras.

#include "GuestKernel.hpp"

#include "ArchViz/Dxgi/CameraLayout.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace layout = geomsrv::archviz::dxgi::cameralayout;
namespace style = geomsrv::archviz::dxgi::overlay;
using guestkernel::Float4;

namespace {

// Input: x the pattern, y the metres, z the metres a pixel spans. Output: the coverage.
constexpr const char* kCoverageKernel = R"hlsl(
StructuredBuffer<float4> Input : register (t8);
RWStructuredBuffer<float4> Output : register (u0);

[numthreads (1, 1, 1)]
void CSMain (uint3 id : SV_DispatchThreadID)
{
    float4 v = Input[id.x];
    Output[id.x] = float4 (DashCoverage ((uint) v.x, v.y, v.z), 0.0, 0.0, 0.0);
}
)hlsl";

// Input 0: the line's a; 1: its b; 2: the model point to follow. Output: for the quad's
// corners 0 (at a) and 1 (at b), the clip position and the metres pair; then the point's
// clip position.
constexpr const char* kLineKernel = R"hlsl(
StructuredBuffer<float4> Input : register (t8);
RWStructuredBuffer<float4> Output : register (u0);

[numthreads (1, 1, 1)]
void CSMain (uint3 id : SV_DispatchThreadID)
{
    float3 a = Input[0].xyz;
    float3 b = Input[1].xyz;
    float3 style = float3 (2.0, 0.0, 10.0); // 2 px wide, the pattern standing 10 m along
    // behind 0, hidden: drawn in the near pass (Mode.z 0), so the corners are placed.
    LineOut at = VSLine (a, b, float4 (1, 1, 1, 1), float4 (0, 0, 0, 0), style, 0u, 0u, 0u);
    LineOut to = VSLine (a, b, float4 (1, 1, 1, 1), float4 (0, 0, 0, 0), style, 0u, 0u, 1u);
    Output[0] = at.position;
    Output[1] = float4 (at.metres, 0.0, 0.0);
    Output[2] = to.position;
    Output[3] = float4 (to.metres, 0.0, 0.0);
    float3 p = Input[2].xyz;
    Output[4] = TowardEye (ArchicadClip (float4 (p, 1.0)), p);
}
)hlsl";

guestkernel::Inputs Patterns ()
{
    guestkernel::Inputs inputs;
    std::vector<float> dashes (32 * 4, 0.0f);
    dashes[0] = 1.0f; // pattern 0: 1 m on, 1 m off
    dashes[1] = 1.0f;
    dashes[8] = 1.0f; // pattern 1: a dash-dot, 1 on, 0.25 off, 0.1 on, 0.25 off
    dashes[9] = 0.25f;
    dashes[10] = 0.1f;
    dashes[11] = 0.25f;
    inputs.variables = { { "Dashes", dashes } };
    return inputs;
}

float Coverage (float pattern, float metres, float metresPerPixel)
{
    std::string error;
    const std::vector<Float4> out =
        guestkernel::Run (kCoverageKernel, Patterns (), { { pattern, metres, metresPerPixel, 0.0f } }, 1, error);
    EXPECT_EQ (out.size (), 1u) << error;
    return out.empty () ? -1.0f : out[0][0];
}

// The eye at `eye`, looking along +y, as the production reading draws it
// (test_guestdepthpull.cpp says how the matrices read).
guestkernel::Inputs Camera (float eyeX, float eyeY, float eyeZ)
{
    guestkernel::Inputs inputs;
    inputs.interpretation = layout::kRelative;
    const float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eyeX, -eyeY, -eyeZ, 1 };
    const float s = 1.5f;
    const float projection[16] = { s, 0, 0, 0, 0, 0, 0, 1, 0, s, 0, 0, 0, 0, 0, 0 };
    std::copy (view, view + 16, inputs.view);
    std::copy (projection, projection + 16, inputs.projection);
    inputs.variables = { { "Surface", { 1000.0f, 800.0f, 1.0f, style::kGuestDepthPullFraction } },
                         { "Mode", { 0.0f, 1.0f, 0.0f, 0.0f } } };
    return inputs;
}

// The metres the rasterizer gives the fragment where `point` lands on the line's quad:
// its screen position between the two corners, the pair interpolated there, divided.
double MetresAt (const guestkernel::Inputs& camera, const Float4& a, const Float4& b, const Float4& point)
{
    std::string error;
    const std::vector<Float4> out = guestkernel::Run (kLineKernel, camera, { a, b, point }, 5, error);
    // Room for five per input thread; the kernel's one reading is the first five.
    EXPECT_GE (out.size (), 5u) << error;
    if (out.size () < 5u)
        return -1.0;
    // In pixels (1000 x 800), where the quad's offset is square to the line.
    const double x0 = out[0][0] * 500.0, y0 = out[0][1] * 400.0, x1 = out[2][0] * 500.0, y1 = out[2][1] * 400.0;
    const double px = out[4][0] / out[4][3] * 500.0, py = out[4][1] / out[4][3] * 400.0;
    const double dx = x1 - x0, dy = y1 - y0;
    const double u = ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy);
    const double over = out[1][0] + u * (out[3][0] - out[1][0]);
    const double inverse = out[1][1] + u * (out[3][1] - out[1][1]);
    return over / inverse;
}

} // namespace

TEST (GuestDashes, ADashCoversAGapDoesNotAndAnEdgeIsHalf)
{
    EXPECT_NEAR (Coverage (0, 0.5f, 0.01f), 1.0f, 1e-5f);
    EXPECT_NEAR (Coverage (0, 1.5f, 0.01f), 0.0f, 1e-5f);
    EXPECT_NEAR (Coverage (0, 2.5f, 0.01f), 1.0f, 1e-5f); // the next period
    EXPECT_NEAR (Coverage (0, 1.0f, 0.01f), 0.5f, 1e-4f); // at the edge
    EXPECT_NEAR (Coverage (1, 1.3f, 0.01f), 1.0f, 1e-5f); // the dot of a dash-dot
    EXPECT_NEAR (Coverage (1, 1.15f, 0.01f), 0.0f, 1e-5f);
    EXPECT_NEAR (Coverage (255, 1.5f, 0.01f), 1.0f, 1e-6f); // solid
}

// A 2 m period over 2 pixels would shimmer: it is an even line at the dashes' share.
TEST (GuestDashes, APatternFinerThanThePixelsIsItsAverage)
{
    EXPECT_NEAR (Coverage (0, 0.5f, 1.0f), 0.5f, 1e-5f);
    EXPECT_NEAR (Coverage (0, 1.5f, 1.0f), 0.5f, 1e-5f);
    EXPECT_NEAR (Coverage (1, 1.15f, 1.0f), 1.1f / 1.6f, 1e-5f);
}

// ⚠️ THE CRAWL, PINNED. A point 7 m along a line that recedes from the eye has the pattern's
// metres 10 + 7 wherever the camera stands -- the rasterizer's own interpolation of what
// VSLine hands it, perspective-correct -- so a dash stays on the model as the view moves.
TEST (GuestDashes, AModelPointHasTheSameMetresFromEveryCamera)
{
    const Float4 a = { 0.0f, 5.0f, 0.0f, 1.0f }, b = { 0.0f, 25.0f, 3.0f, 1.0f };
    const double length = std::sqrt (20.0 * 20.0 + 3.0 * 3.0);
    const double along = 7.0;
    const Float4 point = { 0.0f, float (5.0 + 20.0 * along / length), float (3.0 * along / length), 1.0f };
    for (const auto& eye : { std::vector<float> { 0.0f, -10.0f, 1.6f }, std::vector<float> { 4.0f, -3.0f, 8.0f },
                             std::vector<float> { -6.0f, 0.0f, 2.0f } })
        EXPECT_NEAR (MetresAt (Camera (eye[0], eye[1], eye[2]), a, b, point), 10.0 + along, 1e-3)
            << eye[0] << " " << eye[1] << " " << eye[2];
}
