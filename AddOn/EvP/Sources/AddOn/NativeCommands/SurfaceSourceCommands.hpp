#ifndef EVP_NATIVECOMMANDS_SURFACESOURCECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SURFACESOURCECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// SDK-free face correspondence reads over the caller's immutable snapshot.
NativeCommandRegistrations GetSurfaceSourceCommandRegistrations ();

} // namespace geomsrv

#endif
