#ifndef EVP_NATIVECOMMANDS_VIEWERSYNCCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_VIEWERSYNCCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// HOW THE VIEWER FOLLOWS ARCHICAD — camera, geometry and selection — as opposed
// to the viewer itself, which is ArchVizCommands.
//
// The boundary is worth stating because it is the one that keeps being crossed.
// Three separate things can track Archicad, they are switched independently, and
// each is a different mechanism with a different cost:
//
//   camera      `SetCameraSyncMode` and the poll behind it. Continuous, and
//               deliberately OVERLAY-ONLY (PLAT-RE124) — the panel takes one
//               camera at open and then navigates standalone, so
//               `SyncDiligentCameraOnce` is how it gets back.
//   geometry    the difference generator, polled (PLAT-RE125), plus the manual
//               `RefreshDiligentModel`. It writes nothing to the project, which
//               is the whole reason it replaced per-element observers.
//   selection   `SetDiligentSelectionBridge`, two independent directions.
//
// `ViewerNavLog`/`NavLogMark` live here because what they measure IS the
// following: a nav log with no sync running has nothing to say.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetViewerSyncCommandRegistrations ();

} // namespace geomsrv

#endif
