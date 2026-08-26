#ifndef EVP_GRASSHOPPER_GRASSHOPPERBRIDGE_HPP
#define EVP_GRASSHOPPER_GRASSHOPPERBRIDGE_HPP

// The managed-to-native direction of the Rhino.Inside host: one function, which
// runs a Tapioca native command on the calling thread and hands back JSON.
//
// ⚠️ WHY THIS EXISTS INSTEAD OF AN HTTP ENDPOINT. Measured 2026-08-26 from
// inside a live Grasshopper solution: while a solve holds Archicad's main thread
// inside an add-on callback, Archicad executes NO JSON command that needs that
// thread — not Tapir's, not Tapioca's, and not Graphisoft's own
// API.GetSelectedElements, from any thread. Only API.IsAlive survives, because
// Archicad answers it without scheduling. Pumping messages does not help: a
// callback cannot return to the loop it is standing on. So an in-process
// Grasshopper component CANNOT reach Archicad over a socket, ever.
//
// The same fact read the other way is why this bridge is trivial. A Grasshopper
// component solves ON Archicad's main thread, which is the thread ACAPI demands.
// There is nothing to schedule and nothing to wait for: the command runs inline
// and returns in the same stack frame. No socket, no queue, no timeout, and none
// of the ~2 s per-call dispatch cost the JSON port charges.
//
// See GrasshopperHostApi.h for the POD contract and the status codes.

#include "GrasshopperHostApi.h"

namespace evp {
namespace grasshopper {

// The table handed to the managed half at start. Its lifetime is the add-on's;
// the managed side must not retain the pointer past Stop, and GrasshopperHost
// revokes it there.
const TapiocaGhNativeApi* NativeApi ();

// Stops serving calls. After this, every call returns TapiocaGhStatus_NotRunning
// instead of touching ACAPI — the same refuse-first discipline the rest of the
// host uses, applied to the one entry point a foreign runtime holds a pointer to.
void RevokeNativeApi ();

} // namespace grasshopper
} // namespace evp

#endif
