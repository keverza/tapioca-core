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

// CPU time this thread has used, in milliseconds; 0 where unavailable. Set
// beside a wall-clock interval it tells a slow traversal from a STARVED one:
// the study worker runs below normal priority (#97 P0).
inline double CurrentThreadCpuMilliseconds ()
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (::GetThreadTimes (::GetCurrentThread (), &created, &exited, &kernel, &user) == 0)
        return 0.0;
    const auto ticks = [] (const FILETIME& time) {
        return (unsigned long long) (time.dwHighDateTime) << 32 | time.dwLowDateTime;
    };
    return double (ticks (kernel) + ticks (user)) / 10000.0; // 100 ns ticks
#else
    return 0.0;
#endif
}

} // namespace evp::sunstudy

#endif
