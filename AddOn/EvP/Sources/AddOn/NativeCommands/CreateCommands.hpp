#ifndef EVP_NATIVECOMMANDS_CREATECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_CREATECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Element creation — every command here is a WriteCommand, so its Execute is the
// bare undo-free core and the CALLER supplies the undo scope (a transaction, or
// the dispatcher for a lone write). None of these is installed on Archicad's
// JSON port; see the policy note in CommandRegistry.cpp.
//
// PlaceLevelDimension, CreateMesh, CreateWall, CreateColumn.
// Roofs live in NativeCommands/RoofCreateCommands.hpp (split out when this file
// reached its recorded size limit); PlaceLibraryObject in LibraryObjectCommands.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetCreateCommandRegistrations ();

} // namespace geomsrv

#endif
