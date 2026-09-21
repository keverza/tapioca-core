// ArchViz/Dxgi/PassProvenance -- diagnostic-only pass lineage.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: this file
// observes the established camera snapshot and Present points and changes neither.

#include "ArchViz/Dxgi/PassProvenance.hpp"

#include "ArchViz/Dxgi/ContextHook.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace passprovenance {

namespace {

constexpr size_t kShaderResourceSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr size_t kRenderTargetSlots = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr size_t kResourceCapacity = 512;

constexpr uint64_t kEnabledBit = uint64_t (1) << 63;
constexpr uint64_t kCallbackCountMask = ~kEnabledBit;

struct ResourceEntry {
    std::atomic<uint64_t> version { 0 };
    std::atomic<uint64_t> resource { 0 };
    std::atomic<uint64_t> scenePass { 0 };
    std::atomic<uint32_t> state { uint32_t (ResourceState::Unknown) };
    std::atomic<uint32_t> ambiguityMask { 0 };
};

struct ResourceSnapshot {
    uint64_t resource = 0;
    uint64_t scenePass = 0;
    ResourceState state = ResourceState::Unknown;
    uint32_t ambiguityMask = 0;
};

std::atomic<uint64_t> g_callbackGate { 0 };
std::atomic<uint64_t> g_resetRequested { 0 };
std::atomic<uint64_t> g_contextResetApplied { 0 };
std::atomic<uint64_t> g_presentResetApplied { 0 };
std::atomic<uint64_t> g_resourceResetRequested { 0 };
std::atomic<uint64_t> g_resourceResetApplied { 0 };
std::atomic<uint32_t> g_contextThread { 0 };
std::atomic<uint32_t> g_presentThread { 0 };
std::atomic<uint64_t> g_cameraPublicationVersion { 0 };
std::atomic<uint64_t> g_cameraPass { 0 };
std::atomic<uint64_t> g_contextOperationVersion { 0 };
std::atomic<uint32_t> g_contextOperations { 0 };

ResourceEntry g_resources[kResourceCapacity];
uint64_t g_shaderResources[kShaderResourceSlots] = {};
uint64_t g_renderTargets[kRenderTargetSlots] = {};
uint64_t g_pendingCameraTarget = 0;
uint64_t g_pendingCameraPass = 0;

struct RowSlot {
    std::atomic<uint64_t> published { 0 };
    std::atomic<uint64_t> present { 0 };
    std::atomic<uint64_t> imagePass { 0 };
    std::atomic<uint64_t> cameraPass { 0 };
    std::atomic<uint64_t> cameraHash { 0 };
    std::atomic<uint64_t> backBuffer { 0 };
    std::atomic<int64_t> delta { 0 };
    std::atomic<uint32_t> relation { uint32_t (Relation::Unknown) };
    std::atomic<uint32_t> resourceState { uint32_t (ResourceState::Unknown) };
    std::atomic<uint32_t> resourceAmbiguityMask { 0 };
    std::atomic<bool> presentContextOverlap { false };
};

RowSlot g_rows[kRowCapacity];
std::atomic<uint64_t> g_rowsWritten { 0 };
std::atomic<uint64_t> g_rowsVisibleFrom { 0 };

std::atomic<uint64_t> g_presents { 0 };
std::atomic<uint64_t> g_matched { 0 };
std::atomic<uint64_t> g_mismatched { 0 };
std::atomic<uint64_t> g_unknown { 0 };
std::atomic<uint64_t> g_ambiguous { 0 };
std::atomic<uint64_t> g_backBufferFailures { 0 };
std::atomic<uint64_t> g_resourceTableOverflows { 0 };
std::atomic<uint64_t> g_renderThreadViolations { 0 };
std::atomic<uint64_t> g_unsupportedGpuWork { 0 };
std::atomic<uint64_t> g_snapshotDrainTimeouts { 0 };
std::atomic<uint64_t> g_contextHookRepairsAtStart { 0 };
std::atomic<uint64_t> g_firstAmbiguityTransitions[kResourceAmbiguityReasonCount];
std::atomic<uint64_t> g_resourceAmbiguousPresents { 0 };
std::atomic<uint64_t> g_presentContextOverlaps { 0 };

Row g_pendingPresentRow;
uint64_t g_pendingContextOperationVersion = 0;
bool g_pendingContextOverlap = false;
bool g_pendingDiscardPresent = false;
uint64_t g_cachedSwapChain = 0;
DXGI_SWAP_EFFECT g_cachedSwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;

bool EnterCallback ()
{
    uint64_t state = g_callbackGate.load (std::memory_order_acquire);
    while ((state & kEnabledBit) != 0) {
        if ((state & kCallbackCountMask) == kCallbackCountMask)
            return false;
        if (g_callbackGate.compare_exchange_weak (state, state + 1, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
            return true;
    }
    return false;
}

void LeaveCallback ()
{
    g_callbackGate.fetch_sub (1, std::memory_order_release);
}

class CallbackGuard {
  public:
    CallbackGuard () : entered (EnterCallback ())
    {
    }
    ~CallbackGuard ()
    {
        if (entered)
            LeaveCallback ();
    }
    explicit operator bool () const
    {
        return entered;
    }

  private:
    bool entered;
};

void WriteResource (ResourceEntry& entry, uint64_t resource, uint64_t scenePass, ResourceState state,
                    uint32_t ambiguityMask = 0)
{
    entry.version.fetch_add (1, std::memory_order_acq_rel);
    entry.resource.store (resource, std::memory_order_relaxed);
    entry.scenePass.store (scenePass, std::memory_order_relaxed);
    entry.state.store (uint32_t (state), std::memory_order_relaxed);
    entry.ambiguityMask.store (ambiguityMask, std::memory_order_relaxed);
    entry.version.fetch_add (1, std::memory_order_release);
}

bool ReadResourceEntry (const ResourceEntry& entry, ResourceSnapshot& snapshot)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = entry.version.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        ResourceSnapshot candidate;
        candidate.resource = entry.resource.load (std::memory_order_relaxed);
        candidate.scenePass = entry.scenePass.load (std::memory_order_relaxed);
        candidate.state = ResourceState (entry.state.load (std::memory_order_relaxed));
        candidate.ambiguityMask = entry.ambiguityMask.load (std::memory_order_relaxed);
        const uint64_t after = entry.version.load (std::memory_order_acquire);
        if (before == after) {
            snapshot = candidate;
            return true;
        }
    }
    return false;
}

void ClearResourceTable ()
{
    for (ResourceEntry& entry : g_resources) {
        if (entry.resource.load (std::memory_order_relaxed) != 0)
            WriteResource (entry, 0, 0, ResourceState::Unknown);
    }
}

void ClearContextState ()
{
    ClearResourceTable ();
    std::memset (g_shaderResources, 0, sizeof (g_shaderResources));
    std::memset (g_renderTargets, 0, sizeof (g_renderTargets));
    g_pendingCameraTarget = 0;
    g_pendingCameraPass = 0;
}

void ClearPublishedLineage ()
{
    g_cameraPublicationVersion.fetch_add (1, std::memory_order_acq_rel);
    ClearContextState ();
    g_cameraPass.store (0, std::memory_order_relaxed);
    g_cameraPublicationVersion.fetch_add (1, std::memory_order_release);
}

void ClearPublishedResources ()
{
    g_cameraPublicationVersion.fetch_add (1, std::memory_order_acq_rel);
    ClearResourceTable ();
    g_pendingCameraTarget = 0;
    g_pendingCameraPass = 0;
    g_cameraPass.store (0, std::memory_order_relaxed);
    g_cameraPublicationVersion.fetch_add (1, std::memory_order_release);
}

void ApplyContextResetIfNeeded ()
{
    const uint64_t requested = g_resetRequested.load (std::memory_order_acquire);
    if (requested == g_contextResetApplied.load (std::memory_order_relaxed))
        return;
    const uint64_t resourceRequested = g_resourceResetRequested.load (std::memory_order_acquire);
    ClearPublishedLineage ();
    g_resourceTableOverflows.store (0, std::memory_order_relaxed);
    g_unsupportedGpuWork.store (0, std::memory_order_relaxed);
    g_resourceResetApplied.store (resourceRequested, std::memory_order_release);
    g_contextResetApplied.store (requested, std::memory_order_release);
}

void ApplyResourceResetIfNeeded ()
{
    const uint64_t requested = g_resourceResetRequested.load (std::memory_order_acquire);
    if (requested == g_resourceResetApplied.load (std::memory_order_relaxed))
        return;
    ClearPublishedResources ();
    g_resourceResetApplied.store (requested, std::memory_order_release);
}

void ResetPresentRowsAndStats ()
{
    g_rowsVisibleFrom.store (g_rowsWritten.load (std::memory_order_acquire), std::memory_order_release);
    g_presents.store (0, std::memory_order_relaxed);
    g_matched.store (0, std::memory_order_relaxed);
    g_mismatched.store (0, std::memory_order_relaxed);
    g_unknown.store (0, std::memory_order_relaxed);
    g_ambiguous.store (0, std::memory_order_relaxed);
    g_backBufferFailures.store (0, std::memory_order_relaxed);
    g_resourceAmbiguousPresents.store (0, std::memory_order_relaxed);
    g_presentContextOverlaps.store (0, std::memory_order_relaxed);
    g_cachedSwapChain = 0;
}

bool ApplyPresentResetIfReady ()
{
    const uint64_t requested = g_resetRequested.load (std::memory_order_acquire);
    if (g_contextResetApplied.load (std::memory_order_acquire) != requested)
        return false;
    if (g_presentResetApplied.load (std::memory_order_relaxed) == requested)
        return true;
    ResetPresentRowsAndStats ();
    g_presentResetApplied.store (requested, std::memory_order_release);
    return true;
}

bool EnterOwnedThread (std::atomic<uint32_t>& owner)
{
    const uint32_t current = ::GetCurrentThreadId ();
    uint32_t expected = owner.load (std::memory_order_acquire);
    if (expected == 0) {
        owner.compare_exchange_strong (expected, current, std::memory_order_acq_rel);
        expected = owner.load (std::memory_order_acquire);
    }
    if (expected != current) {
        g_renderThreadViolations.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool EnterContextThread ()
{
    if (!EnterOwnedThread (g_contextThread))
        return false;
    ApplyContextResetIfNeeded ();
    ApplyResourceResetIfNeeded ();
    return true;
}

bool EnterPresentThread ()
{
    return EnterOwnedThread (g_presentThread) && ApplyPresentResetIfReady ();
}

bool ReadResource (uint64_t resource, ResourceSnapshot& snapshot);

bool ReadCameraAndResource (uint64_t resource, uint64_t& cameraPass, ResourceSnapshot& snapshot, bool& hasResource)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = g_cameraPublicationVersion.load (std::memory_order_acquire);
        if ((before & 1u) != 0)
            continue;
        const uint64_t candidateCamera = g_cameraPass.load (std::memory_order_relaxed);
        ResourceSnapshot candidateResource;
        const bool candidateHasResource = ReadResource (resource, candidateResource);
        const uint64_t after = g_cameraPublicationVersion.load (std::memory_order_acquire);
        if (before == after) {
            cameraPass = candidateCamera;
            snapshot = candidateResource;
            hasResource = candidateHasResource;
            return true;
        }
    }
    cameraPass = 0;
    hasResource = false;
    return false;
}

uint64_t ResourceBehind (ID3D11View* view)
{
    if (view == nullptr)
        return 0;
    ID3D11Resource* resource = nullptr;
    view->GetResource (&resource);
    if (resource == nullptr)
        return 0;
    const uint64_t identity = uint64_t (uintptr_t (resource));
    resource->Release ();
    return identity;
}

size_t ResourceSlot (uint64_t resource)
{
    return size_t ((resource >> 4) ^ (resource >> 13)) % kResourceCapacity;
}

ResourceEntry* FindResource (uint64_t resource, bool insert)
{
    if (resource == 0)
        return nullptr;
    const size_t start = ResourceSlot (resource);
    for (size_t offset = 0; offset < kResourceCapacity; ++offset) {
        ResourceEntry& entry = g_resources[(start + offset) % kResourceCapacity];
        const uint64_t stored = entry.resource.load (std::memory_order_relaxed);
        if (stored == resource)
            return &entry;
        if (stored == 0) {
            if (!insert)
                return nullptr;
            WriteResource (entry, resource, 0, ResourceState::Unknown);
            return &entry;
        }
    }
    if (insert)
        g_resourceTableOverflows.fetch_add (1, std::memory_order_relaxed);
    return nullptr;
}

bool ReadResource (uint64_t resource, ResourceSnapshot& snapshot)
{
    if (resource == 0)
        return false;
    const size_t start = ResourceSlot (resource);
    for (size_t offset = 0; offset < kResourceCapacity; ++offset) {
        ResourceSnapshot candidate;
        if (!ReadResourceEntry (g_resources[(start + offset) % kResourceCapacity], candidate))
            continue;
        if (candidate.resource == resource) {
            snapshot = candidate;
            return true;
        }
    }
    return false;
}

void StampKnown (uint64_t resource, uint64_t scenePass)
{
    if (scenePass == 0)
        return;
    ResourceEntry* entry = FindResource (resource, true);
    if (entry == nullptr)
        return;
    WriteResource (*entry, resource, scenePass, ResourceState::Known);
}

void CountFirstAmbiguityTransitions (uint32_t ambiguityMask)
{
    for (size_t i = 0; i < kResourceAmbiguityReasonCount; ++i) {
        if ((ambiguityMask & (1u << i)) != 0)
            g_firstAmbiguityTransitions[i].fetch_add (1, std::memory_order_relaxed);
    }
}

void StampAmbiguous (ResourceEntry& entry, uint64_t resource, uint32_t ambiguityMask)
{
    const ResourceState previous = ResourceState (entry.state.load (std::memory_order_relaxed));
    if (previous == ResourceState::Known)
        CountFirstAmbiguityTransitions (ambiguityMask);
    else if (previous == ResourceState::Ambiguous)
        ambiguityMask |= entry.ambiguityMask.load (std::memory_order_relaxed);
    WriteResource (entry, resource, 0, ResourceState::Ambiguous, ambiguityMask);
}

void StampAmbiguous (uint64_t resource, uint32_t ambiguityMask)
{
    ResourceEntry* entry = FindResource (resource, true);
    if (entry != nullptr)
        StampAmbiguous (*entry, resource, ambiguityMask);
}

void StampUnknown (uint64_t resource)
{
    ResourceEntry* entry = FindResource (resource, false);
    if (entry == nullptr)
        return;
    WriteResource (*entry, resource, 0, ResourceState::Unknown);
}

void Publish (const Row& row)
{
    const uint64_t written = g_rowsWritten.fetch_add (1, std::memory_order_acq_rel);
    RowSlot& slot = g_rows[written % kRowCapacity];
    slot.published.store (0, std::memory_order_release);
    slot.present.store (row.present, std::memory_order_relaxed);
    slot.imagePass.store (row.imagePass, std::memory_order_relaxed);
    slot.cameraPass.store (row.cameraPass, std::memory_order_relaxed);
    slot.cameraHash.store (row.cameraHash, std::memory_order_relaxed);
    slot.backBuffer.store (row.backBuffer, std::memory_order_relaxed);
    slot.delta.store (row.delta, std::memory_order_relaxed);
    slot.relation.store (uint32_t (row.relation), std::memory_order_relaxed);
    slot.resourceState.store (uint32_t (row.resourceState), std::memory_order_relaxed);
    slot.resourceAmbiguityMask.store (row.resourceAmbiguityMask, std::memory_order_relaxed);
    slot.presentContextOverlap.store (row.presentContextOverlap, std::memory_order_relaxed);
    slot.published.store (written + 1, std::memory_order_release);
}

void CountAndPublish (Row& row)
{
    row.present = g_presents.fetch_add (1, std::memory_order_relaxed) + 1;
    if (row.relation == Relation::Match)
        g_matched.fetch_add (1, std::memory_order_relaxed);
    else if (row.relation == Relation::Mismatch)
        g_mismatched.fetch_add (1, std::memory_order_relaxed);
    else if (row.relation == Relation::Ambiguous)
        g_ambiguous.fetch_add (1, std::memory_order_relaxed);
    else
        g_unknown.fetch_add (1, std::memory_order_relaxed);
    if (row.resourceState == ResourceState::Ambiguous)
        g_resourceAmbiguousPresents.fetch_add (1, std::memory_order_relaxed);
    if (row.presentContextOverlap)
        g_presentContextOverlaps.fetch_add (1, std::memory_order_relaxed);
    Publish (row);
}

} // namespace

bool SetEnabled (bool enabled)
{
    if (enabled) {
        if (!Enabled ()) {
            if ((g_callbackGate.load (std::memory_order_acquire) & kCallbackCountMask) != 0) {
                g_snapshotDrainTimeouts.fetch_add (1, std::memory_order_relaxed);
                return false;
            }
            g_contextThread.store (0, std::memory_order_release);
            g_presentThread.store (0, std::memory_order_release);
            g_renderThreadViolations.store (0, std::memory_order_relaxed);
            g_snapshotDrainTimeouts.store (0, std::memory_order_relaxed);
            g_contextHookRepairsAtStart.store (GetContextHookStats ().repairs, std::memory_order_relaxed);
            Reset ();
        }
        g_callbackGate.fetch_or (kEnabledBit, std::memory_order_release);
        SetContextSlotEnabled (ContextSlot::PSSetShaderResources, true);
        return true;
    }
    else {
        g_callbackGate.fetch_and (kCallbackCountMask, std::memory_order_acq_rel);
        SetContextSlotEnabled (ContextSlot::PSSetShaderResources, false);
        int attempt = 0;
        for (; attempt < 1000 && (g_callbackGate.load (std::memory_order_acquire) & kCallbackCountMask) != 0; ++attempt)
            ::Sleep (1);
        if ((g_callbackGate.load (std::memory_order_acquire) & kCallbackCountMask) != 0) {
            g_snapshotDrainTimeouts.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
}

bool Enabled ()
{
    return (g_callbackGate.load (std::memory_order_acquire) & kEnabledBit) != 0;
}

void Reset ()
{
    const uint64_t requested = g_resetRequested.fetch_add (1, std::memory_order_acq_rel) + 1;
    ResetPresentRowsAndStats ();
    g_resourceTableOverflows.store (0, std::memory_order_relaxed);
    g_unsupportedGpuWork.store (0, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& count : g_firstAmbiguityTransitions)
        count.store (0, std::memory_order_relaxed);
    g_presentResetApplied.store (requested, std::memory_order_release);
}

bool BeginContextOperation ()
{
    if (!EnterCallback ())
        return false;
    if (!EnterContextThread ()) {
        LeaveCallback ();
        return false;
    }
    g_contextOperations.fetch_add (1, std::memory_order_acq_rel);
    g_contextOperationVersion.fetch_add (1, std::memory_order_release);
    return true;
}

void EndContextOperation (bool active)
{
    if (!active)
        return;
    g_contextOperationVersion.fetch_add (1, std::memory_order_release);
    g_contextOperations.fetch_sub (1, std::memory_order_release);
    LeaveCallback ();
}

void OnRenderTargets (uint32_t count, ID3D11RenderTargetView* const* targets)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    std::memset (g_renderTargets, 0, sizeof (g_renderTargets));
    const uint32_t tracked = count < kRenderTargetSlots ? count : uint32_t (kRenderTargetSlots);
    for (uint32_t i = 0; i < tracked; ++i)
        g_renderTargets[i] = ResourceBehind (targets != nullptr ? targets[i] : nullptr);

    // D3D11 nulls any input aliasing an output. Mirror that implicit hazard
    // resolution so a stale requested SRV cannot create lineage on a later draw.
    for (uint64_t target : g_renderTargets) {
        if (target == 0)
            continue;
        for (uint64_t& shaderResource : g_shaderResources) {
            if (shaderResource == target)
                shaderResource = 0;
        }
    }
}

void OnPSShaderResources (uint32_t startSlot, uint32_t count, ID3D11ShaderResourceView* const* views)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = startSlot + i;
        if (slot >= kShaderResourceSlots)
            break;
        uint64_t resource = ResourceBehind (views != nullptr ? views[i] : nullptr);
        for (uint64_t target : g_renderTargets) {
            if (resource != 0 && resource == target) {
                resource = 0;
                break;
            }
        }
        g_shaderResources[slot] = resource;
    }
}

void OnClearRenderTarget (ID3D11RenderTargetView* target)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    StampUnknown (ResourceBehind (target));
}

void OnCameraSnapshot (uint64_t scenePassGeneration)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    g_pendingCameraTarget = g_renderTargets[0];
    g_pendingCameraPass = scenePassGeneration;
}

void OnDrawCompleted ()
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;

    if (g_pendingCameraPass != 0) {
        if (g_pendingCameraTarget == g_renderTargets[0]) {
            g_cameraPublicationVersion.fetch_add (1, std::memory_order_acq_rel);
            ResourceEntry* targetEntry = FindResource (g_pendingCameraTarget, false);
            if (targetEntry == nullptr ||
                ResourceState (targetEntry->state.load (std::memory_order_relaxed)) != ResourceState::Ambiguous)
                StampKnown (g_pendingCameraTarget, g_pendingCameraPass);
            g_cameraPass.store (g_pendingCameraPass, std::memory_order_relaxed);
            g_cameraPublicationVersion.fetch_add (1, std::memory_order_release);
        }
        for (size_t i = 1; i < kRenderTargetSlots; ++i) {
            if (g_renderTargets[i] != 0)
                StampAmbiguous (g_renderTargets[i], ReasonMask (ResourceAmbiguityReason::SecondaryCameraTarget));
        }
        g_pendingCameraTarget = 0;
        g_pendingCameraPass = 0;
        return;
    }

    uint64_t sampledPass = 0;
    bool sampledAmbiguous = false;
    uint32_t drawAmbiguityMask = ReasonMask (ResourceAmbiguityReason::NonCameraDraw);
    for (uint64_t resource : g_shaderResources) {
        ResourceEntry* entry = FindResource (resource, false);
        if (entry == nullptr)
            continue;
        const ResourceState state = ResourceState (entry->state.load (std::memory_order_relaxed));
        if (state == ResourceState::Unknown)
            continue;
        if (state == ResourceState::Ambiguous) {
            sampledAmbiguous = true;
            drawAmbiguityMask |= ReasonMask (ResourceAmbiguityReason::SampledAmbiguous);
            drawAmbiguityMask |= entry->ambiguityMask.load (std::memory_order_relaxed);
            break;
        }
        const uint64_t scenePass = entry->scenePass.load (std::memory_order_relaxed);
        if (sampledPass == 0)
            sampledPass = scenePass;
        else if (sampledPass != scenePass) {
            sampledAmbiguous = true;
            drawAmbiguityMask |= ReasonMask (ResourceAmbiguityReason::ConflictingSampledPasses);
            break;
        }
    }

    for (uint64_t target : g_renderTargets) {
        ResourceEntry* targetEntry = FindResource (target, false);
        if (sampledAmbiguous || sampledPass != 0 ||
            (targetEntry != nullptr &&
             ResourceState (targetEntry->state.load (std::memory_order_relaxed)) == ResourceState::Known)) {
            // A bound SRV is only a candidate input: without shader reflection
            // the hook cannot prove what a non-camera draw changed. Every bound
            // colour output is therefore tainted rather than retaining stale proof.
            StampAmbiguous (target, drawAmbiguityMask);
        }
    }
}

void OnCopyResource (ID3D11Resource* destination, ID3D11Resource* source)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    const uint64_t destinationId = uint64_t (uintptr_t (destination));
    ResourceEntry* sourceEntry = FindResource (uint64_t (uintptr_t (source)), false);
    const ResourceState sourceState = sourceEntry == nullptr
                                          ? ResourceState::Unknown
                                          : ResourceState (sourceEntry->state.load (std::memory_order_relaxed));
    if (sourceState == ResourceState::Unknown)
        StampUnknown (destinationId);
    else if (sourceState == ResourceState::Known)
        StampKnown (destinationId, sourceEntry->scenePass.load (std::memory_order_relaxed));
    else
        StampAmbiguous (destinationId, sourceEntry->ambiguityMask.load (std::memory_order_relaxed));
}

void OnPartialResourceCopy (ID3D11Resource* destination, ID3D11Resource* source)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    const uint64_t destinationId = uint64_t (uintptr_t (destination));
    ResourceEntry* destinationEntry = FindResource (destinationId, false);
    ResourceEntry* sourceEntry = FindResource (uint64_t (uintptr_t (source)), false);
    const bool hasLineage =
        (destinationEntry != nullptr &&
         ResourceState (destinationEntry->state.load (std::memory_order_relaxed)) != ResourceState::Unknown) ||
        (sourceEntry != nullptr &&
         ResourceState (sourceEntry->state.load (std::memory_order_relaxed)) != ResourceState::Unknown);
    if (hasLineage) {
        uint32_t ambiguityMask = ReasonMask (ResourceAmbiguityReason::PartialCopy);
        if (sourceEntry != nullptr)
            ambiguityMask |= sourceEntry->ambiguityMask.load (std::memory_order_relaxed);
        StampAmbiguous (destinationId, ambiguityMask);
    }
    else
        StampUnknown (destinationId);
}

void OnResourceWrite (ID3D11Resource* resource)
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    const uint64_t resourceId = uint64_t (uintptr_t (resource));
    ResourceEntry* entry = FindResource (resourceId, false);
    if (entry != nullptr && ResourceState (entry->state.load (std::memory_order_relaxed)) != ResourceState::Unknown)
        StampAmbiguous (resourceId, ReasonMask (ResourceAmbiguityReason::ResourceWrite));
}

void OnUnsupportedGpuWork ()
{
    CallbackGuard guard;
    if (!guard || !EnterContextThread ())
        return;
    for (ResourceEntry& entry : g_resources) {
        if (ResourceState (entry.state.load (std::memory_order_relaxed)) != ResourceState::Known)
            continue;
        const uint64_t resource = entry.resource.load (std::memory_order_relaxed);
        if (resource != 0)
            StampAmbiguous (entry, resource, ReasonMask (ResourceAmbiguityReason::UnsupportedGpuWork));
    }
    std::memset (g_shaderResources, 0, sizeof (g_shaderResources));
    std::memset (g_renderTargets, 0, sizeof (g_renderTargets));
    g_pendingCameraTarget = 0;
    g_pendingCameraPass = 0;
    g_unsupportedGpuWork.fetch_add (1, std::memory_order_relaxed);
}

bool BeginPresent (IDXGISwapChain* swapChain, uint64_t cameraHash)
{
    if (!EnterCallback ())
        return false;
    if (swapChain == nullptr || !EnterPresentThread ()) {
        LeaveCallback ();
        return false;
    }

    const uint32_t operationsBefore = g_contextOperations.load (std::memory_order_acquire);
    const uint64_t operationVersionBefore = g_contextOperationVersion.load (std::memory_order_acquire);

    ID3D11Texture2D* texture = nullptr;
    const HRESULT bufferResult = swapChain->GetBuffer (0, __uuidof (ID3D11Texture2D), (void**) &texture);
    const uint64_t backBuffer = (SUCCEEDED (bufferResult) && texture != nullptr)
                                    ? uint64_t (uintptr_t (static_cast<ID3D11Resource*> (texture)))
                                    : 0;
    if (backBuffer == 0)
        g_backBufferFailures.fetch_add (1, std::memory_order_relaxed);

    Row row;
    row.cameraHash = cameraHash;
    row.backBuffer = backBuffer;

    ResourceSnapshot entry;
    bool hasEntry = false;
    const bool resourcesReady = g_resourceResetRequested.load (std::memory_order_acquire) ==
                                g_resourceResetApplied.load (std::memory_order_acquire);
    if (resourcesReady)
        ReadCameraAndResource (backBuffer, row.cameraPass, entry, hasEntry);
    if (hasEntry) {
        row.resourceState = entry.state;
        row.resourceAmbiguityMask = entry.ambiguityMask;
        if (entry.state == ResourceState::Known)
            row.imagePass = entry.scenePass;
    }

    if (hasEntry && entry.state == ResourceState::Ambiguous) {
        row.relation = Relation::Ambiguous;
    }
    else if (row.imagePass == 0 || row.cameraPass == 0) {
        row.relation = Relation::Unknown;
    }
    else {
        row.delta = int64_t (row.cameraPass) - int64_t (row.imagePass);
        if (row.delta == 0) {
            row.relation = Relation::Match;
        }
        else {
            row.relation = Relation::Mismatch;
        }
    }
    if (texture != nullptr)
        texture->Release ();

    const uint64_t operationVersionAfter = g_contextOperationVersion.load (std::memory_order_acquire);
    const uint32_t operationsAfter = g_contextOperations.load (std::memory_order_acquire);
    const uint64_t swapChainIdentity = uint64_t (uintptr_t (swapChain));
    if (g_cachedSwapChain != swapChainIdentity) {
        DXGI_SWAP_CHAIN_DESC description = {};
        if (SUCCEEDED (swapChain->GetDesc (&description))) {
            g_cachedSwapChain = swapChainIdentity;
            g_cachedSwapEffect = description.SwapEffect;
        }
    }
    g_pendingPresentRow = row;
    g_pendingContextOperationVersion = operationVersionAfter;
    g_pendingContextOverlap =
        operationsBefore != 0 || operationsAfter != 0 || operationVersionBefore != operationVersionAfter;
    g_pendingDiscardPresent =
        g_cachedSwapChain == swapChainIdentity &&
        (g_cachedSwapEffect == DXGI_SWAP_EFFECT_DISCARD || g_cachedSwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    return true;
}

void EndPresent (bool active, bool succeeded)
{
    if (!active)
        return;
    if (succeeded) {
        const bool overlap =
            g_pendingContextOverlap || g_contextOperations.load (std::memory_order_acquire) != 0 ||
            g_contextOperationVersion.load (std::memory_order_acquire) != g_pendingContextOperationVersion;
        if (overlap) {
            g_pendingPresentRow.presentContextOverlap = true;
            g_pendingPresentRow.relation = Relation::Ambiguous;
        }
        CountAndPublish (g_pendingPresentRow);
        if (g_pendingDiscardPresent)
            g_resourceResetRequested.fetch_add (1, std::memory_order_release);
    }
    LeaveCallback ();
}

void OnResizeBuffers ()
{
    CallbackGuard guard;
    if (!guard)
        return;
    g_resourceResetRequested.fetch_add (1, std::memory_order_release);
}

size_t CopyRows (Row* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    const uint64_t written = g_rowsWritten.load (std::memory_order_acquire);
    uint64_t first = g_rowsVisibleFrom.load (std::memory_order_acquire);
    if (written > kRowCapacity && first < written - kRowCapacity)
        first = written - kRowCapacity;
    if (written - first > capacity)
        first = written - capacity;
    size_t copied = 0;
    for (uint64_t index = first; index < written; ++index) {
        RowSlot& slot = g_rows[index % kRowCapacity];
        const uint64_t expected = index + 1;
        if (slot.published.load (std::memory_order_acquire) != expected)
            continue;
        Row row;
        row.present = slot.present.load (std::memory_order_relaxed);
        row.imagePass = slot.imagePass.load (std::memory_order_relaxed);
        row.cameraPass = slot.cameraPass.load (std::memory_order_relaxed);
        row.cameraHash = slot.cameraHash.load (std::memory_order_relaxed);
        row.backBuffer = slot.backBuffer.load (std::memory_order_relaxed);
        row.delta = slot.delta.load (std::memory_order_relaxed);
        row.relation = Relation (slot.relation.load (std::memory_order_relaxed));
        row.resourceState = ResourceState (slot.resourceState.load (std::memory_order_relaxed));
        row.resourceAmbiguityMask = slot.resourceAmbiguityMask.load (std::memory_order_relaxed);
        row.presentContextOverlap = slot.presentContextOverlap.load (std::memory_order_relaxed);
        if (slot.published.load (std::memory_order_acquire) == expected)
            out[copied++] = row;
    }
    return copied;
}

Stats GetStats ()
{
    Stats stats;
    stats.enabled = Enabled ();
    stats.contextHookInstalled = ContextHookInstalled ();
    stats.presentHookInstalled = PresentHookInstalled ();
    stats.hookInstalled = stats.contextHookInstalled && stats.presentHookInstalled;
    stats.srvHookEnabled = ContextSlotEnabled (ContextSlot::PSSetShaderResources);
    stats.contextThreadId = g_contextThread.load (std::memory_order_acquire);
    stats.presentThreadId = g_presentThread.load (std::memory_order_acquire);
    stats.presents = g_presents.load (std::memory_order_relaxed);
    stats.matched = g_matched.load (std::memory_order_relaxed);
    stats.mismatched = g_mismatched.load (std::memory_order_relaxed);
    stats.unknown = g_unknown.load (std::memory_order_relaxed);
    stats.ambiguous = g_ambiguous.load (std::memory_order_relaxed);
    stats.backBufferFailures = g_backBufferFailures.load (std::memory_order_relaxed);
    stats.resourceTableOverflows = g_resourceTableOverflows.load (std::memory_order_relaxed);
    stats.renderThreadViolations = g_renderThreadViolations.load (std::memory_order_relaxed);
    stats.unsupportedGpuWork = g_unsupportedGpuWork.load (std::memory_order_relaxed);
    stats.snapshotDrainTimeouts = g_snapshotDrainTimeouts.load (std::memory_order_relaxed);
    for (size_t i = 0; i < kResourceAmbiguityReasonCount; ++i)
        stats.firstAmbiguityTransitions[i] = g_firstAmbiguityTransitions[i].load (std::memory_order_relaxed);
    stats.resourceAmbiguousPresents = g_resourceAmbiguousPresents.load (std::memory_order_relaxed);
    stats.presentContextOverlaps = g_presentContextOverlaps.load (std::memory_order_relaxed);
    const ContextHookStats contextStats = GetContextHookStats ();
    const uint64_t repairsAtStart = g_contextHookRepairsAtStart.load (std::memory_order_relaxed);
    stats.contextHookRepairs = contextStats.repairs >= repairsAtStart ? contextStats.repairs - repairsAtStart : 1;
    const uint64_t rowsWritten = g_rowsWritten.load (std::memory_order_acquire);
    const uint64_t rowsVisibleFrom = g_rowsVisibleFrom.load (std::memory_order_acquire);
    stats.rowsOverwritten =
        rowsWritten - rowsVisibleFrom > kRowCapacity ? rowsWritten - rowsVisibleFrom - kRowCapacity : 0;
    stats.contextSlotsPatched = contextStats.slotsStillPatched;
    stats.resourceResetPending = g_resourceResetRequested.load (std::memory_order_acquire) !=
                                 g_resourceResetApplied.load (std::memory_order_acquire);
    return stats;
}

} // namespace passprovenance
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
