// ArchViz/Dxgi/PassProvenanceFirstTransition -- first Known->Ambiguous non-camera draw latch, no behaviour change.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: stores only what PassProvenance.cpp hands it.

#include "ArchViz/Dxgi/PassProvenanceFirstTransition.hpp"

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace passprovenance {
namespace firsttransition {

namespace {

struct FirstDrawPublication {
    std::atomic<uint64_t> resetGeneration { 0 };
    std::atomic<uint64_t> drawsSinceCamera { 0 };
    std::atomic<uint32_t> drawKind { uint32_t (DrawKind::Unknown) };
    std::atomic<uint32_t> drawCount { 0 };
    std::atomic<int32_t> renderTargetSlot { -1 };
    std::atomic<uint64_t> targetResource { 0 };
    std::atomic<uint64_t> targetScenePass { 0 };
    std::atomic<uint64_t> cameraPass { 0 };
    std::atomic<uint32_t> sampledLineage { uint32_t (SampledLineage::None) };
    std::atomic<int32_t> shaderResourceSlot { -1 };
    std::atomic<uint64_t> sampledResource { 0 };
    std::atomic<uint64_t> sampledScenePass { 0 };
    std::atomic<uint32_t> sampledAmbiguityMask { 0 };
    std::atomic<uint32_t> resultingAmbiguityMask { 0 };
};

FirstDrawPublication g_publication;

} // namespace

void Reset ()
{
    g_publication.resetGeneration.store (0, std::memory_order_release);
}

void Publish (const FirstKnownToAmbiguousDraw& draw, const std::atomic<uint64_t>& contextResetApplied)
{
    g_publication.resetGeneration.store (0, std::memory_order_release);
    g_publication.drawsSinceCamera.store (draw.drawsSinceCamera, std::memory_order_relaxed);
    g_publication.drawKind.store (uint32_t (draw.drawKind), std::memory_order_relaxed);
    g_publication.drawCount.store (draw.drawCount, std::memory_order_relaxed);
    g_publication.renderTargetSlot.store (draw.renderTargetSlot, std::memory_order_relaxed);
    g_publication.targetResource.store (draw.targetResource, std::memory_order_relaxed);
    g_publication.targetScenePass.store (draw.targetScenePass, std::memory_order_relaxed);
    g_publication.cameraPass.store (draw.cameraPass, std::memory_order_relaxed);
    g_publication.sampledLineage.store (uint32_t (draw.sampledLineage), std::memory_order_relaxed);
    g_publication.shaderResourceSlot.store (draw.shaderResourceSlot, std::memory_order_relaxed);
    g_publication.sampledResource.store (draw.sampledResource, std::memory_order_relaxed);
    g_publication.sampledScenePass.store (draw.sampledScenePass, std::memory_order_relaxed);
    g_publication.sampledAmbiguityMask.store (draw.sampledAmbiguityMask, std::memory_order_relaxed);
    g_publication.resultingAmbiguityMask.store (draw.resultingAmbiguityMask, std::memory_order_relaxed);
    g_publication.resetGeneration.store (contextResetApplied.load (std::memory_order_relaxed),
                                         std::memory_order_release);
}

FirstKnownToAmbiguousDraw Read (const std::atomic<uint64_t>& resetRequested,
                                const std::atomic<uint64_t>& contextResetApplied)
{
    FirstKnownToAmbiguousDraw draw;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t generation = g_publication.resetGeneration.load (std::memory_order_acquire);
        const uint64_t requested = resetRequested.load (std::memory_order_acquire);
        if (generation == 0 || generation != requested ||
            contextResetApplied.load (std::memory_order_acquire) != requested)
            return FirstKnownToAmbiguousDraw {};
        draw.drawsSinceCamera = g_publication.drawsSinceCamera.load (std::memory_order_relaxed);
        draw.drawKind = DrawKind (g_publication.drawKind.load (std::memory_order_relaxed));
        draw.drawCount = g_publication.drawCount.load (std::memory_order_relaxed);
        draw.renderTargetSlot = g_publication.renderTargetSlot.load (std::memory_order_relaxed);
        draw.targetResource = g_publication.targetResource.load (std::memory_order_relaxed);
        draw.targetScenePass = g_publication.targetScenePass.load (std::memory_order_relaxed);
        draw.cameraPass = g_publication.cameraPass.load (std::memory_order_relaxed);
        draw.sampledLineage = SampledLineage (g_publication.sampledLineage.load (std::memory_order_relaxed));
        draw.shaderResourceSlot = g_publication.shaderResourceSlot.load (std::memory_order_relaxed);
        draw.sampledResource = g_publication.sampledResource.load (std::memory_order_relaxed);
        draw.sampledScenePass = g_publication.sampledScenePass.load (std::memory_order_relaxed);
        draw.sampledAmbiguityMask = g_publication.sampledAmbiguityMask.load (std::memory_order_relaxed);
        draw.resultingAmbiguityMask = g_publication.resultingAmbiguityMask.load (std::memory_order_relaxed);
        if (generation == g_publication.resetGeneration.load (std::memory_order_acquire) &&
            requested == resetRequested.load (std::memory_order_acquire)) {
            draw.valid = true;
            return draw;
        }
    }
    return FirstKnownToAmbiguousDraw {};
}

} // namespace firsttransition
} // namespace passprovenance
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
