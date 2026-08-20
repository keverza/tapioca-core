#ifndef EVP_NATIVECOMMANDS_SELECTIONCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SELECTIONCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E3 — selection & highlight: GetSelection, SetSelection, HighlightElements,
// ClearHighlights, ZoomTo.
//
// The command classes stay in the .cpp's anonymous namespace; only their ordered
// registration view is exported.
NativeCommandRegistrations GetSelectionCommandRegistrations ();

} // namespace geomsrv

#endif
