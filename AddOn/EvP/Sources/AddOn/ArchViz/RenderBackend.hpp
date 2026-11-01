#ifndef EVP_ARCHVIZ_RENDERBACKEND_HPP
#define EVP_ARCHVIZ_RENDERBACKEND_HPP

// ArchViz/RenderBackend — WHICH graphics API the viewport runs on, decided from
// facts about the adapters rather than from whatever the engine would pick.
//
// ⚠️ D3D11 IS PERMANENT, NOT A TRANSITION. Older GPUs have no hardware ray
// tracing and will never get it; the viewport must keep opening on them
// unchanged. D3D12 is chosen only where it buys what it exists for -- hardware
// ray tracing -- on an adapter that can be shown to have it. Everything else,
// and every D3D12 failure, falls back to D3D11 and says why.
//
// ⚠️ A SOFTWARE ADAPTER IS NEVER A D3D12 SUCCESS. The first feasibility run
// found the RTX 4070 Ti, failed it inside a GPU-partitioned VM, and the engine
// silently fell back to the Microsoft Basic Render Driver. That is WARP: a
// device that "works" at a fraction of the speed and with software ray-tracing
// caps. So the refusal is on the vendor id as well as the software flag, the
// same rule as DxrDevice::AdapterRefusal, and the CREATED device is checked
// again after creation, because what was asked for is not proof of what came
// back.
//
// ⚠️ THE INJECTED OVERLAY IS NOT A CLIENT OF THIS. It is a guest on Archicad's
// own D3D11 device (OVERLAY-INVARIANTS §12b) and can never move to D3D12; this
// decides only the viewport's own device.
//
// Pure: no Windows, Diligent or DevKit types, so the whole rule is offline
// tested (tests/cpp/test_renderbackend.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {

enum class RenderBackend : uint8_t {
    D3D11 = 0,
    D3D12 = 1,
};

// What the owner asked for. `Auto` is the only route to D3D12.
//
// ⚠️ THE DEFAULT IS D3D11 UNTIL THE A5 COMPARISON PASSES (RE51.D2 acceptance:
// "D3D12 matches D3D11 on the A5 scenes before it becomes the preferred
// backend"). Changing the default is its own commit, made after that run.
enum class RenderBackendPreference : uint8_t {
    D3D11 = 0,
    Auto = 1,
};
constexpr RenderBackendPreference kDefaultRenderBackendPreference = RenderBackendPreference::D3D11;

enum class RenderAdapterKind : uint8_t {
    Unknown = 0,
    Software = 1,
    Integrated = 2,
    Discrete = 3,
};

// PCI vendor id DXGI gives Microsoft's own adapters (WARP, Basic Render Driver).
constexpr uint32_t kMicrosoftAdapterVendorId = 0x1414;

// Every ray-tracing capability the renderer will rely on: standalone shaders,
// inline and indirect (Diligent's RAY_TRACING_CAP_FLAG 0x01 | 0x02 | 0x04).
// The RTX 4070 Ti reported exactly this in RE51.D1.
constexpr uint32_t kRequiredRayTracingCaps = 0x07;

struct RenderAdapterFacts {
    std::string name;
    uint32_t vendorId = 0;
    RenderAdapterKind kind = RenderAdapterKind::Unknown;
    bool rayTracingEnabled = false; // the device feature, not only the caps
    uint32_t rayTracingCaps = 0;
};

struct RenderBackendConstraints {
    // The D3D12 runtime could not be loaded at all.
    bool d3d12Unavailable = false;
    // Something in this start shares a D3D11 resource with another device
    // (the host-composite mirror), so the device must be D3D11.
    bool requiresD3D11Interop = false;
    std::string interopReason;
};

struct RenderBackendChoice {
    RenderBackend backend = RenderBackend::D3D11;
    int adapterIndex = -1; // into the facts list; -1 for D3D11
    std::string adapterName;
    std::string reason; // one line, for the log and DiligentViewportState
};

// Why this adapter may not run the D3D12 viewport, or nullptr when it may.
const char* RenderAdapterRefusal (const RenderAdapterFacts& adapter);

RenderBackendChoice ChooseRenderBackend (RenderBackendPreference preference,
                                         const std::vector<RenderAdapterFacts>& adapters,
                                         const RenderBackendConstraints& constraints);

// What the D3D12 device that came back actually is.
struct CreatedDeviceFacts {
    RenderAdapterFacts adapter;
    // DeviceInfo.NDC.MinZ. Camera.cpp hard-codes the 0..1 depth convention; a
    // device that is not 0..1 inverts the depth test over half the scene.
    float ndcMinZ = 0.0f;
};

// Why the created device must be released in favour of D3D11, or empty when it
// may be kept.
std::string RefuseCreatedD3D12Device (const RenderBackendChoice& choice, const CreatedDeviceFacts& created);

bool ParseRenderBackendPreference (const std::string& text, RenderBackendPreference& out);
const char* RenderBackendPreferenceName (RenderBackendPreference preference);
const char* RenderBackendName (RenderBackend backend);

} // namespace geomsrv::archviz

#endif
