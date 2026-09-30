#ifndef EVP_ARCHVIZ_OVERLAYCLICKTIMING_HPP
#define EVP_ARCHVIZ_OVERLAYCLICKTIMING_HPP

// ArchViz/OverlayClickTiming -- how long a click keeps Archicad's main thread busy, the
// same way for a press on the overlays' HUD and for one on any other window of the thread:
// a DG palette's checkbox, the view itself.
//
// ⚠️ ONE MEASURE FOR BOTH, OR NO COMPARISON (the user, 2026-09-29: is a DG panel lighter
// than the HUD, or similar?). A HUD press costs a layout, an upload and a redraw of the
// view; a DG checkbox's costs its handler and its repaint. Neither is seen whole from
// inside its own code, and each side's code timing itself would measure different things.
// So the thread is watched instead: from the press, every stretch the main thread is busy
// -- from a message taken off its queue to the moment it next goes idle -- is summed for
// `kWindowMicroseconds`, whatever it did. The HUD's own layout and redraw inside that are
// counted apart too, to say where its share went.
//
// ⚠️ A BUSY THREAD IS NOT ALL THE CLICK'S. Archicad's own timers and idle work land in the
// same half second; one click proves nothing and a comparison is of medians over several.
//
// Pure: timestamps in microseconds come from the caller (OverlayInput.cpp reads them from
// the performance counter in its hooks); tests/cpp builds it. MAIN THREAD, and never
// allocates between `Reset` and `Samples`: it runs inside a message hook.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayclicks {

// What was pressed: the HUD (the press taken from the view it floats on), the view (a
// press the HUD passed to Archicad), or any other window of the thread.
enum class Target : uint8_t { Hud = 0, View = 1, Other = 2 };

const char* TargetName (Target target);

// How long after a press its busy stretches count.
constexpr uint64_t kWindowMicroseconds = 500000;
// The presses kept, the newest.
constexpr size_t kSamples = 64;

struct Sample {
    Target target = Target::Other;
    char windowClass[32] = {}; // the window pressed, as Windows names its class
    uint64_t at = 0;           // the press, microseconds
    bool complete = false;     // its window is over
    uint32_t busyMicroseconds = 0;
    uint32_t firstIdleMicroseconds = 0; // from the press to the thread's first idle; 0: not yet
    uint32_t bursts = 0;                // busy stretches in its window
    uint32_t layoutMicroseconds = 0;    // the HUD laid out again in it...
    uint32_t layouts = 0;
    uint32_t redrawMicroseconds = 0; // ...and the view redrawn for the HUD
    uint32_t redraws = 0;
};

class Meter {
  public:
    // Everything forgotten: armed afresh.
    void Reset ();
    // A message taken off the thread's queue: busy from `now` if it was idle.
    void Wake (uint64_t now);
    // A left press on `target`, whose window's class is `windowClass`: a new sample, and
    // the busy stretch running is this press's from `now`.
    void Press (Target target, const char* windowClass, uint64_t now);
    // The thread is about to go idle: the busy stretch ends.
    void Idle (uint64_t now);
    // The HUD's layout and the view's redraw for it, in the newest press's window.
    void Layout (uint32_t microseconds, uint64_t now);
    void Redraw (uint32_t microseconds, uint64_t now);

    // Oldest first; allocates, so never from a hook.
    std::vector<Sample> Samples (uint64_t now) const;
    uint64_t Idles () const
    {
        return idles_;
    }

  private:
    Sample* Open (uint64_t now);

    Sample samples_[kSamples] = {};
    uint64_t count_ = 0; // presses ever
    bool busy_ = false;
    uint64_t start_ = 0;
    uint64_t idles_ = 0;
};

} // namespace overlayclicks
} // namespace archviz
} // namespace geomsrv

#endif
