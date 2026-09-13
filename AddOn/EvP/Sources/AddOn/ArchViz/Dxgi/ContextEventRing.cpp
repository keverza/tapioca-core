// ArchViz/Dxgi/ContextEventRing -- see the header. Every rule about the
// publish order and the overrun counter is there; this is the mechanism.

#include "ArchViz/Dxgi/ContextEventRing.hpp"

#include "ArchViz/NavLog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace eventring {

namespace {

// ⚠️ BIGGER THAN THE PRESENT RING AND STILL HOLDING LESS TIME. These calls
// arrive in the thousands per frame where Present arrives once, so 16384 slots
// is a second or two rather than half a minute. `FlushContextLogIfFilling` on
// the camera tick is not an optimisation; it is the only thing keeping a run
// from being a record of its own last few frames.
constexpr size_t kRingSize = 16384;
ContextEvent g_ring[kRingSize] = {};
std::atomic<uint64_t> g_reserved {0};
std::atomic<uint64_t> g_published {0};
std::atomic<uint64_t> g_drained {0};
std::atomic<uint64_t> g_dropped {0};

uint64_t MicrosecondsNow ()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!QueryPerformanceFrequency (&frequency) || frequency.QuadPart == 0 ||
        !QueryPerformanceCounter (&counter))
        return 0;
    return uint64_t (counter.QuadPart * 1000000ll / frequency.QuadPart);
}

}   // namespace

void Record (ContextSlot slot, uint64_t handle, uint32_t a, uint32_t b, uint32_t c)
{
    const uint64_t reserved = g_reserved.fetch_add (1, std::memory_order_relaxed);
    const uint64_t drained = g_drained.load (std::memory_order_acquire);
    if (reserved - drained >= kRingSize) {
        g_dropped.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    ContextEvent& event = g_ring[reserved % kRingSize];
    event.timestampUs = MicrosecondsNow ();
    event.slot = uint32_t (slot);
    event.handle = handle;
    event.a = a;
    event.b = b;
    event.c = c;
    // Published AFTER the payload -- see the header.
    g_published.fetch_add (1, std::memory_order_release);
}

size_t Drain (ContextEvent* out, size_t max)
{
    if (out == nullptr || max == 0)
        return 0;
    const uint64_t published = g_published.load (std::memory_order_acquire);
    uint64_t drained = g_drained.load (std::memory_order_relaxed);
    size_t written = 0;
    while (drained < published && written < max) {
        out[written++] = g_ring[drained % kRingSize];
        ++drained;
    }
    g_drained.store (drained, std::memory_order_release);
    return written;
}

void Reset ()
{
    g_reserved.store (0, std::memory_order_release);
    g_published.store (0, std::memory_order_release);
    g_drained.store (0, std::memory_order_release);
    g_dropped.store (0, std::memory_order_relaxed);
}

uint64_t Recorded ()
{
    return g_published.load (std::memory_order_acquire);
}

uint64_t Dropped ()
{
    return g_dropped.load (std::memory_order_relaxed);
}

bool HalfFull ()
{
    const uint64_t published = g_published.load (std::memory_order_acquire);
    const uint64_t drained = g_drained.load (std::memory_order_acquire);
    return (published - drained) >= kRingSize / 2;
}

void Flush ()
{
    // ⚠️ THE TWO CLOCKS ARE TIED TOGETHER HERE, ONCE, exactly as
    // `FlushPresentLog` does it: the ring is stamped with QPC microseconds and
    // every other row in the log is milliseconds since the session header.
    // Anchoring both to "now" per flush -- not per row -- puts these events on
    // the shared timeline with their spacing intact, which is the whole reason
    // they can be correlated with the Present rows at all.
    const uint64_t nowUs = MicrosecondsNow ();
    const uint64_t nowSessionMs = navlog::SessionNowMs ();

    ContextEvent batch[512];
    for (;;) {
        const size_t count = Drain (batch, 512);
        if (count == 0)
            break;
        for (size_t i = 0; i < count; ++i) {
            const ContextEvent& event = batch[i];
            if (event.timestampUs == 0 || event.timestampUs > nowUs)
                continue;
            const uint64_t agoMs = (nowUs - event.timestampUs) / 1000ull;
            const uint64_t sessionMs = (nowSessionMs > agoMs) ? (nowSessionMs - agoMs) : 0;
            navlog::LogContextEvent (sessionMs, ContextSlotName (ContextSlot (event.slot)),
                                     event.handle, event.timestampUs, event.a, event.b,
                                     event.c);
        }
        if (count < 512)
            break;
    }
}

}   // namespace eventring
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
