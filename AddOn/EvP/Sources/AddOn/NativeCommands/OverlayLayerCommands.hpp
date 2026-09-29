#ifndef EVP_NATIVECOMMANDS_OVERLAYLAYERCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_OVERLAYLAYERCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The caller's own content on the overlays in Archicad's views (ArchViz/OverlayLayers.hpp):
// named layers of polylines, points, meshes -- styled, or heatmaps with a ramp --
// texts fixed to the model or to the view, dimensions and legends. One store, drawn
// by each overlay through its own transform.
//
//   Tapioca.SetOverlayLayer, Tapioca.ClearOverlayLayer, Tapioca.OverlayLayers
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetOverlayLayerCommandRegistrations ();

} // namespace geomsrv

#endif
