#ifndef EVP_ARCHVIZ_CAMERAWAKE_HPP
#define EVP_ARCHVIZ_CAMERAWAKE_HPP

// Seeing navigation input BEFORE Archicad acts on it (PLAT-RE75).
//
// WHY POLLING CANNOT ANSWER THIS. Everything else in the camera path learns that
// the user is navigating by observing a CONSEQUENCE: the camera differs from last
// tick, or the plan samples came out torn. Both are true only after motion has
// already happened, so the earliest possible reaction is one poll interval late
// -- and a frame has been drawn in the wrong place by then. That is exactly what
// the 2026-08-13 run reported of `hideonnav`: "does not vanish immediately; the
// overlay jumps to some directional location and then disappears". The jump WAS
// the detection.
//
// A `WH_GETMESSAGE` hook runs inside the message pump, on our own thread, BEFORE
// Archicad's window procedure sees the message. A wheel notch or the start of a
// middle-button drag is known at that instant -- ahead of Archicad's own
// response, not behind it.
//
// ⚠️ THE HOOK NEVER CALLS ACAPI. Not "should not" -- it runs at an arbitrary
// point inside Archicad's own message dispatch, re-entrant with whatever it is
// doing. It sets atomics and returns. The camera READ still happens on the timer,
// on the main thread, where it has always been legal.
//
// ⚠️ THREAD-LOCAL, NEVER GLOBAL. `SetWindowsHookEx` with a thread id hooks only
// our own UI thread; a global hook would be injected into every process on the
// desktop, which is a different product with a different risk profile. The
// handoff also rules out `WH_MOUSE_LL` for the same reason -- system-wide scope,
// no benefit here.
//
// ⚠️ IT MUST BE REMOVED BEFORE THE DLL UNLOADS. A hook still installed when the
// add-on goes away is Windows calling into freed code -- the same hazard as a
// surviving window procedure (PlanOverlay crashed Archicad on close once, before
// that rule was written down). `Remove` is idempotent and is called from the mode
// switch's teardown and from `FreeData`.
//
// ⚠️ WHY A POSTED MESSAGE BEATS A FASTER TIMER, which is the whole reason the
// read moved here. `WM_TIMER` is synthesised only when the queue has nothing
// else in it -- it is the lowest-priority message Windows has. During a drag
// Archicad's queue is never empty, so the timer is served last however short its
// interval, and the 2026-08-13 runs measured exactly that: a 15 ms request
// delivering 21 ms at idle and 24-41 ms under drag, barely improved by holding
// timeBeginPeriod(1). A posted private message is an ORDINARY posted message and
// is dispatched ahead of any WM_TIMER, so the sample lands while the drag is
// happening rather than after the queue drains.
//
// THE READ IS COALESCED, NEVER QUEUED. One poll may be outstanding at a time; a
// burst of mouse-moves sets the flag that is already set and posts nothing. A
// queued camera would be a stale camera by the time it was read, and a backlog
// of them would keep the main thread reading ACAPI after the user stopped.
//
// ⚠️ THE TIMER STAYS ARMED UNDERNEATH AS A HEARTBEAT. Input is not the only thing
// that moves an Archicad view -- a zoom animation continues after the wheel notch
// that started it, and a window resize moves the camera with no navigation input
// at all. Without the timer those go unsampled until the user touches the mouse.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace camerawake {

// The ACAPI read to run when navigation input arrives. Called on the MAIN
// THREAD, from the message pump, never from the hook itself. Pass nullptr (or
// simply never call this) to get the blank-only behaviour: `Install` does not
// require a callback and `hideonnav` does not set one.
using PollCallback = void (*) ();
void SetPollCallback (PollCallback callback);

// Whether an input should blank the overlay immediately. Default false, and
// `Remove` resets it.
//
// ⚠️ IT USED TO SAY "`hideonnav` sets this; `wake` must NOT". That rule was right
// about the hazard and wrong about the cause: what must never happen is a blank
// with nobody left to LIFT it, and the lift lives in `ApplyHideOnNavigation`,
// which used to run in one mode only. Since PLAT-RE116 it runs whenever the
// independent `hideOnNav` switch is on, so every hook-installing mode sets this
// from that switch and the lift always has an owner. Set it from
// `CurrentHideOnNav ()`, never from a literal.
void SetBlankOnInput (bool blank);

// MAIN THREAD ONLY -- the hook is bound to the calling thread. False with
// `error` filled if Windows refused; the caller must then not pretend it is
// armed.
bool Install (std::string& error);
void Remove ();
bool Installed ();

// True while a navigation input has been seen within the settle window. This is
// the signal `hideonnav` blanks on, and unlike every other source of the same
// answer it is true on the input itself rather than on its consequence.
bool Navigating (uint32_t settleMs);

// Milliseconds since the last navigation input, or a large number if there has
// never been one.
uint64_t MillisecondsSinceInput ();

struct Stats {
    bool     installed = false;
    uint64_t wheelEvents = 0;
    uint64_t dragEvents = 0;
    uint64_t keyEvents = 0;
    // Posted vs coalesced says whether the coalescing is doing anything. If
    // `coalesced` stays near zero the input rate is below the read rate and the
    // wake path is not the bottleneck; if it dwarfs `posted`, the read is slower
    // than the input arriving and a cheaper read is the next move, not a faster
    // wake.
    uint64_t pollsPosted = 0;
    uint64_t pollsCoalesced = 0;
};
Stats GetStats ();

}   // namespace camerawake
}   // namespace archviz
}   // namespace geomsrv

#endif
