#ifndef EVP_NATIVECOMMANDS_SURFACEINSPECTORCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SURFACEINSPECTORCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Stage A of HANDOFF-ExposedSurfaceInspector.md -- the two SDK entry points that
// had NO caller before it:
//
//   * Tapioca.GetExposedSurfaceQuantities wraps ACAPI_Element_GetSurfaceQuantities
//     (the native "exposed surface area" read, with an explicit cover list).
//   * Tapioca.PickPolygon is the one-shot face picker: a start/poll host
//     interaction over ACAPI_UserInput_GetPoint, NEVER a MainThreadCommand's
//     blocking Invoke (an interactive pick holds the main thread for human time).
//
// The command classes live in SurfaceInspectorCommands.cpp. The provider is
// registered through CommandRegistry.cpp's domainProviders[] like every other
// domain (one provider per *Commands.cpp is the architecture gate's rule), so the
// Component3D C ModelAccess family and this Inspector family stay separate files
// under the same command system.
NativeCommandRegistrations GetSurfaceInspectorCommandRegistrations ();

// Drop any in-flight pick and invalidate a completion that has not been reported
// yet. Call on project close / new / open and on add-on unload, from the main
// thread. Safe to call with no request pending; the next pick observes the bumped
// lifecycle generation and cannot publish into the next document.
void ResetSurfaceInspectorState ();

} // namespace geomsrv

#endif
