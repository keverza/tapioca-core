#ifndef EVP_NATIVECOMMANDS_ELEMENTMODIFYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_ELEMENTMODIFYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E16 Path 3 — the SYMMETRIC WRITE for GetElementDetails: SetElementDetails.
// Takes back the same kind-discriminated `details` record the read emits and
// writes the scalar settings in it. The read side is
// NativeCommands/ElementReadCommands.cpp -> GetElementDetailsCommand; keep the
// two field spellings identical, that is the whole premise of the pair.
NativeCommandRegistrations GetElementModifyCommandRegistrations ();

} // namespace geomsrv

#endif
