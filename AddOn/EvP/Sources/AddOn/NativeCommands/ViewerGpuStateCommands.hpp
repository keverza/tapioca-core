#ifndef EVP_NATIVECOMMANDS_VIEWERGPUSTATECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_VIEWERGPUSTATECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// THE GPU-STATE DISCOVERY VERBS (PLAT-RE153..RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md).
//
// WHY THEY ARE NOT IN ViewerSyncCommands. That file owns HOW THE VIEWER FOLLOWS
// ARCHICAD -- camera, geometry, selection -- through Archicad's public API.
// These verbs ask a different question with a different mechanism and a
// different risk: what did Archicad hand to the GPU for the frame it is drawing
// right now. They pin a build by hash and refuse to run on any other, they
// install and repair vtable detours on Archicad's own D3D11 context, and they
// turn the user's 3D camera. Nothing here is part of the following contract, and
// mixing them made one file grow past the size cap while the two halves changed
// on completely different schedules.
//
// ⚠️ EVERY VERB HERE IS RECOVERABLE FROM `CameraSyncReset`, and that is not an
// afterthought -- see ArchViz/AutoOrbit.hpp and ArchViz/ContextHook.hpp. A
// cancelled run cannot clean up after itself, because after a Stop the bus
// refuses the calls its own `finally` would make.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetViewerGpuStateCommandRegistrations ();

} // namespace geomsrv

#endif
