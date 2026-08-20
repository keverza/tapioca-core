#ifndef EVP_NATIVECOMMANDS_SCHEMAVALIDATOR_HPP
#define EVP_NATIVECOMMANDS_SCHEMAVALIDATOR_HPP

#include "ObjectState.hpp"
#include "Optional.hpp"

namespace geomsrv {

// Validates the JSON-schema subset used by native commands. The
// schema and shared GetNativeSchemaDefinitions() document are parsed on each
// call, so malformed contracts fail closed instead of reaching a handler.
//
// ObjectState exposes JSON value kinds and values, but not JSON null. Null is
// therefore the only JSON value this validator cannot represent. Descriptive
// keywords are ignored. Required/closed objects, types, refs, arrays, oneOf/anyOf,
// const, enum, numeric bounds, string/array lengths, and uniqueItems are enforced.
bool ValidateObjectStateSchema (const GS::ObjectState& value,
                                const GS::Optional<GS::UniString>& schemaJson,
                                GS::UniString& error);

// Exercises every supported value-domain keyword against real ObjectState
// values. Add-on initialization calls this so validator regressions fail closed.
bool RunSchemaValidatorSelfCheck (GS::UniString& error);

} // namespace geomsrv

#endif
