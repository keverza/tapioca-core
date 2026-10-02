#ifndef EVP_SUNSTUDY_SUNSTUDYREFRESHSCHEDULE_HPP
#define EVP_SUNSTUDY_SUNSTUDYREFRESHSCHEDULE_HPP

#include <cstdint>

namespace evp::sunstudy {

// Keep one latest geometry target, never a queue of snapshots to calculate.
// Repeated edits need a window longer than ModelWatch's usual 750 ms cadence;
// isolated edits retain the short delay. Navigation does not change the target.
class SunStudyRefreshSchedule {
  public:
    void Reset (uint32_t edits)
    {
        observed_ = edits;
        pending_ = false;
        quietSince_ = 0;
        batchSince_ = 0;
        pendingSignals_ = 0;
        pendingObservations_ = 0;
        burstSignals_ = 0;
        quietMilliseconds_ = 300;
    }
    bool Observe (uint32_t edits, int64_t now)
    {
        if (edits == observed_)
            return false;
        const uint32_t signals = edits - observed_; // unsigned counter wrap is intentional
        if (burstSignals_ == 0 || now - quietSince_ > 2000)
            burstSignals_ = 0;
        const uint32_t added = signals < 3 ? signals : 3;
        burstSignals_ = burstSignals_ + added < 3 ? burstSignals_ + added : 3;
        quietMilliseconds_ = burstSignals_ == 1 ? 300 : burstSignals_ == 2 ? 750 : 1500;
        if (!pending_) {
            batchSince_ = now;
            pendingSignals_ = 0;
            pendingObservations_ = 0;
        }
        pendingSignals_ += signals;
        ++pendingObservations_;
        observed_ = edits;
        quietSince_ = now;
        pending_ = true;
        return true;
    }
    bool Pending () const
    {
        return pending_;
    }
    bool Ready (int64_t now, bool navigating) const
    {
        return pending_ && !navigating && MillisecondsUntilReady (now) == 0;
    }
    int64_t MillisecondsUntilReady (int64_t now) const
    {
        if (!pending_)
            return -1;
        const int64_t remaining = quietMilliseconds_ - (now - quietSince_);
        return remaining > 0 ? remaining : 0;
    }
    int64_t QuietMilliseconds () const
    {
        return quietMilliseconds_;
    }
    int64_t LastEditMs () const
    {
        return quietSince_;
    }
    int64_t BatchStartedMs () const
    {
        return batchSince_;
    }
    uint64_t PendingSignals () const
    {
        return pendingSignals_;
    }
    uint64_t PendingObservations () const
    {
        return pendingObservations_;
    }
    void Complete ()
    {
        pending_ = false;
        pendingSignals_ = 0;
        pendingObservations_ = 0;
    }

  private:
    uint32_t observed_ = 0;
    int64_t quietSince_ = 0;
    int64_t batchSince_ = 0;
    int64_t quietMilliseconds_ = 300;
    uint64_t pendingSignals_ = 0;
    uint64_t pendingObservations_ = 0;
    uint32_t burstSignals_ = 0;
    bool pending_ = false;
};

} // namespace evp::sunstudy

#endif
