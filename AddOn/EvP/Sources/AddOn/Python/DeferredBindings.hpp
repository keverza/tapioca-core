#pragma once

#include "ObjectState.hpp"

namespace evp {

// Paths are dot-separated ObjectState field names. Array indexing and escaping
// literal dots in field names are deliberately not part of this protocol.
bool ApplyDeferredBinding (const GS::ObjectState& source, const GS::UniString& sourcePath,
                           GS::ObjectState& target, const GS::UniString& targetPath,
                           GS::UniString& error);

} // namespace evp
