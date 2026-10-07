#ifndef EVP_ARCHVIZ_HUDDEBUGCOUNTERS_HPP
#define EVP_ARCHVIZ_HUDDEBUGCOUNTERS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace geomsrv::archviz::huddebug {
constexpr uint64_t kWindowMilliseconds = 60000;
inline uint64_t Now ()
{
    return uint64_t (
        std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now ().time_since_epoch ())
            .count ());
}
// Display-only baselines: lifetime counters still drive rates, revisions and
// camera progress. Resetting those producers would invalidate other consumers.
template <size_t Count> class CounterWindow {
  public:
    std::array<uint64_t, Count> Observe (uint64_t now, const std::array<uint64_t, Count>& totals)
    {
        if (!known || now < lastTime || now - started >= kWindowMilliseconds) {
            known = true;
            started = now;
            baseline = totals;
        }
        std::array<uint64_t, Count> shown {};
        for (size_t i = 0; i < Count; ++i) {
            // Producers can restart independently on a viewport/session change.
            if (totals[i] < last[i])
                baseline[i] = totals[i];
            shown[i] = totals[i] - baseline[i];
        }
        last = totals;
        lastTime = now;
        return shown;
    }

  private:
    bool known = false;
    uint64_t started = 0, lastTime = 0;
    std::array<uint64_t, Count> baseline {}, last {};
};
} // namespace geomsrv::archviz::huddebug
#endif
