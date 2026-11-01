// ArchViz/RenderBackend — the rule that decides whether the viewport may run on
// D3D12 (RE51.D2).
//
// ⚠️ WHY OFFLINE: every wrong answer here is invisible on the machine that
// matters. The reference GPU qualifies either way, so a rule that let WARP, an
// unknown adapter or a half-capable one through would pass every live run on it
// and only fail on somebody else's older GPU -- as a slow or broken viewport
// rather than as a clean D3D11 fallback.

#include "ArchViz/RenderBackend.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

// Raw values repeated on purpose, so the module renumbering them is caught.
constexpr uint32_t kNvidia = 0x10DE;
constexpr uint32_t kAmd = 0x1002;
constexpr uint32_t kMicrosoft = 0x1414;
constexpr uint32_t kAllCaps = 0x01 | 0x02 | 0x04;

RenderAdapterFacts Rtx4070Ti ()
{
    RenderAdapterFacts adapter;
    adapter.name = "NVIDIA GeForce RTX 4070 Ti";
    adapter.vendorId = kNvidia;
    adapter.kind = RenderAdapterKind::Discrete;
    adapter.rayTracingEnabled = true;
    adapter.rayTracingCaps = kAllCaps;
    return adapter;
}

RenderAdapterFacts Warp ()
{
    RenderAdapterFacts adapter;
    adapter.name = "Microsoft Basic Render Driver";
    adapter.vendorId = kMicrosoft;
    adapter.kind = RenderAdapterKind::Software;
    // WARP does report ray tracing; that is exactly why caps alone are not enough.
    adapter.rayTracingEnabled = true;
    adapter.rayTracingCaps = kAllCaps;
    return adapter;
}

RenderAdapterFacts OldGpu ()
{
    RenderAdapterFacts adapter;
    adapter.name = "NVIDIA GeForce GTX 970";
    adapter.vendorId = kNvidia;
    adapter.kind = RenderAdapterKind::Discrete;
    return adapter;
}

CreatedDeviceFacts Created (const RenderAdapterFacts& adapter)
{
    CreatedDeviceFacts facts;
    facts.adapter = adapter;
    facts.ndcMinZ = 0.0f;
    return facts;
}

} // namespace

// The owner's 2026-11-01 decision: D3D12 by default where it qualifies, D3D11 the
// fallback. A change back is a deliberate decision, and this test makes it one.
TEST (RenderBackend, DefaultPreferenceIsAutoWithD3D11AsFallback)
{
    EXPECT_EQ (kDefaultRenderBackendPreference, RenderBackendPreference::Auto);
    // The default on a machine with no qualifying adapter is still D3D11.
    EXPECT_EQ (ChooseRenderBackend (kDefaultRenderBackendPreference, {}, {}).backend, RenderBackend::D3D11);
}

TEST (RenderBackend, D3D11PreferenceNeverLooksAtAdapters)
{
    const RenderBackendChoice choice = ChooseRenderBackend (RenderBackendPreference::D3D11, { Rtx4070Ti () }, {});
    EXPECT_EQ (choice.backend, RenderBackend::D3D11);
    EXPECT_EQ (choice.adapterIndex, -1);
    EXPECT_EQ (choice.reason, "preference d3d11");
}

TEST (RenderBackend, AutoPicksAQualifiedHardwareAdapter)
{
    const RenderBackendChoice choice = ChooseRenderBackend (RenderBackendPreference::Auto, { Rtx4070Ti () }, {});
    EXPECT_EQ (choice.backend, RenderBackend::D3D12);
    EXPECT_EQ (choice.adapterIndex, 0);
    EXPECT_EQ (choice.adapterName, "NVIDIA GeForce RTX 4070 Ti");
    EXPECT_NE (choice.reason.find ("0x07"), std::string::npos) << choice.reason;
}

TEST (RenderBackend, AutoSkipsWarpAndPicksTheHardwareAdapterAfterIt)
{
    const RenderBackendChoice choice =
        ChooseRenderBackend (RenderBackendPreference::Auto, { Warp (), Rtx4070Ti () }, {});
    EXPECT_EQ (choice.backend, RenderBackend::D3D12);
    EXPECT_EQ (choice.adapterIndex, 1);
}

TEST (RenderBackend, WarpAloneFallsBackToD3D11)
{
    const RenderBackendChoice choice = ChooseRenderBackend (RenderBackendPreference::Auto, { Warp () }, {});
    EXPECT_EQ (choice.backend, RenderBackend::D3D11);
    EXPECT_NE (choice.reason.find ("software"), std::string::npos) << choice.reason;
}

TEST (RenderBackend, MicrosoftVendorIsRefusedEvenWithoutTheSoftwareFlag)
{
    RenderAdapterFacts basic = Warp ();
    basic.kind = RenderAdapterKind::Discrete; // a flag-only filter let this through (#57)
    EXPECT_NE (RenderAdapterRefusal (basic), nullptr);
    EXPECT_EQ (ChooseRenderBackend (RenderBackendPreference::Auto, { basic }, {}).backend, RenderBackend::D3D11);
}

TEST (RenderBackend, UnknownAdapterTypeIsRefused)
{
    RenderAdapterFacts unknown = Rtx4070Ti ();
    unknown.kind = RenderAdapterKind::Unknown;
    EXPECT_NE (RenderAdapterRefusal (unknown), nullptr);
}

TEST (RenderBackend, OlderGpuWithoutRayTracingFallsBackToD3D11)
{
    const RenderBackendChoice choice = ChooseRenderBackend (RenderBackendPreference::Auto, { OldGpu () }, {});
    EXPECT_EQ (choice.backend, RenderBackend::D3D11);
    EXPECT_NE (choice.reason.find ("GTX 970"), std::string::npos) << choice.reason;
}

TEST (RenderBackend, EveryRequiredCapabilityIsRequired)
{
    for (uint32_t caps : { 0x00u, 0x01u, 0x03u, 0x05u, 0x06u }) {
        RenderAdapterFacts partial = Rtx4070Ti ();
        partial.rayTracingCaps = caps;
        EXPECT_NE (RenderAdapterRefusal (partial), nullptr) << "caps " << caps;
    }
    RenderAdapterFacts extra = Rtx4070Ti ();
    extra.rayTracingCaps = kAllCaps | 0x80; // a future flag does not disqualify
    EXPECT_EQ (RenderAdapterRefusal (extra), nullptr);
}

TEST (RenderBackend, CapsWithoutTheEnabledFeatureAreRefused)
{
    RenderAdapterFacts adapter = Rtx4070Ti ();
    adapter.rayTracingEnabled = false;
    EXPECT_NE (RenderAdapterRefusal (adapter), nullptr);
}

TEST (RenderBackend, IntegratedAdapterWithFullRayTracingQualifies)
{
    RenderAdapterFacts integrated = Rtx4070Ti ();
    integrated.name = "Intel Arc integrated";
    integrated.vendorId = 0x8086;
    integrated.kind = RenderAdapterKind::Integrated;
    EXPECT_EQ (RenderAdapterRefusal (integrated), nullptr);
}

TEST (RenderBackend, NoAdaptersFallsBackToD3D11)
{
    const RenderBackendChoice choice = ChooseRenderBackend (RenderBackendPreference::Auto, {}, {});
    EXPECT_EQ (choice.backend, RenderBackend::D3D11);
    EXPECT_NE (choice.reason.find ("none enumerated"), std::string::npos) << choice.reason;
}

TEST (RenderBackend, MissingRuntimeFallsBackToD3D11)
{
    RenderBackendConstraints constraints;
    constraints.d3d12Unavailable = true;
    EXPECT_EQ (ChooseRenderBackend (RenderBackendPreference::Auto, { Rtx4070Ti () }, constraints).backend,
               RenderBackend::D3D11);
}

TEST (RenderBackend, AD3D11InteropKeepsD3D11AndNamesIt)
{
    RenderBackendConstraints constraints;
    constraints.requiresD3D11Interop = true;
    constraints.interopReason = "the host-composite mirror";
    const RenderBackendChoice choice =
        ChooseRenderBackend (RenderBackendPreference::Auto, { Rtx4070Ti () }, constraints);
    EXPECT_EQ (choice.backend, RenderBackend::D3D11);
    EXPECT_NE (choice.reason.find ("host-composite mirror"), std::string::npos) << choice.reason;
}

TEST (RenderBackend, TheCreatedDeviceIsCheckedAgain)
{
    const RenderBackendChoice choice = ChooseRenderBackend (RenderBackendPreference::Auto, { Rtx4070Ti () }, {});
    EXPECT_TRUE (RefuseCreatedD3D12Device (choice, Created (Rtx4070Ti ())).empty ());

    // RE51.D1's 2026-08-22 run: asked for the RTX, the engine handed back WARP.
    EXPECT_FALSE (RefuseCreatedD3D12Device (choice, Created (Warp ())).empty ());

    RenderAdapterFacts other = Rtx4070Ti ();
    other.name = "AMD Radeon RX 7900";
    other.vendorId = kAmd;
    EXPECT_FALSE (RefuseCreatedD3D12Device (choice, Created (other)).empty ());

    CreatedDeviceFacts glDepth = Created (Rtx4070Ti ());
    glDepth.ndcMinZ = -1.0f;
    EXPECT_FALSE (RefuseCreatedD3D12Device (choice, glDepth).empty ());
}

TEST (RenderBackend, PreferenceNamesRoundTripAndUnknownIsRejected)
{
    for (RenderBackendPreference preference : { RenderBackendPreference::D3D11, RenderBackendPreference::Auto }) {
        RenderBackendPreference parsed = RenderBackendPreference::D3D11;
        ASSERT_TRUE (ParseRenderBackendPreference (RenderBackendPreferenceName (preference), parsed));
        EXPECT_EQ (parsed, preference);
    }
    RenderBackendPreference untouched = RenderBackendPreference::Auto;
    EXPECT_FALSE (ParseRenderBackendPreference ("d3d12", untouched)); // never a forced D3D12
    EXPECT_FALSE (ParseRenderBackendPreference ("D3D11", untouched));
    EXPECT_EQ (untouched, RenderBackendPreference::Auto);
    EXPECT_STREQ (RenderBackendName (RenderBackend::D3D11), "d3d11");
    EXPECT_STREQ (RenderBackendName (RenderBackend::D3D12), "d3d12");
}
