#ifndef EVP_COMMANDCONTROLDISPATCH_HPP
#define EVP_COMMANDCONTROLDISPATCH_HPP

#include "ObjectState.hpp"
#include "UniString.hpp"

namespace evp {

// Handles shared command discovery, execution, run-state and server-control verbs.
// Returns false for other verbs, leaving the central dispatcher to route them.
bool DispatchCommandControlVerb (const GS::UniString& backend, const GS::UniString& name, const GS::UniString& command,
                                 const GS::ObjectState& params, GS::ObjectState& data, GS::ObjectState& error,
                                 bool& ok);

} // namespace evp

#endif
