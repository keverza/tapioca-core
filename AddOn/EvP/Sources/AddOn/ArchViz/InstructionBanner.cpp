// ArchViz/InstructionBanner -- see the header for why the overlay carries an
// instruction at all, and why it holds a deadline rather than a count.

#include "ArchViz/InstructionBanner.hpp"

namespace geomsrv {
namespace archviz {

void InstructionBanner::Set (const std::string& text, double seconds)
{
    std::lock_guard<std::mutex> lock (mutex_);
    text_ = text;
    hasDeadline_ = (seconds >= 0.0);
    if (hasDeadline_) {
        deadline_ = std::chrono::steady_clock::now () +
                    std::chrono::milliseconds (int64_t (seconds * 1000.0));
    }
}

void InstructionBanner::PublishTo (HudState& hud)
{
    std::lock_guard<std::mutex> lock (mutex_);
    hud.instruction = text_;
    hud.instructionSecondsRemaining = -1.0;
    if (!hasDeadline_ || text_.empty ())
        return;

    const double secondsLeft =
        std::chrono::duration<double> (deadline_ - std::chrono::steady_clock::now ()).count ();
    // ⚠️ IT CLEARS ITSELF. A banner that merely stopped counting would sit over
    // the drawing until something else happened to set one -- and the run that
    // most needs this channel is exactly the one most likely to be cancelled
    // halfway through.
    if (secondsLeft <= 0.0) {
        text_.clear ();
        hud.instruction.clear ();
    } else {
        hud.instructionSecondsRemaining = secondsLeft;
    }
}

}   // namespace archviz
}   // namespace geomsrv
