#ifndef EVP_NATIVECOMMANDS_GRAPHCAPTURECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_GRAPHCAPTURECOMMANDS_HPP

// Tapioca.List3DViews - the View Map's 3D views, for the capture node's Model
// View picker.
//
// ⚠️ SEPARATE FROM GraphCaptureService.cpp BECAUSE A REGISTRATION PROVIDER
// MUST LIVE IN A *Commands.cpp - the architecture gate checks it, and it is
// right to: the registry is discovered by that name, so a provider hiding in a
// file called something else is a command that silently is not there. It is
// also a cleaner cut than the name suggests, since a service and a verb are
// different things.

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

NativeCommandRegistrations GetGraphCaptureCommandRegistrations ();

} // namespace geomsrv

#endif
