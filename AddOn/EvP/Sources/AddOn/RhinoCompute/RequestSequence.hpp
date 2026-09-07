#ifndef EVP_RHINOCOMPUTE_REQUESTSEQUENCE_HPP
#define EVP_RHINOCOMPUTE_REQUESTSEQUENCE_HPP

// Which solve result is allowed to become the picture on screen.
//
// ⚠️ THIS IS THE WHOLE OF RC-005, AND IT IS A RULE ABOUT NUMBERS RATHER THAN
// ABOUT HTTP. Preview solves are fired from a debounced slider drag, they run
// concurrently, and NOTHING GUARANTEES THEY FINISH IN ORDER — a 4 ms cached
// solve issued second will beat a 120 ms cold one issued first. Whichever
// arrives last wins unless something says otherwise, and what the user sees is
// then the geometry of a value they have already dragged past.
//
// So the ordering lives here, in a class with no socket, no thread and no
// Archicad in it, and the offline suite can prove the property that actually
// matters: a result older than one already accepted can NEVER be drawn.
//
// Deliberately NOT cancellation. SPEC-RhinoCompute.md and the task both say the
// MVP does not need to stop an in-flight solve; it needs to stop a stale one
// from being believed. Those are different problems and this is the cheap one.

#include <cstdint>
#include <mutex>

namespace evp {
namespace rhinocompute {

class RequestSequence {
  public:
    // Hands out the id for a solve about to be issued. Monotonic and never
    // reused within a session.
    uint64_t Next ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return ++issued;
    }

    // The id most recently handed out. What a caller compares against when it
    // wants to know whether it is still the newest request in flight.
    uint64_t Latest () const
    {
        std::lock_guard<std::mutex> lock (mutex);
        return issued;
    }

    // ⚠️ ACCEPTANCE IS ORDERED AGAINST WHAT WAS ALREADY ACCEPTED, NOT AGAINST
    // WHAT WAS ISSUED. Comparing with `issued` would reject every result but the
    // very last one, so a drag whose final solve FAILS would leave the viewport
    // showing nothing rather than the newest result that did succeed. Comparing
    // with `accepted` keeps the best answer that actually arrived.
    //
    // Equal ids are refused too: a result already accepted must not be applied
    // twice, because the preview cache treats a repeat as a change.
    bool Accept (uint64_t requestId)
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (requestId == 0 || requestId <= accepted)
            return false;

        accepted = requestId;
        return true;
    }

    uint64_t Accepted () const
    {
        std::lock_guard<std::mutex> lock (mutex);
        return accepted;
    }

    // True when nothing newer has been issued since this id was, i.e. this
    // request is still the one the user is waiting for. Lets a caller skip work
    // it already knows is pointless — converting a mesh it is about to discard —
    // without changing what Accept would decide.
    bool IsCurrent (uint64_t requestId) const
    {
        std::lock_guard<std::mutex> lock (mutex);
        return requestId != 0 && requestId >= issued;
    }

    // Forgetting what was accepted WITHOUT rewinding what was issued. A new
    // definition means the previous preview is gone rather than superseded, so
    // the next result must be accepted whatever its id — but ids must keep
    // climbing, or a solve still in flight from the old definition would pass
    // the ordering test and draw into the new one.
    void ResetAccepted ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        accepted = 0;
    }

  private:
    mutable std::mutex mutex;
    uint64_t issued = 0;
    uint64_t accepted = 0;
};

} // namespace rhinocompute
} // namespace evp

#endif
