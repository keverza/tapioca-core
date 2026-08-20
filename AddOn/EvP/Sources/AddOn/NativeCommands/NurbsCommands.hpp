#ifndef EVP_NATIVECOMMANDS_NURBSCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_NURBSCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E24 — the curved-geometry path: GetNurbsBody (the full
// vertex→edge→trim→loop→face→shell→lump topology tree) and GetPointClouds.
// Neither has any representation in the tessellated snapshot.
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetNurbsCommandRegistrations ();

} // namespace geomsrv

#endif
