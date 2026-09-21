#ifndef EVP_ARCHVIZ_DXGI_PASSPROVENANCE_HPP
#define EVP_ARCHVIZ_DXGI_PASSPROVENANCE_HPP

// PASS_PROVENANCE answers one question: did the camera snapshot and the image
// reaching Present come from the same scene pass? It observes resource identity
// only. It does not gate composition, choose a camera, or change draw state.

#include <cstddef>
#include <cstdint>

struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace passprovenance {

constexpr size_t kRowCapacity = 1024;
constexpr size_t kResourceAmbiguityReasonCount = 7;

enum class Relation : uint32_t { Unknown = 0, Match = 1, Mismatch = 2, Ambiguous = 3 };
enum class ResourceState : uint32_t { Unknown = 0, Known = 1, Ambiguous = 2 };
enum class ResourceAmbiguityReason : uint32_t {
    NonCameraDraw = 1u << 0,
    SampledAmbiguous = 1u << 1,
    ConflictingSampledPasses = 1u << 2,
    PartialCopy = 1u << 3,
    ResourceWrite = 1u << 4,
    UnsupportedGpuWork = 1u << 5,
    SecondaryCameraTarget = 1u << 6
};

constexpr uint32_t ReasonMask (ResourceAmbiguityReason reason)
{
    return uint32_t (reason);
}

struct Row {
    uint64_t present = 0;
    uint64_t imagePass = 0;
    uint64_t cameraPass = 0;
    uint64_t cameraHash = 0;
    uint64_t backBuffer = 0;
    int64_t delta = 0; // cameraPass - imagePass; meaningful only for Match/Mismatch
    Relation relation = Relation::Unknown;
    ResourceState resourceState = ResourceState::Unknown;
    uint32_t resourceAmbiguityMask = 0;
    bool presentContextOverlap = false;
};

struct Stats {
    bool enabled = false;
    bool hookInstalled = false;
    bool contextHookInstalled = false;
    bool presentHookInstalled = false;
    bool srvHookEnabled = false;
    uint32_t contextThreadId = 0;
    uint32_t presentThreadId = 0;
    uint64_t presents = 0;
    uint64_t matched = 0;
    uint64_t mismatched = 0;
    uint64_t unknown = 0;
    uint64_t ambiguous = 0;
    uint64_t backBufferFailures = 0;
    uint64_t resourceTableOverflows = 0;
    uint64_t renderThreadViolations = 0;
    uint64_t unsupportedGpuWork = 0;
    uint64_t snapshotDrainTimeouts = 0;
    uint64_t contextHookRepairs = 0;
    uint64_t rowsOverwritten = 0;
    uint64_t firstAmbiguityTransitions[kResourceAmbiguityReasonCount] = {};
    uint64_t resourceAmbiguousPresents = 0;
    uint64_t presentContextOverlaps = 0;
    uint32_t contextSlotsPatched = 0;
    bool resourceResetPending = false;
};

// MAIN THREAD. Enabling also gates the otherwise-hot PSSetShaderResources slot.
bool SetEnabled (bool enabled);
bool Enabled ();
void Reset ();

// CONTEXT THREAD, from context detours. Present may run on a separate stable
// thread; resource entries are published atomically across that boundary. The
// view/resource references obtained here are released before return.
void OnRenderTargets (uint32_t count, ID3D11RenderTargetView* const* targets);
void OnPSShaderResources (uint32_t startSlot, uint32_t count, ID3D11ShaderResourceView* const* views);
void OnClearRenderTarget (ID3D11RenderTargetView* target);
void OnCameraSnapshot (uint64_t scenePassGeneration);
void OnDrawCompleted ();
void OnCopyResource (ID3D11Resource* destination, ID3D11Resource* source);
void OnPartialResourceCopy (ID3D11Resource* destination, ID3D11Resource* source);
void OnResourceWrite (ID3D11Resource* resource);
void OnUnsupportedGpuWork ();
bool BeginContextOperation ();
void EndContextOperation (bool active);

// PRESENT THREAD. The interval spans the pre-overlay sample through completion
// of the real Present so concurrent host GPU work cannot be called conclusive.
bool BeginPresent (IDXGISwapChain* swapChain, uint64_t cameraHash);
void EndPresent (bool active, bool succeeded);
void OnResizeBuffers ();

// MAIN THREAD. Copies the newest rows in chronological order.
size_t CopyRows (Row* out, size_t capacity);
Stats GetStats ();

} // namespace passprovenance
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
