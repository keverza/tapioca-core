#ifndef TAPIOCA_ARCHVIZ_VIEWERSCENELAYERS_HPP
#define TAPIOCA_ARCHVIZ_VIEWERSCENELAYERS_HPP

// #201: the overlay layers the Tapioca 3D viewer draws over its own model. Pure and
// MAIN THREAD (the viewer editor's refresh); no renderer state is touched here.
#include "ArchViz/OverlayLayers.hpp"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace geomsrv::archviz::viewerscene {
using LayerPtr = std::shared_ptr<const overlaylayers::Layer>;

// ⚠️ THE VIEWER HAS ONLY THE GUEST'S SCENE PIPELINE. The injected overlay draws an
// unstyled mesh or plain polyline on its raw pipelines (overlaylayers::Prepare3D) and
// overlayscene::PrepareScene skips it, so the viewer drew nothing of the envelope or
// the slices. This copy marks those items for the guest with the look the raw path gives
// them: the mesh's own colour (Shading::Flat) and the layer's own occlusion. A layer with
// nothing raw is returned as it is.
//
// `hideBehind`: while the exploded diagram stands in, what lies behind it is hidden, never
// faint or dashed. Those styles read through the Archicad model; behind a diagram body they
// drew the property line across the storeys (user, 2026-11-19). "Always" items stay.
LayerPtr GuestDrawn (const LayerPtr& layer, bool hideBehind = false);

// What the viewer draws: the editor's layer always, and while it hosts the Massing
// workflow every layer the overlay would draw in 3D -- shown, and drawn in that view.
// The exposed-surface layer is left out: the viewer draws its own measured highlight.
// While the exploded diagram stands in for the massing (its "hide source" is on), the
// layers that draw those storeys in place are left out too, and so is the envelope: the
// diagram is their representation then, and a second copy at the assembled position reads
// as a clash.
struct Selection {
    bool massing = false;
    bool diagramStandsIn = false;
    std::function<bool (const std::string&)> shown; // the HUD's per-layer visibility
};
// The layers the diagram replaces: the massing storeys at their source position, and the
// envelope around them.
bool DrawsStoreysInPlace (const std::string& layer);
std::vector<LayerPtr> Select (const std::vector<LayerPtr>& all, const Selection& selection);

// The conversions of the last call kept by source layer, so an unchanged layer gives
// the same converted object -- the identity PrepareScene's per-layer cache is keyed on.
class Converter {
  public:
    std::vector<LayerPtr> Convert (const std::vector<LayerPtr>& layers, bool hideBehind = false);
    void Clear ()
    {
        last_.clear ();
    }

  private:
    std::vector<std::pair<LayerPtr, LayerPtr>> last_;
    bool hideBehind_ = false; // what `last_` was converted with
};
} // namespace geomsrv::archviz::viewerscene
#endif
