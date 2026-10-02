#ifndef EVP_ARCHVIZ_MODELCONTENTWATCH_HPP
#define EVP_ARCHVIZ_MODELCONTENTWATCH_HPP

// ArchViz/ModelContentWatch -- the 3D overlay follows what the 3D window SHOWS: isolating a few
// elements, a layer turned on or off, a 3D filter. None of those edits an element, so the
// difference generator (ModelWatch) does not report them, and on a large project it polls every
// 30 s besides; and Archicad has no notification for them (only custom-window and element
// database events).
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md BEFORE EDITING. §5: the overlay
// follows the model by polling what writes nothing -- this reads, never attaches.
//
// ⚠️ THE MODELERAPI MODEL IS ARCHICAD'S LIVE ONE (2026-10-02): the same `ModelerAPI::Model` an
// extraction pass held went from 3889 elements to 7 when the user isolated a few. So one held
// model, asked its element count and three of its elements' GUIDs every tick of the overlay's
// runtime, says when the 3D window's content changed, for a few microseconds -- and the change
// then does what an element edit does (`modelwatch::NoteContentChanged`): a full re-extraction,
// and a new model revision, which is what lets the camera re-pin when the draw it was pinned to
// is gone -- the overlay froze while isolated (195 draws a frame shown, 20 isolated, the pinned
// occurrence among the missing) and only synced again when everything was shown.
//
// ⚠️ ONLY WITH THE 3D WINDOW IN FRONT. Acquiring a model while the 3D is not current can make
// Archicad regenerate it, so the held one is let go when another window comes to the front.
//
// ⚠️ AND DURING A PASS TOO, BUT THEN IT ONLY MOVES THE REVISION. The pass in flight watches its
// own model and starts again when it changes (ExtractionThread); the camera still needs the new
// revision. A pass that FINISHED read the model as it was when it began, so the first idle tick
// after one compares that count with the model's now and re-extracts if they differ.
//
// ⚠️ AND THE SWEEP IS THE DETECTOR: it walks the held model at most 2 ms a tick -- each element's
// change stamp and tessellated vertex count -- and a walk that differs from the previous one sends
// exactly those elements to be updated (`modelwatch::NoteSweepChange`). Measured 12:30-12:31 on
// 3889 elements: a walk costs ~5.4 ms over 3 ticks, it saw a hide 12 s and an edit 10 s before the
// difference generator did, and the generator's poll costs up to 2833 ms. A change of the COUNT
// still moves the revision at once (the camera re-pins); which elements to read is the sweep's.
//
// MAIN THREAD, all of it: the model is DevKit code and dies where it was made.

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace modelcontentwatch {

// What the 3D window's content is taken to be: the model's element count, and the GUIDs at its
// first, middle and last index -- a layer swapped for another of the same size moves one of them.
struct Reading {
    int32_t count = -1;
    std::string first, middle, last;

    bool operator== (const Reading& other) const
    {
        return count == other.count && first == other.first && middle == other.middle && last == other.last;
    }
    bool operator!= (const Reading& other) const
    {
        return !(*this == other);
    }
};

// ⚠️ A CHANGE IS ACTED ON ONCE IT HAS READ THE SAME ON TWO TICKS IN A ROW: a toggle in progress
// is not re-extracted twice, and one that comes straight back is not re-extracted at all. Pure,
// so tests/cpp pins it.
class Settle {
  public:
    static constexpr uint32_t kTicks = 2;

    void Reset (const Reading& baseline)
    {
        baseline_ = baseline;
        pendingTicks_ = 0;
    }
    const Reading& Baseline () const
    {
        return baseline_;
    }

    // True when `now` is a settled change -- and it is then the baseline.
    bool Observe (const Reading& now)
    {
        if (now == baseline_) {
            pendingTicks_ = 0;
            return false;
        }
        if (pendingTicks_ == 0 || now != pending_) {
            pending_ = now;
            pendingTicks_ = 1;
            return false;
        }
        if (++pendingTicks_ < kTicks)
            return false;
        Reset (now);
        return true;
    }

  private:
    Reading baseline_;
    Reading pending_;
    uint32_t pendingTicks_ = 0;
};

// ---- the content sweep -------------------------------------------------------------------------
// What one walk of the held model reads per element (GUID): its change stamp
// (`ModelerAPI::Element::GetGenId`) and its tessellated vertex count.
struct Signature {
    uint32_t genId = 0;
    int64_t vertices = 0;
};
using Signatures = std::unordered_map<std::string, Signature>;

// What differs between two walks. Pure, so tests/cpp pins it.
struct SweepDiff {
    std::vector<std::string> appeared, vanished, stamped, reshaped;
    size_t emptied = 0, filled = 0, stampedAndReshaped = 0;

    bool Any () const
    {
        return !appeared.empty () || !vanished.empty () || !stamped.empty () || !reshaped.empty ();
    }
    // The elements an update must read again: every one that appeared, vanished, or whose stamp
    // or vertices moved -- the shown replaced, the gone removed (ExtractionWorker::StartUpdate).
    std::set<std::string> Changed () const
    {
        std::set<std::string> changed (appeared.begin (), appeared.end ());
        changed.insert (vanished.begin (), vanished.end ());
        changed.insert (stamped.begin (), stamped.end ());
        changed.insert (reshaped.begin (), reshaped.end ());
        return changed;
    }
};

// ⚠️ THE NULL GUID IS NOT AN ELEMENT. Elements with no GUID of their own read as it (12:31:43:
// its stamp moved beside a real edit's); it names nothing an update could read.
inline const char* NullGuid ()
{
    return "00000000-0000-0000-0000-000000000000";
}

inline SweepDiff Compare (const Signatures& before, const Signatures& after)
{
    SweepDiff diff;
    for (const auto& entry : after) {
        if (entry.first == NullGuid ())
            continue;
        const auto was = before.find (entry.first);
        if (was == before.end ()) {
            diff.appeared.push_back (entry.first);
            continue;
        }
        const bool stamp = was->second.genId != entry.second.genId;
        const bool shape = was->second.vertices != entry.second.vertices;
        if (stamp)
            diff.stamped.push_back (entry.first);
        if (shape) {
            diff.reshaped.push_back (entry.first);
            diff.emptied += entry.second.vertices == 0 ? 1 : 0;
            diff.filled += was->second.vertices == 0 ? 1 : 0;
        }
        diff.stampedAndReshaped += stamp && shape ? 1 : 0;
    }
    for (const auto& entry : before)
        if (entry.first != NullGuid () && after.find (entry.first) == after.end ())
            diff.vanished.push_back (entry.first);
    return diff;
}

// Every tick of the 3D overlay's runtime. `threeDInFront`: Archicad's 3D window is the front one.
void Tick (bool threeDInFront);

// Let go of the held model -- at the overlay's stop, and before a project closes (§8).
void Release ();

struct Stats {
    uint32_t changes = 0;  // content changes seen this session
    uint32_t acquires = 0; // baselines taken
    int32_t elements = -1; // the held model's count at its baseline, -1 when none is held
};
Stats Get ();

} // namespace modelcontentwatch
} // namespace archviz
} // namespace geomsrv

#endif
