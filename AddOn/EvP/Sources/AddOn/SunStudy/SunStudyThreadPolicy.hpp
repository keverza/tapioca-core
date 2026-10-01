#ifndef EVP_SUNSTUDY_SUNSTUDYTHREADPOLICY_HPP
#define EVP_SUNSTUDY_SUNSTUDYTHREADPOLICY_HPP

#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace evp::sunstudy {

inline void NotifyStudyReady (const std::function<void ()>& notification)
{
    try {
        if (notification)
            notification ();
    }
    catch (...) {
        // A failed wake must not kill calculation; heartbeat polling is the fallback.
    }
}

inline bool UseInteractiveStudyPriority ()
{
#ifdef _WIN32
    // CPU priority only. Background-mode memory/I/O priority would also demote
    // pages shared with the renderer/host and can make navigation slower.
    return ::SetThreadPriority (::GetCurrentThread (), THREAD_PRIORITY_BELOW_NORMAL) != 0;
#else
    return false;
#endif
}

} // namespace evp::sunstudy

#endif
