#ifndef EVP_NATIVECOMMANDS_MODELGEOMETRYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_MODELGEOMETRYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E24 — structured ModelerAPI geometry reads: GetModelInfo, GetModelElements,
// GetBodyGeometry. The BREP-level counterpart to the triangle-soup snapshot.
// Returns the domain's ordered command registrations.
NativeCommandRegistrations GetModelGeometryCommandRegistrations ();

} // namespace geomsrv

#endif
