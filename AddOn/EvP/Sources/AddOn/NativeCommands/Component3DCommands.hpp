#ifndef EVP_NATIVECOMMANDS_COMPONENT3DCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_COMPONENT3DCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E24 — the C API ModelAccess path (API_Component3D): Get3DComponentCounts,
// GetElement3DInfo, GetBodyComponents, DecomposePolygon, GetTextureCoordAtPoint.
// The raw body/polygon/edge/vertex database, which is where wall holes and
// element intersections are visible and the ModelerAPI path is not.
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetComponent3DCommandRegistrations ();

} // namespace geomsrv

#endif
