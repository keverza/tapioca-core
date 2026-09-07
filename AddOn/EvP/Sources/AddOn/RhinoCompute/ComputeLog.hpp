#ifndef EVP_RHINOCOMPUTE_COMPUTELOG_HPP
#define EVP_RHINOCOMPUTE_COMPUTELOG_HPP

// logs\rhinocompute.log — beside startup.log, archviz.log and grasshopper.log.
//
// ⚠️ ITS OWN FILE, NOT grasshopper.log, EVEN THOUGH BOTH BACKENDS RUN
// GRASSHOPPER. The two are alternatives that may be up at the same time, each
// with its own worker process, its own generation counter and its own pid. One
// file would interleave two independent restart histories and make "which
// worker died" a question you answer by reading pids instead of by reading the
// file. GhLog.hpp gives the same reason for stamping every line rather than
// appending plain text.
//
// ⚠️ WHY LOGGING IS NOT OPTIONAL HERE. Everything this subsystem gets wrong is
// invisible from inside Archicad: a cold start that takes 90 seconds looks
// identical to one that hung; a worker killed by the job object on quit leaves
// no trace anywhere else; and compute's own failures are HTTP 200 responses with
// an error buried in a JSON array. When a user reports "the preview did not
// appear", this file is the only place that can say whether a worker was ever
// spawned, whether it answered /version, and what it said.
//
// Deliberately mirrors GhLog rather than sharing it: the stamp is the same shape
// so the two files read the same way, and sharing would put RhinoCompute behind
// an include from Grasshopper/ for no gain beyond a dozen lines.

#include "UniString.hpp"

#include <cstdint>

namespace evp {
namespace rhinocompute {

GS::UniString LogPath ();

// One line from the Archicad side. `pid` 0 means "no worker involved" and prints
// as a dash rather than a zero, because a zero pid reads like a bug.
void LogLine (uint32_t generation, uint32_t pid, const GS::UniString& line);

} // namespace rhinocompute
} // namespace evp

#endif
