#ifndef TAPIOCA_NATIVE_VIRTUAL_EDITOR_PROJECTION_HPP
#define TAPIOCA_NATIVE_VIRTUAL_EDITOR_PROJECTION_HPP

#include "Edit/EditPath.hpp"
#include <string>

namespace geomsrv::nativecommands {
// Native command schema keeps its established signed numeric-percent response.
// An invalid authored value is a command error, never a NaN success payload.
bool VirtualEditorCrossfallPercent (const edit::PathCrossfall& crossfall, double& percent, std::string& error);
} // namespace geomsrv::nativecommands

#endif
