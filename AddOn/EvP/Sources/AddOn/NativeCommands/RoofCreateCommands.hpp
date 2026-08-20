#ifndef EVP_NATIVECOMMANDS_ROOFCREATECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ROOFCREATECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Roof creation — CreateRoof in single-plane and poly modes.
//
// Split out of CreateCommands when CreateColumn arrived and that file was at its
// recorded size limit. Roofs earned the split: between the two modes, the
// pivot/eaves geometry, roofs are half the structural
// create surface and share nothing with a wall or a column beyond ResolveStory
// (which moved to CommandUtils in the same change, on its second domain).
//
// Every command here is a WriteCommand: Execute is the bare undo-free core and
// the CALLER supplies the undo scope. Registrations retain registry order.
NativeCommandRegistrations GetRoofCreateCommandRegistrations ();

} // namespace geomsrv

#endif
