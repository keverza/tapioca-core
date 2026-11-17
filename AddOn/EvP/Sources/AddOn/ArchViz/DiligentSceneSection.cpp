// The section plane's caps and section line (issue #14 P2): when to cut, where the cut runs, and
// how it is drawn. The cut itself is pure and lives in SectionCaps.cpp; this is the scene's half --
// the snapshot, the worker, the upload and the draw.
//
// ⚠️ OFF THE RENDER THREAD, LATEST WINS. Cutting every element the plane crosses is milliseconds on
// a house and far more on a campus, and the plane is dragged. So a build runs on a worker from a
// SNAPSHOT of shared meshes; while it runs the previous caps stay on screen (`building` tells the
// HUD), and when it lands, a plane that moved again in the meantime simply starts the next one.
// Nothing here blocks a frame on a build, except teardown (DiligentSceneImpl ShutdownStorySlices).
//
// ⚠️ STALENESS IS A FINGERPRINT, NOT A REVISION COUNTER. An upsert replaces an element's mesh
// object and a hide flips `hidden`, in different files and command paths; hashing (mesh pointer,
// hidden) per element each frame the caps are on catches every one of them at O(elements), and
// cannot be forgotten by the next path that changes geometry.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/MatrixMath.hpp"

#include <cstring>
#include <string>

namespace geomsrv {
namespace archviz {

namespace {

uint64_t Mix (uint64_t h, uint64_t value)
{
    return (h ^ value) * 1099511628211ull;
}

} // namespace

void DiligentScene::SetSectionCapStyle (const SectionCapStyle& style)
{
    if (impl_ != nullptr)
        impl_->sectionCaps.style = style;
}

void DiligentScene::DrawSectionCaps (Diligent::IDeviceContext* context, const float viewProj[16])
{
    if (impl_ == nullptr || context == nullptr || impl_->device == nullptr)
        return;
    Impl::SectionCaps& caps = impl_->sectionCaps;
    if (!caps.planeEnabled || !caps.style.caps || caps.initFailed)
        return;

    // ---- 1. what the caps should show now -------------------------------------------------
    uint32_t coordinateBits = 0;
    std::memcpy (&coordinateBits, &caps.coordinate, sizeof (coordinateBits));
    uint64_t key = 1469598103934665603ull;
    key = Mix (key, uint64_t (caps.axis));
    key = Mix (key, coordinateBits);
    key = Mix (key, caps.flipped ? 1u : 0u);
    for (const Entry& e : impl_->elements) {
        key = Mix (key, uint64_t (reinterpret_cast<uintptr_t> (e.sectionMesh.get ())));
        key = Mix (key, e.hidden ? 1u : 0u);
    }
    if (key == 0)
        key = 1; // 0 means "nothing built"

    // ---- 2. a finished build lands ----------------------------------------------------------
    if (caps.inFlight.valid () && caps.inFlight.wait_for (std::chrono::seconds (0)) == std::future_status::ready) {
        SectionCapResult result = caps.inFlight.get ();
        if (!caps.layer.IsReady ()) {
            std::string error;
            if (!caps.layer.Init (impl_->device, impl_->initColorFormat, impl_->initDepthFormat, error)) {
                caps.initFailed = true;
                ArchVizLog ("Diligent scene: section caps unavailable (" + error + ")");
                return;
            }
        }
        std::string error;
        if (caps.layer.Upload (impl_->device, context, result.outline, result.fill, result.fillIndices, error)) {
            caps.builtKey = caps.inFlightKey;
            caps.builtAxis = caps.inFlightAxis;
            caps.counts = result.counts;
        }
        else {
            ArchVizLog ("Diligent scene: section caps not uploaded (" + error + ")");
        }
    }

    // ---- 3. a stale one starts the next build -----------------------------------------------
    const bool stale = key != caps.builtKey;
    if (stale && !caps.inFlight.valid ()) {
        SectionCapRequest request;
        request.axis = caps.axis;
        request.coordinate = caps.coordinate;
        request.flipped = caps.flipped;
        request.meshes.reserve (impl_->elements.size ());
        for (const Entry& e : impl_->elements)
            if (!e.hidden && e.sectionMesh != nullptr)
                request.meshes.push_back (e.sectionMesh);
        caps.inFlightKey = key;
        caps.inFlightAxis = caps.axis;
        caps.inFlight =
            std::async (std::launch::async, [request = std::move (request)] () { return BuildSectionCaps (request); });
    }
    caps.counts.building = stale;

    // ---- 4. draw what was built, in its own plane's frame -----------------------------------
    // ⚠️ THE BUILT AXIS, NOT THE CURRENT ONE: caps cut on X drawn through Z's matrix would lie
    // across the model for the frames until the Z build lands.
    if (caps.builtKey == 0 || !caps.layer.IsReady ())
        return;
    float localToWorld[16];
    SectionLocalToWorld (caps.builtAxis, localToWorld);
    float capViewProj[16];
    Multiply (capViewProj, localToWorld, viewProj);
    StorySliceLayer::DrawParams params;
    params.widthPixels = caps.style.linePixels;
    params.rgba = caps.style.lineRgba;
    params.fillRgba = caps.style.fillRgba;
    params.drawFill = true;
    // Behind the kept geometry, a cap is hidden like any face: it is the solid's own cut surface.
    params.occluded = OccludedStyle::Hidden;
    caps.layer.Draw (context, capViewProj, impl_->viewportWidth, impl_->viewportHeight, params);
}

} // namespace archviz
} // namespace geomsrv
