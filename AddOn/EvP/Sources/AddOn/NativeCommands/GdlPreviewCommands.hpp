#ifndef EVP_NATIVECOMMANDS_GDLPREVIEWCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_GDLPREVIEWCOMMANDS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The GDL-object settled-state preview feed — PLAT-AV-GDLPREVIEW.
//
// ONE COMMAND, Tapioca.GdlPreviewFeed. It places or updates a TapiocaPreview
// object by writing a triangle mesh into that part's ARRAY PARAMETERS, and
// reports the timing seams the benchmark is gated on.
//
// ⚠️ WHY THIS EXISTS AT ALL, in one line: the DXGI overlay draws a foreign scene
// through a foreign camera, so every navigation event has to be observed,
// converted and re-composited, and the result lands a frame or more late. A GDL
// object has no camera to sync — the geometry is in Archicad's model, so
// Archicad navigates it with everything else and the lag is zero BY
// CONSTRUCTION, not by being faster. The overlay stays and keeps the interactive
// drag; this covers the settled state. See
// private/docs/architecture/diligent/HANDOFF-GdlPreview.md sections 0 and 6.
//
// ⚠️ THE MESH IS GENERATED HERE, NOT TRANSPORTED. The benchmark's t0 is "build
// the handles", and shipping 300,000 doubles through the JSON command channel
// would swamp exactly the number being measured — the reading would be of the
// bus, not of GDL. So the caller sends a grid size and gets a deterministic
// torus back; the same size always yields the same mesh, which is what makes a
// run repeatable.
//
// ⚠️ WHAT WAS RULED OUT AND MUST NOT BE REVISITED without an SDK from
// Graphisoft (handoff section 1):
//   * a GDX / REQUEST() bridge — REQUEST is served by IGDLRequests, a pure
//     virtual class with one named method per request and no registration hook;
//     no ACAPI_*Register*/*Install* entry point registers a GDL extension; and
//     no GDL import library ships in Support/Lib;
//   * ACAPI_LibraryPart_ShapePrims as an injector — it HANDS geometry out, as
//     2D API_PrimElement floor-plan primitives. It is a diagnostic, not a route.
//
// MAIN THREAD ONLY, and a write: the dispatcher owns the undo scope, and one
// call is therefore one undo entry. That is the whole reason the production rule
// is "update on settle, never during manipulation" — pushing during a drag would
// mean hundreds of undo steps a second.
NativeCommandRegistrations GetGdlPreviewCommandRegistrations ();

} // namespace geomsrv

#endif
