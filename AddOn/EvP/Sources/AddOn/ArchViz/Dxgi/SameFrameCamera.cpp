// ArchViz/Dxgi/SameFrameCamera -- see the header. Extracted from
// ViewMatrixCandidates.cpp (2026-09-13) along the seam between stage 3's
// discovery scorer and stage 4's production resolver.

#include "ArchViz/Dxgi/SameFrameCamera.hpp"

#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/Dxgi/ViewMatrixCandidates.hpp"

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace viewmatrix {

namespace {

std::atomic<uint64_t> g_resolved {0};
std::atomic<uint64_t> g_noView {0};
std::atomic<uint64_t> g_noProjection {0};
std::atomic<uint64_t> g_straddle {0};
std::atomic<uint32_t> g_lastAge {0};
std::atomic<uint64_t> g_lastPass {0};

// ⚠️ CAPPED. The walk is bounded by the tracked table's 64 entries of 16 blocks;
// this is the ceiling on what any one pass can contribute, and a pass with more
// camera-shaped constants than this has bigger problems than a truncated list.
constexpr size_t kMaxBlocks = 128;

}   // namespace

ScenePassCamera ResolveScenePassPair (uint32_t maxAgePasses)
{
    ScenePassCamera camera;

    // ⚠️ THE NEWEST COMPLETED PASS, NOT THE ONE IN PROGRESS. A pass still being
    // recorded has bound a camera and not yet finished drawing with it, and
    // pairing against it would hand stage 5 a camera for a pass that may still
    // bind another.
    const renderstate::ScenePass pass = renderstate::LastCompletedScenePass ();
    if (pass.generation == 0) {
        g_noProjection.fetch_add (1, std::memory_order_relaxed);
        return camera;
    }

    CameraBlock blocks[kMaxBlocks];
    const size_t count = SnapshotCameraBlocks (blocks, kMaxBlocks, pass.generation,
            maxAgePasses);

    const CameraBlock* view = nullptr;
    const CameraBlock* projection = nullptr;
    for (size_t i = 0; i < count; ++i) {
        const CameraBlock& block = blocks[i];
        if (block.projective) {
            if (projection == nullptr || block.scenePass > projection->scenePass)
                projection = &block;
        } else {
            if (view == nullptr || block.scenePass > view->scenePass)
                view = &block;
        }
    }

    if (projection == nullptr) {
        g_noProjection.fetch_add (1, std::memory_order_relaxed);
        return camera;
    }
    if (view == nullptr) {
        g_noView.fetch_add (1, std::memory_order_relaxed);
        return camera;
    }
    if (view->scenePass != projection->scenePass) {
        // ⚠️ THE INTERESTING REFUSAL. Both halves are captured and they belong to
        // different rendering passes -- the splice a Present-based stamp would
        // have accepted without a word.
        g_straddle.fetch_add (1, std::memory_order_relaxed);
        return camera;
    }

    std::memcpy (camera.view, view->m, sizeof (camera.view));
    std::memcpy (camera.projection, projection->m, sizeof (camera.projection));
    Multiply (camera.viewProj, camera.view, camera.projection);

    camera.valid = true;
    camera.scenePass = view->scenePass;
    camera.agePasses = uint32_t (pass.generation - view->scenePass);
    camera.viewBindSlot = view->bindSlot;
    camera.projectionBindSlot = projection->bindSlot;
    camera.colorTarget = pass.colorTarget;
    camera.depthTarget = pass.depthTarget;

    g_resolved.fetch_add (1, std::memory_order_relaxed);
    g_lastAge.store (camera.agePasses, std::memory_order_relaxed);
    g_lastPass.store (camera.scenePass, std::memory_order_relaxed);
    return camera;
}

PairingStats GetPairingStats ()
{
    PairingStats stats;
    stats.resolved = g_resolved.load (std::memory_order_relaxed);
    stats.refusedNoView = g_noView.load (std::memory_order_relaxed);
    stats.refusedNoProj = g_noProjection.load (std::memory_order_relaxed);
    stats.refusedStraddle = g_straddle.load (std::memory_order_relaxed);
    stats.lastAgePasses = g_lastAge.load (std::memory_order_relaxed);
    stats.lastScenePass = g_lastPass.load (std::memory_order_relaxed);
    return stats;
}

void ResetPairing ()
{
    g_resolved.store (0, std::memory_order_relaxed);
    g_noView.store (0, std::memory_order_relaxed);
    g_noProjection.store (0, std::memory_order_relaxed);
    g_straddle.store (0, std::memory_order_relaxed);
    g_lastAge.store (0, std::memory_order_relaxed);
    g_lastPass.store (0, std::memory_order_relaxed);
}

}   // namespace viewmatrix
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
