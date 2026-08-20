#ifndef EVP_WEBUIDISPATCH_HPP
#define EVP_WEBUIDISPATCH_HPP

#include "ObjectState.hpp"
#include "UniString.hpp"

namespace evp {

// Handles the native control verbs used by the WebUI palette. Returns false when
// the verb is not part of this surface, leaving the central dispatcher to route it.
bool DispatchWebUIVerb (const GS::UniString& backend, const GS::UniString& name, const GS::UniString& command,
                        const GS::ObjectState& params, GS::ObjectState& data, GS::ObjectState& error, bool& ok);

} // namespace evp

#endif
