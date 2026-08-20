#ifndef EVP_NATIVECOMMANDS_ATTRIBUTECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ATTRIBUTECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Named-attribute reads: GetAttributeInfo (composite thickness, profile bbox).
//
// Split out of ElementReadCommands when the roof pivot/baseLine read pushed that file
// past its size ceiling — and the right seam anyway: this reads an ATTRIBUTE by name,
// not an element by guid, and it is the sole reason the read domain pulled in
// ProfileVectorImageOperations.
//
NativeCommandRegistrations GetAttributeCommandRegistrations ();

} // namespace geomsrv

#endif
