#ifndef EVP_NATIVECOMMANDS_OVERLAYHUDCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_OVERLAYHUDCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// What the user does on the overlays' HUD, for Python (ArchViz/OverlayHudEvents.hpp): the
// changes they made, as a sequence to poll, and the state they left.
//
//   Tapioca.OverlayHudEvents {sinceSeq?, maxEvents?}   the changes after `sinceSeq`
//   Tapioca.OverlayHud {fontScale?}                     the text size and the panels'
//                                                       state; sets the text size
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetOverlayHudCommandRegistrations ();

} // namespace geomsrv

#endif
