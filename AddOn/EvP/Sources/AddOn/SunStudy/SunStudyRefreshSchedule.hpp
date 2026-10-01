#ifndef EVP_SUNSTUDY_SUNSTUDYREFRESHSCHEDULE_HPP
#define EVP_SUNSTUDY_SUNSTUDYREFRESHSCHEDULE_HPP

#include <cstdint>

namespace evp::sunstudy {

// Coalesce notifications before expensive SDK capture, not just before tracing.
// Navigation delays capture without changing the study's dependency signature.
class SunStudyRefreshSchedule {
  public:
    void Reset (uint32_t edits)
    {
        observed_ = edits;
        pending_ = false;
    }
    bool Observe (uint32_t edits, int64_t now)
    {
        if (edits == observed_)
            return false;
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
        return pending_ && !navigating && now - quietSince_ >= 300;
    }
    void Complete ()
    {
        pending_ = false;
    }

  private:
    uint32_t observed_ = 0;
    int64_t quietSince_ = 0;
    bool pending_ = false;
};

} // namespace evp::sunstudy

#endif
