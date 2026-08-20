#ifndef EVP_NATIVECOMMANDS_CUTTINGPLANECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_CUTTINGPLANECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E24 — the three reads that need a SEPARATE-COMPONENTS model: GetCutPolygons,
// GetBodyBuildingMaterials, GetConnectionTable. Section area through a solid,
// the building material of each body, and which elements touch which.
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetCuttingPlaneCommandRegistrations ();

} // namespace geomsrv

#endif
