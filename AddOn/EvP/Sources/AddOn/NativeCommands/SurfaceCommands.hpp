#ifndef EVP_NATIVECOMMANDS_SURFACECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SURFACECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// §7.4 — element-level surface override: SetElementSurface (write).
// Returns the domain's ordered command registrations.
NativeCommandRegistrations GetSurfaceCommandRegistrations ();

} // namespace geomsrv

#endif
