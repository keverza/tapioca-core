#ifndef EVP_DYNAMOHOST_HPP
#define EVP_DYNAMOHOST_HPP

#include "UniString.hpp"

#include <cstdint>

namespace evp::dynamo {

// Opens the pinned Dynamo 4 editor in its own .NET 10 process. Dynamo 4 cannot
// share Archicad's process-wide .NET 8 runtime with Rhino.Inside.
void OpenFromMenu ();

// Starts the owned, non-visual Dynamo runner asynchronously.
bool StartHeadless ();
GS::UniString HeadlessStatusText ();
bool IsHeadlessReady ();
bool RunHeadlessGraph (const GS::UniString& graphPath, const GS::UniString& paramsJson, uint64_t generation,
                       GS::UniString& result, GS::UniString& error);

// Stops the owned runner and drops the user-owned editor's native handles.
void Release ();

} // namespace evp::dynamo

#endif
