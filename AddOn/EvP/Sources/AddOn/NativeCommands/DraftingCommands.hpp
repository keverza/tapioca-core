#ifndef EVP_NATIVECOMMANDS_DRAFTINGCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_DRAFTINGCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// 2D drafting — annotation that sits ON a database rather than in the model:
// CreateText (API_TextID), GetTextElements (its read side) and PlacePicture
// (API_PictureID, the Figure element). Separate from CreateCommands because
// those build MODEL elements from stories and building materials, while these
// share the other concern: a placement point, an anchor and a database.
// GetTextElements lives here rather than with the other reads because text
// content and anchors are this domain's subject, and ElementReadCommands.cpp is
// at its recorded size cap (tools/quality/check_cpp.py OVERSIZED).
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetDraftingCommandRegistrations ();

} // namespace geomsrv

#endif
