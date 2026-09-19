// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// ArchViz/OverlayRuntimeReport -- everything the injected overlay SAYS about
// itself, and nothing it does.
//
// ⚠️ THIS IS A SEPARATE CONCERN AND THE FILE SIZES SAID SO
// TWICE. `InjectedOverlayRuntime` grew past its cap on two consecutive commits,
// both times because the report grew, not because the runtime did. The runtime
// decides; this decides what a reader is told and how often.
//
// The rules the report obeys, which are contract and not style:
//
//   ONE LINE PER TRANSITION, NEVER PER TICK. A state that has not moved is not
//   news. If it stops, the last line is the diagnosis.
//
//   DELTAS, NOT TOTALS. A cumulative counter answers "did this ever happen";
//   every question worth asking after the overlay starts is "is it happening
//   NOW". `OVERLAY DRAWING` once read identically for "composing every frame"
//   and "composed once, ten minutes ago".
//
//   NO SILENT TRUNCATION. The line buffer must hold the whole line: 256 cut the
//   LIVE report exactly where `suppressed+` and `redraw=` began, which are the
//   numbers the resize acceptance criteria are read from.
//
// MAIN THREAD ONLY, from the runtime heartbeat.

#ifndef GEOMSRV_ARCHVIZ_OVERLAYRUNTIMEREPORT_HPP
#define GEOMSRV_ARCHVIZ_OVERLAYRUNTIMEREPORT_HPP

#include "ArchViz/InjectedOverlayRuntime.hpp"

#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayruntime {
namespace report {

// One channel line. The channel is padded so the log reads as columns.
void Say (const char* channel, const std::string& detail);

// The per-tick live picture: rates, both viewports, every refusal by reason.
void Live (const Health& health);

// While the overlay is requested but not drawing: the whole stage chain, the
// stage it is blocked at, and -- when the gate is what refused -- the
// measurements the gate was applied to, so a threshold can be argued with.
void Chain (const Health& health);

// Whether the model watch is looking, what it has seen, and whether the host
// snapshot has caught up. One line per CHANGE, like everything else here.
void Watch (const Health& health);

// The Diligent boundary, and SILENT while the native backend is in use -- a line
// that prints "not using Diligent" every tick is a line nobody reads.
void Backend (const Health& health);

// Who is keeping the runtime alive, and how long a blank frame waited for the
// redraw that ends it. On a CHANGE, like the rest of these.
//
// ⚠️ IT EXISTS BECAUSE "THE MENU HIDES ON NAVIGATION AND
// THE COMMAND DOES NOT" HAD NO INSTRUMENT. Both arm `hookdiag at 33 ms,
// hideOnNav off, gpuState on` -- the log says so sixty-seven times -- so the
// arming is not the difference and reading the rendering code again cannot find
// it. What differs is that `Tapioca.OverlayRuntime` calls `Tick` and a menu
// click does not, which is section 9 of OVERLAY-INVARIANTS.md if it turns out to
// matter. This is the line that decides whether it does.
void Pulse (const Health& health);

// At the start of a session. See OVERLAY-INVARIANTS.md section 8.
void Reset ();

} // namespace report
} // namespace overlayruntime
} // namespace archviz
} // namespace geomsrv

#endif
