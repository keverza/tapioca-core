// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- §3, a view rebuilt is rebound and not
// relearned; §8, what outlives a session is named, checked and forgotten.
// ArchViz/OverlayCameraKeep -- see the header.

#include "ArchViz/OverlayCameraKeep.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/HookMarker.hpp"
#include "ArchViz/Dxgi/PresentHook.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"
#include "ArchViz/OverlayRedrawBudget.hpp"
#include "ArchViz/OverlayRuntimeReport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {
namespace camerakeep {

namespace {

namespace cen = dxgi::census;

Kept g_held;
// The camera this session resumed, for the line that says why it was dropped.
dxgi::census::KeptCamera g_applied;
// The recognizer's drops already said this session.
uint64_t g_droppedSaid = 0;

} // namespace

Kept Take ()
{
    Kept kept;
    kept.camera = cen::Keep ();
    kept.window = dxgi::SwapChainWindow (dxgi::MarkerTarget ());
    if (!kept.camera.valid || kept.window == 0) {
        kept.camera.valid = false;
        return kept;
    }
    // The scene pass's extent as learned; a camera resumed and not found again yet has
    // only its own viewport, which is that pass's.
    const dxgi::renderstate::SceneSignature scene = dxgi::renderstate::GetSceneSignature ();
    kept.sceneWidth = scene.learned ? scene.viewportWidth : kept.camera.fingerprint.viewportWidth;
    kept.sceneHeight = scene.learned ? scene.viewportHeight : kept.camera.fingerprint.viewportHeight;
    std::vector<cen::Group> groups (cen::kGroupCapacity);
    const size_t count = cen::CopyGroups (groups.data (), groups.size ());
    for (size_t i = 0; i < count; ++i) {
        if (groups[i].groupId == kept.camera.selection.groupId)
            kept.camera.cameraIndexCount = groups[i].cameraIndexCount;
    }
    if (kept.camera.cameraIndexCount == 0)
        kept.camera.cameraIndexCount = cen::KeptCameraIndexCount ();
    // ⚠️ THE KEPT CAMERA IS ITS GROUP'S CAMERA DRAW. A census group is keyed without the
    // index count, so the fingerprint's -- the group's LAST draw when it was chosen -- can be
    // another draw that took the occurrence (§4). In session the pin never asks; a kept
    // camera has only the fingerprint, and one naming another draw matches nothing (the
    // user, 2026-10-01 09:06:12: `g7 occ6 idx636` drawn every frame, dropped after five).
    if (kept.camera.cameraIndexCount != 0)
        kept.camera.fingerprint.indexCount = kept.camera.cameraIndexCount;
    return kept;
}

void Hold (const Kept& kept)
{
    g_held = kept;
}

void Forget ()
{
    g_held = Kept {};
    g_droppedSaid = 0;
}

void Apply ()
{
    g_droppedSaid = 0;
    const Kept kept = g_held;
    g_held = Kept {};
    if (!kept.camera.valid)
        return;
    if (!::IsWindow (HWND (uintptr_t (kept.window)))) {
        report::Say ("CAMERA", "the 3D window the camera was kept for is gone; it is learned again");
        return;
    }
    cen::Resume (kept.camera);
    g_applied = kept.camera;
    dxgi::renderstate::ExpectSceneExtent (kept.sceneWidth, kept.sceneHeight);
    dxgi::NominateWindowOnSight (kept.window);
    redrawbudget::AskSoon ();
    char line[260] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE,
                 "kept across the view change: occ%u interp%u idx%u, scene %.0fx%.0f on window 0x%llx -- rebound "
                 "on the first model frame that matches it, or learned again",
                 kept.camera.fingerprint.occurrenceIndex, kept.camera.fingerprint.variant, kept.camera.cameraIndexCount,
                 kept.sceneWidth, kept.sceneHeight, (unsigned long long) kept.window);
    report::Say ("CAMERA", line);
}

void Narrate ()
{
    const uint64_t dropped = cen::GetBindingStats ().resumesDropped;
    if (dropped <= g_droppedSaid)
        return;
    g_droppedSaid = dropped;
    // ⚠️ AND IT SAYS WHICH TERM REFUSED (§7): a draw that agreed on every term but one is
    // the camera, and that term is why. Without it the 09:06:12 drop could only be argued.
    const cen::FingerprintDiagnosis diagnosis = cen::GetFingerprintDiagnosis ();
    std::string misses;
    for (uint32_t term = 0; term < cen::kFingerprintTermCount; ++term) {
        if (diagnosis.soleMiss[term] == 0)
            continue;
        const uint32_t* seen = diagnosis.observed[term];
        char one[120] = {};
        _snprintf_s (one, sizeof (one), _TRUNCATE, " %s x%llu (first seen %u %u %u %u)",
                     cen::FingerprintTermName (term), (unsigned long long) diagnosis.soleMiss[term], seen[0], seen[1],
                     seen[2], seen[3]);
        misses += one;
    }
    char line[200] = {};
    _snprintf_s (line, sizeof (line), _TRUNCATE, "; it was occ%u idx%u %.0fx%.0f",
                 g_applied.fingerprint.occurrenceIndex, g_applied.fingerprint.indexCount,
                 g_applied.fingerprint.viewportWidth, g_applied.fingerprint.viewportHeight);
    report::Say ("CAMERA",
                 "the kept camera matched no draw in the model frames it was given; the census chooses "
                 "afresh. Sole misses:" +
                     (misses.empty () ? std::string (" none -- no draw agreed on all terms but one") : misses) + line);
}

} // namespace camerakeep
} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv
