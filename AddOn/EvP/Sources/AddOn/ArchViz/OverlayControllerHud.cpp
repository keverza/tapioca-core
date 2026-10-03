// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/OverlayControllerHud -- the controller's HUD half (OverlayController.hpp): what the
// user did to the overlays' HUD, read and set for the verbs, and the renderers brought into
// line with it. Moved out of OverlayController.cpp whole when the HUD's own pages arrived;
// the controller's view-following and its sessions stay there.
//
// THREAD. Every entry point is MAIN THREAD.

#include "ArchViz/OverlayController.hpp"

#include "ArchViz/OverlayClickTiming.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayVisibility.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlaycontrol {

HudReport Hud ()
{
    HudReport out;
    const std::shared_ptr<overlayhud::State> state = guesttext::HudState ();
    out.fontScale = overlayhud::FontScaleOf (*state);
    out.open = overlayhud::HudOpen (*state);
    out.visible = overlayhud::ContentShown (*state);
    out.hover = overlayhud::HoverMode (*state);
    out.hiddenLayers = overlayhud::HiddenLayers (*state);
    const std::string selected = overlayhud::SelectedKey (*state);
    for (const auto& layer : overlaylayers::Layers ())
        for (size_t i = 0; i < layer->panels.size (); ++i) {
            const overlaylayers::Panel& panel = layer->panels[i];
            const std::string key = layer->name + "#" + std::to_string (i);
            HudPanel record { layer->name, uint32_t (i), panel.title, !panel.title.empty () && key == selected,
                              overlayhud::Values (*state, key) };
            if (!panel.title.empty () || !record.values.empty ())
                out.panels.push_back (std::move (record));
        }
    return out;
}

std::vector<std::shared_ptr<const overlaylayers::Layer>> ShownLayers ()
{
    std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = overlaylayers::Layers ();
    const std::shared_ptr<overlayhud::State> state = guesttext::HudState ();
    layers.erase (std::remove_if (layers.begin (), layers.end (),
                                  [&] (const std::shared_ptr<const overlaylayers::Layer>& layer) {
                                      return !overlayhud::LayerShown (*state, layer->name);
                                  }),
                  layers.end ());
    return layers;
}

namespace {

// What the renderers last followed of the HUD's state.
uint64_t g_followedRevision = 0;
std::vector<std::string> g_followedHidden;

} // namespace

void FollowHudState ()
{
    const std::shared_ptr<overlayhud::State> state = guesttext::HudState ();
    const uint64_t revision = overlayhud::Revision (*state);
    if (revision == g_followedRevision)
        return;
    g_followedRevision = revision;
    // Shown or hidden as a whole: read at Present, nothing rebuilt.
    overlayvisibility::SetContentShown (overlayhud::ContentShown (*state));
    // Hover mode: read by the input hook, which then follows the pointer over the whole view.
    overlayvisibility::SetHovering (overlayhud::HoverMode (*state));
    // A layer hidden or shown again: the content is rebuilt without it -- the 3D view's now,
    // the plan's at its next tick, as the store moved.
    std::vector<std::string> hidden = overlayhud::HiddenLayers (*state);
    if (hidden != g_followedHidden) {
        g_followedHidden = std::move (hidden);
        overlaylayers::Touch ();
        PublishLayers ();
    }
    // Both views' HUDs follow -- the dock's circle, the tabs of a hidden layer -- and draw.
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

void SetOverlayVisible (bool visible)
{
    overlayhud::SetContentShown (*guesttext::HudState (), visible);
    FollowHudState ();
}

void SetLayerVisible (const std::string& layer, bool visible)
{
    overlayhud::SetLayerShown (*guesttext::HudState (), layer, visible);
    FollowHudState ();
}

void SetHudOpen (bool open)
{
    overlayhud::SetHudOpen (*guesttext::HudState (), open);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

bool SelectHudPanel (const std::string& layer, uint32_t panel)
{
    for (const auto& set : overlaylayers::Layers ())
        if (set->name == layer && panel < set->panels.size () && !set->panels[panel].title.empty ()) {
            overlayhud::SelectKey (*guesttext::HudState (), layer + "#" + std::to_string (panel));
            overlayinput::RequestLayout (overlayinput::View::ThreeD);
            overlayinput::RequestLayout (overlayinput::View::Plan);
            return true;
        }
    return false;
}

void TimeHudClicks (bool on)
{
    overlayinput::TimeClicks (on);
}

ClickReport HudClicks ()
{
    ClickReport out;
    out.armed = overlayinput::TimingClicks ();
    out.idles = overlayinput::ClickIdles ();
    const std::vector<overlayclicks::Sample> samples = overlayinput::ClickSamples ();
    uint64_t newest = 0;
    for (const overlayclicks::Sample& sample : samples)
        newest = (std::max) (newest, sample.at);
    for (const overlayclicks::Sample& sample : samples) {
        ClickRecord record;
        record.target = overlayclicks::TargetName (sample.target);
        record.windowClass = sample.windowClass;
        record.complete = sample.complete;
        record.ageMilliseconds = uint32_t ((newest - sample.at) / 1000u);
        record.busyMicroseconds = sample.busyMicroseconds;
        record.firstIdleMicroseconds = sample.firstIdleMicroseconds;
        record.bursts = sample.bursts;
        record.layoutMicroseconds = sample.layoutMicroseconds;
        record.layouts = sample.layouts;
        record.redrawMicroseconds = sample.redrawMicroseconds;
        record.redraws = sample.redraws;
        out.clicks.push_back (std::move (record));
    }
    return out;
}

void SetHoverMode (bool on)
{
    // The readout is in the HUD's floating panel: on, the panel opens to show it.
    if (on)
        overlayhud::SetHudOpen (*guesttext::HudState (), true);
    overlayhud::SetHoverMode (*guesttext::HudState (), on);
    FollowHudState ();
}

void SetHudFontScale (float scale)
{
    overlayhud::SetFontScale (*guesttext::HudState (), scale);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

} // namespace overlaycontrol
} // namespace archviz
} // namespace geomsrv
