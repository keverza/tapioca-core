// ArchViz/Dxgi/GuestShaderSources: the guest's 3D depth pull, RUN -- not just compiled --
// on WARP (GuestKernel.hpp). `TowardEye` decides what the building hides; the constant
// NDC step it replaced reached metres at the distances a model is seen from, and a ghost
// box behind a wall came in front of it at some of them (the live run of 2026-09-29,
// 10:39). The kernel calls the shader's own function with a camera of known geometry.

#include "GuestKernel.hpp"

#include "ArchViz/Dxgi/CameraLayout.hpp"
#include "ArchViz/Dxgi/OverlayStyle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace layout = geomsrv::archviz::dxgi::cameralayout;
namespace style = geomsrv::archviz::dxgi::overlay;
using guestkernel::Float4;

namespace {

constexpr const char* kPullKernel = R"hlsl(
StructuredBuffer<float4> Input : register (t8);
RWStructuredBuffer<float4> Output : register (u0);

[numthreads (1, 1, 1)]
void CSMain (uint3 id : SV_DispatchThreadID)
{
    float3 p = Input[id.x].xyz;
    float4 c = ArchicadClip (float4 (p, 1.0));
    Output[id.x * 2] = c;
    Output[id.x * 2 + 1] = TowardEye (c, p);
}
)hlsl";

std::vector<float> Surface ()
{
    return { 1000.0f, 800.0f, 1.0f, style::kGuestDepthPullFraction };
}

// The production reading: the eye from b1, the point moved to it through b0, and the
// overlay's own depth line (CameraLayout.hpp). Here the eye stands at (0, -20, 1.6)
// looking along +y, so w is how far ahead of it a point is.
guestkernel::Inputs Perspective ()
{
    guestkernel::Inputs inputs;
    inputs.interpretation = layout::kRelative;
    const float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 20.0f, -1.6f, 1 };
    const float s = 1.5f; // x and y per metre ahead: about a 67 degree view
    const float projection[16] = { s, 0, 0, 0, 0, 0, 0, 1, 0, s, 0, 0, 0, 0, 0, 0 };
    std::copy (view, view + 16, inputs.view);
    std::copy (projection, projection + 16, inputs.projection);
    inputs.variables = { { "Surface", Surface () } };
    return inputs;
}

// The NDC depth the overlay's own line gives a point `w` metres ahead.
double DepthAt (double w)
{
    return layout::kDepthA + layout::kDepthB / w;
}

} // namespace

TEST (GuestDepthPull, APointIsPulledTowardTheEyeByAFractionOfItsDistanceOnItsOwnPixel)
{
    const std::vector<Float4> points = { { 1.0f, 10.0f, 2.0f, 1.0f },
                                         { -3.0f, 0.0f, 0.5f, 1.0f },
                                         { 0.5f, 80.0f, -1.0f, 1.0f } };
    std::string error;
    const std::vector<Float4> result = guestkernel::Run (kPullKernel, Perspective (), points, 2, error);
    ASSERT_EQ (result.size (), 2 * points.size ()) << error;
    const double f = style::kGuestDepthPullFraction;
    for (size_t i = 0; i < points.size (); ++i) {
        const Float4& c = result[2 * i];
        const Float4& pulled = result[2 * i + 1];
        const double ahead = double (points[i][1]) + 20.0;
        ASSERT_NEAR (c[3], ahead, 1e-4) << i;
        // The same pixel...
        EXPECT_NEAR (pulled[0] / pulled[3], c[0] / c[3], 1e-6) << i;
        EXPECT_NEAR (pulled[1] / pulled[3], c[1] / c[3], 1e-6) << i;
        // ...at the depth of a point (1 - f) as far away.
        EXPECT_NEAR (pulled[2] / pulled[3], DepthAt ((1.0 - f) * ahead), 3e-7) << i;
        EXPECT_LT (pulled[2] / pulled[3], c[2] / c[3]) << i;
    }
}

// What the constant NDC step did at the distance the ghost box was seen from: the
// point drew as if metres nearer. The pull moves it centimetres.
TEST (GuestDepthPull, TheOldStepReachedMetresWhereThePullReachesCentimetres)
{
    const double ahead = 20.0;
    const double stepped = layout::kDepthB / (DepthAt (ahead) - style::kHostWireframeDepthBias - layout::kDepthA);
    EXPECT_GT (ahead - stepped, 5.0);
    EXPECT_LT (ahead * style::kGuestDepthPullFraction, 0.05);
}

// A parallel view has no eye: the pull is 1.5 pixels' worth of metres along the view.
TEST (GuestDepthPull, AParallelViewIsPulledByPixelsWorthOfMetres)
{
    guestkernel::Inputs inputs;
    inputs.interpretation = 0;                       // world x View x Projection, both row-major
    const float s = 0.1f;                            // 20 m across the view
    const float nearPlane = 1.0f, farPlane = 501.0f; // depth along +y
    const float depth = 1.0f / (farPlane - nearPlane);
    const float projection[16] = { s, 0, 0, 0, 0, 0, depth, 0, 0, s, 0, 0, 0, 0, -nearPlane * depth, 1 };
    std::copy (projection, projection + 16, inputs.projection);
    inputs.variables = { { "Surface", Surface () } };
    std::string error;
    const std::vector<Float4> result =
        guestkernel::Run (kPullKernel, inputs, { { 2.0f, 40.0f, 1.0f, 1.0f } }, 2, error);
    ASSERT_EQ (result.size (), 2u) << error;
    const Float4& c = result[0];
    const Float4& pulled = result[1];
    ASSERT_NEAR (c[3], 1.0, 1e-6);
    EXPECT_NEAR (pulled[0], c[0], 1e-6);
    EXPECT_NEAR (pulled[1], c[1], 1e-6);
    const double metresPerPixel = (2.0 / 1000.0) / s; // 0.02 m
    EXPECT_NEAR (pulled[2], c[2] - 1.5 * metresPerPixel * depth, 1e-7);
}
