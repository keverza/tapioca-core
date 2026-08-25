#ifndef EVP_PALETTE_AUTOMATICPREVIEWSTATE_HPP
#define EVP_PALETTE_AUTOMATICPREVIEWSTATE_HPP

#include <chrono>
#include <cstdint>

namespace evp {

// DevKit-free scheduling policy. DG events supply mutations and idle supplies time;
// this object decides only whether one coalesced launch is due.
class AutomaticPreviewState {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds Delay { 350 };

    void SelectionChanged (Clock::time_point now);
    bool ShouldLaunch (Clock::time_point now, bool eligible, bool anyRunActive) const;
    void Started (uint64_t generation);
    bool Finished (uint64_t generation);
    void Cancel ();
    bool IsInFlight () const
    {
        return inFlight;
    }

  private:
    bool scheduled = false;
    bool inFlight = false;
    Clock::time_point due {};
    uint64_t generation = 0;
};

} // namespace evp

#endif
