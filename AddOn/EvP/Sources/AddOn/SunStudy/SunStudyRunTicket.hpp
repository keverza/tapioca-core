#ifndef TAPIOCA_SUNSTUDY_RUN_TICKET_HPP
#define TAPIOCA_SUNSTUDY_RUN_TICKET_HPP

#include <atomic>
#include <functional>
#include <mutex>

namespace tapioca {

// Cancellation and the atomic display enqueue have one linearization point.
// Once Cancel accepts, no completion of this ticket can publish. Once publication
// commits, Cancel declines rather than claiming to undo an already completed run.
class SunStudyRunTicket {
  public:
    std::atomic<bool> cancelled { false };

    bool Cancel ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (published || cancelled.load ())
            return false;
        cancelled.store (true);
        return true;
    }

    bool Publish (const std::function<bool ()>& enqueue)
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (cancelled.load () || published)
            return false;
        published = enqueue ();
        return published;
    }

  private:
    std::mutex mutex;
    bool published = false;
};

} // namespace tapioca
#endif
