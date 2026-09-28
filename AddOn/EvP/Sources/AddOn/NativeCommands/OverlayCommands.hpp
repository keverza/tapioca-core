#ifndef EVP_NATIVECOMMANDS_OVERLAYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_OVERLAYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The overlays drawn into Archicad's own views, as a caller asks for them -- the
// same intents the `Tapioca 3D Overlay` and `Tapioca 2D Overlay` menu items toggle
// (ArchViz/OverlayController.hpp), so a script and a user can never disagree about
// whether an overlay is on. `OverlayRuntime` stays the 3D runtime's diagnostic
// surface; these are the product's.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetOverlayCommandRegistrations ();

} // namespace geomsrv

#endif
