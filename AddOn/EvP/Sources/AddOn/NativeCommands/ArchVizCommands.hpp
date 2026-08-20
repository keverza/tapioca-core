#ifndef EVP_NATIVECOMMANDS_ARCHVIZCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ARCHVIZCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Temporary Diligent device/viewport diagnostics retained while the P0 renderer
// decision is still in progress. The completed bgfx viewer verbs are retired.
//
// Returns this domain's retained commands in registry order.
NativeCommandRegistrations GetArchVizCommandRegistrations ();

} // namespace geomsrv

#endif
