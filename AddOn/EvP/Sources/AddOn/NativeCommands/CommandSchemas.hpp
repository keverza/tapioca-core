#ifndef EVP_NATIVECOMMANDS_COMMANDSCHEMAS_HPP
#define EVP_NATIVECOMMANDS_COMMANDSCHEMAS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace geomsrv {

// Central contract table for the coordinated Tapioca v2 C++ pass. Domain
// handlers may temporarily keep an override while their wire shape is migrated;
// every other command resolves its final schema here by registry name.
GS::Optional<GS::UniString> GetNativeSchemaDefinitions ();
GS::Optional<GS::UniString> GetNativeInputSchema (const GS::String& commandName);
GS::Optional<GS::UniString> GetNativeResponseSchema (const GS::String& commandName);

} // namespace geomsrv

#endif
