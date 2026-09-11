#ifndef EVP_NATIVECOMMANDS_ELEMENTPROPERTYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ELEMENTPROPERTYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The Property-manager side of an element: its user-defined PROPERTIES and its
// CLASSIFICATIONS, plus the four identity fields that make a property table
// readable (type name, element ID, layer, home story).
//
// A domain of its own rather than a branch in ElementReadCommands, and the
// reason is the API rather than tidiness: nothing here comes off the element
// struct. ElementReadCommands reads API_Element unions (a wall's begC, a roof's
// pivot); this reads a per-element DEFINITION LIST and then a value for each
// definition, which is a different call pattern with a different cost — the
// extractor calls it "the expensive half" and gates it behind MetaLevel::Full.
//
// GetElementProperties (read). There is deliberately no writer: setting a
// property value is a project modification and belongs with the other write
// commands, behind the same undo discipline, once something needs it.
NativeCommandRegistrations GetElementPropertyCommandRegistrations ();

} // namespace geomsrv

#endif
