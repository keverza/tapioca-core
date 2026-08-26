#ifndef EVP_GRASSHOPPER_GHLOG_HPP
#define EVP_GRASSHOPPER_GHLOG_HPP

// logs\grasshopper.log, next to startup.log and archviz.log — one file, now with
// two PROCESSES writing to it.
//
// ⚠️ EVERY LINE CARRIES THE WORKER'S PID AND RESTART GENERATION, AND THAT IS THE
// WHOLE REASON THIS IS NOT JUST AppendTextLine.
// HANDOFF-GrasshopperInsideArchicad.md, "Supervision is the point": once a
// worker can be killed and replaced, "the definition hung and the worker was
// restarted" and "the definition never hung" produce identical logs without
// them. The worker does not write the file itself: its lines arrive over the
// bridge as protocol::MessageType::Log and are stamped HERE, so the interleaving
// between the two halves is recorded rather than reconstructed from clocks that
// are not the same clock.

#include "UniString.hpp"

#include <cstdint>

namespace evp {
namespace grasshopper {

GS::UniString LogPath ();

// One line from the Archicad side. `pid` 0 means "no worker involved" and is
// printed as a dash rather than as a zero, because a zero pid reads like a bug.
void LogLine (uint32_t generation, uint32_t pid, const GS::UniString& line);

// One line the worker sent us. Same file, marked so the two halves can be told
// apart at a glance.
void LogWorkerLine (uint32_t generation, uint32_t pid, const GS::UniString& line);

} // namespace grasshopper
} // namespace evp

#endif
