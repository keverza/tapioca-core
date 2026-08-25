#include "Palette/AutomaticPreviewState.hpp"

namespace evp {

void AutomaticPreviewState::SelectionChanged (Clock::time_point now)
{
    scheduled = true;
    due = now + Delay;
}

bool AutomaticPreviewState::ShouldLaunch (Clock::time_point now, bool eligible, bool anyRunActive) const
{
    return scheduled && !inFlight && eligible && !anyRunActive && now >= due;
}

void AutomaticPreviewState::Started (uint64_t value)
{
    scheduled = false;
    inFlight = true;
    generation = value;
}

bool AutomaticPreviewState::Finished (uint64_t value)
{
    if (!inFlight || value != generation)
        return false;
    inFlight = false;
    generation = 0;
    return true;
}

void AutomaticPreviewState::Cancel ()
{
    scheduled = false;
    inFlight = false;
    generation = 0;
}

} // namespace evp
