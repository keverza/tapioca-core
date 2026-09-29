#ifndef EVP_NATIVECOMMANDS_OVERLAYLAYERCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_OVERLAYLAYERCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The caller's own geometry on the overlays in Archicad's views (ArchViz/OverlayLayers.hpp):
// named layers of polylines, points and meshes. One store, drawn by each overlay through
// its own transform.
//
//   Tapioca.SetOverlayLayer, Tapioca.ClearOverlayLayer, Tapioca.OverlayLayers
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetOverlayLayerCommandRegistrations ();

} // namespace geomsrv

#endif
