#ifndef EVP_NATIVECOMMANDS_TOPOLOGYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_TOPOLOGYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E6 — zone/room topology: GetCollisions, GetConnectedElements.
// Returns the domain's ordered command registrations.
NativeCommandRegistrations GetTopologyCommandRegistrations ();

} // namespace geomsrv

#endif
