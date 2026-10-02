// ArchViz/Dxgi/ComposeTiming: the COST line's numbers. The histogram is pure; the query ring
// runs on a WARP device the way `Compose` drives it -- begin, a mark per stage, end -- and
// must hand back timed frames without ever being waited on.

#include "ArchViz/Dxgi/ComposeTiming.hpp"

#include <gtest/gtest.h>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

namespace timing = geomsrv::archviz::dxgi::composetiming;
using Microsoft::WRL::ComPtr;

TEST (ComposeTiming, BucketsGrowByHalfFromATenthOfAMillisecond)
{
    EXPECT_EQ (timing::BucketUpperMicros (0), 100u);
    EXPECT_EQ (timing::BucketUpperMicros (1), 150u);
    EXPECT_EQ (timing::BucketUpperMicros (2), 225u);
    EXPECT_GT (timing::BucketUpperMicros (timing::kBuckets - 1), 1000000u) << "past a second at the last";
    EXPECT_EQ (timing::BucketOf (0), 0u);
    EXPECT_EQ (timing::BucketOf (100), 0u);
    EXPECT_EQ (timing::BucketOf (101), 1u);
    EXPECT_EQ (timing::BucketOf (5000000), timing::kBuckets - 1);
}

TEST (ComposeTiming, AQuantileIsTheUpperBoundOfTheBucketHoldingIt)
{
    uint64_t counts[timing::kBuckets] = {};
    EXPECT_EQ (timing::Quantile (counts, 0.5), 0u);
    counts[2] = 9;  // nine frames of at most 0.225 ms
    counts[10] = 1; // and one slow one
    EXPECT_EQ (timing::Quantile (counts, 0.50), 225u);
    EXPECT_EQ (timing::Quantile (counts, 0.90), 225u);
    EXPECT_EQ (timing::Quantile (counts, 0.95), timing::BucketUpperMicros (10));
}

namespace {

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11RenderTargetView> target;
};

bool MakeDevice (Device& made)
{
    if (FAILED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &made.device, nullptr, &made.context)))
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 512;
    desc.Height = 512;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED (made.device->CreateTexture2D (&desc, nullptr, &texture)) &&
           SUCCEEDED (made.device->CreateRenderTargetView (texture.Get (), nullptr, &made.target));
}

} // namespace

TEST (ComposeTiming, FramesOnARealDeviceComeBackTimedAndEachLineIsADelta)
{
    Device gpu;
    ASSERT_TRUE (MakeDevice (gpu)) << "no WARP device";
    timing::ReleaseDeviceObjects (); // a session's start
    const float colour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    constexpr int kFrames = 40;
    for (int frame = 0; frame < kFrames; ++frame) {
        timing::BeginFrame (gpu.context.Get ());
        gpu.context->ClearRenderTargetView (gpu.target.Get (), colour);
        timing::Mark (gpu.context.Get (), timing::Stage::Occluder);
        timing::Mark (gpu.context.Get (), timing::Stage::Host);
        // No caller layers this frame: the Layers stage is stamped with the next mark.
        timing::Mark (gpu.context.Get (), timing::Stage::Guest);
        timing::EndFrame (gpu.context.Get ());
        gpu.context->Flush ();
        ::Sleep (1);
    }
    const timing::Window window = timing::Take ();
    EXPECT_EQ (window.cpuFrames, uint64_t (kFrames));
    EXPECT_EQ (window.intervals, uint64_t (kFrames - 1)) << "the first compose of a session has no interval";
    EXPECT_GT (window.timed, uint64_t (kFrames / 2));
    EXPECT_LE (window.timed + window.untimed, uint64_t (kFrames));
    EXPECT_GE (window.gpuP95Ms, window.gpuP50Ms);
    EXPECT_GE (window.gpuMaxMs, window.stageMaxMs[0]);
    EXPECT_GT (window.intervalP50Ms, 0.0);

    // §7: the next line says only what happened since this one.
    const timing::Window again = timing::Take ();
    EXPECT_EQ (again.cpuFrames, 0u);
    EXPECT_EQ (again.timed, 0u);
    EXPECT_EQ (again.intervals, 0u);
    timing::ReleaseDeviceObjects ();
}

TEST (ComposeTiming, AFrameThatDrawsOnlyTheHudStillComesBack)
{
    // The user's hide: `Compose` begins, the guest draws the HUD, and it ends.
    Device gpu;
    ASSERT_TRUE (MakeDevice (gpu)) << "no WARP device";
    timing::ReleaseDeviceObjects ();
    for (int frame = 0; frame < 12; ++frame) {
        timing::BeginFrame (gpu.context.Get ());
        timing::Mark (gpu.context.Get (), timing::Stage::Guest);
        timing::EndFrame (gpu.context.Get ());
        gpu.context->Flush ();
        ::Sleep (1);
    }
    const timing::Window window = timing::Take ();
    EXPECT_GT (window.timed, 0u);
    EXPECT_EQ (window.cpuFrames, 12u);
    timing::ReleaseDeviceObjects ();
}
