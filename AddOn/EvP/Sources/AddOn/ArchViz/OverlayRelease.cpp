// ⚠️ BOUND BY OVERLAY-INVARIANTS.md §8 -- session state must not outlive the session, and
// here not the overlay either: what it kept to start again fast goes when the user turns
// it off.
// ArchViz/OverlayRelease -- see the header.

#include "ArchViz/OverlayRelease.hpp"

#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/Dxgi/HostOccluders.hpp"
#include "ArchViz/Dxgi/LayerOverlay3D.hpp"
#include "ArchViz/Dxgi/SceneGuest.hpp"
#include "ArchViz/ExtractionThread.hpp"
#include "ArchViz/OverlayAnnotations.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/SceneCmdQueue.hpp"
#include "ArchViz/StorySliceOverlay.hpp"

namespace geomsrv {
namespace archviz {
namespace overlayrelease {

void Model ()
{
    if (DiligentViewport::Get ().IsRunning ())
        return;
    // ⚠️ STOPPED BEFORE IT IS FORGOTTEN: the extraction's worker writes the snapshot it
    // builds from its own thread.
    ExtractionWorker::Get ().Stop ();
    // Only the portable viewport takes from the queue: without it, the model's meshes
    // waited there for nobody.
    SceneCmdQueue::Get ().Clear ();
    // The next start finds no snapshot and extracts the model again.
    dxgi::hostocclusion::Clear ();
}

void ThreeD ()
{
    dxgi::sceneguest::Publish (overlayscene::Scene {}, 1.0f);
    dxgi::sceneguest::PublishHud (overlayscene::Scene {}, 1.0f);
    dxgi::layers3d::Publish (overlaylayers::Prepared3D {});
    // The layers as laid out for it; the plan's are the plan's.
    overlayscene::ForgetDrafts (overlaylayers::Views::ThreeD);
    guesttext::ReleaseHud (overlayinput::View::ThreeD);
}

void Plan ()
{
    guesttext::ReleaseHud (overlayinput::View::Plan);
}

void Shared ()
{
    // The add-on's own layers first, each with what drives it: the slices' timer, the
    // trace's annotations.
    storysliceoverlay::OnProjectClosed ();
    overlayannotations::OnProjectClosed ();
    overlaylayers::ClearEverything ();
    guesttext::ForgetHudState ();
    // The laid-out layers hold the text engines' pages: they go first.
    overlayscene::ForgetDrafts ();
    guesttext::ReleaseText ();
}

} // namespace overlayrelease
} // namespace archviz
} // namespace geomsrv
