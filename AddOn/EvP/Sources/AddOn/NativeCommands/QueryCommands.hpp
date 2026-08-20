#ifndef EVP_NATIVECOMMANDS_QUERYCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_QUERYCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E2 — the geometry data plane: Raycast, SliceZ, RaycastAll, RaycastAllBatch,
// ClosestPoint, NearestElements, Query. These are the gate-free commands
// (NeedsMainThread() == false): they read the immutable snapshot and its
// mutex-guarded BVH, never ACAPI, so the dispatcher runs them inline.
//
// QueryCommandBase, their shared setup, deliberately does NOT live in
// CommandBase.hpp: it depends on MeshStore/QueryEngine/QueryIndexCache and only
// these seven commands derive from it. Keeping it file-local in the .cpp is what
// keeps CommandBase.hpp's include surface at almost nothing.
//
NativeCommandRegistrations GetQueryCommandRegistrations ();

} // namespace geomsrv

#endif
