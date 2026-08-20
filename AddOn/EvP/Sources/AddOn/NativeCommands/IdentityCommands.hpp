#ifndef EVP_NATIVECOMMANDS_IDENTITYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_IDENTITYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The element "ID" field of Element Settings — the API's compound INFO STRING —
// read and written for ANY element type: GetElementIds (read), SetElementIds
// (write).
NativeCommandRegistrations GetIdentityCommandRegistrations ();

} // namespace geomsrv

#endif
