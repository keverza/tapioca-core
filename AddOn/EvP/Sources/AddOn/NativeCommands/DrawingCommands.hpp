#ifndef EVP_NATIVECOMMANDS_DRAWINGCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_DRAWINGCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Placed Drawings on layouts — the crop (clip polygon) that the JSON API cannot
// reach by any path (E20). GetDrawingClipPolygon (read), SetDrawingClipPolygon
// (write). Registrations retain registry order.
NativeCommandRegistrations GetDrawingCommandRegistrations ();

} // namespace geomsrv

#endif
