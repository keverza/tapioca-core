// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// See ComposeTiming.hpp.

#include "ArchViz/Dxgi/ComposeTiming.hpp"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace composetiming {

namespace {

// Frames in flight before a slot comes round again: more than the GPU queues.
constexpr uint32_t kRing = 8;
// A stamp before the first stage and one after each.
constexpr uint32_t kStamps = kStages + 1;
// An interval longer than this is a pause between gestures, not a frame.
constexpr uint64_t kFrameIntervalLimitNs = 1000000000ull;

struct Slot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* stamps[kStamps] = {};
    bool pending = false;
};

// RENDER THREAD state.
Slot g_slots[kRing];
uint32_t g_next = 0;
bool g_created = false;
bool g_unavailable = false; // the queries could not be made: every frame untimed, and counted
int g_active = -1;          // the slot this frame writes, -1 when it is not timed
uint32_t g_marked = 0;      // the last stamp issued this frame
int64_t g_cpuStart = 0;
int64_t g_lastBegin = 0;
int64_t g_qpcFrequency = 0;

// ⚠️ WHAT THE MAIN THREAD TAKES: atomics only, no lock on the render thread (§11).
std::atomic<uint64_t> g_timed { 0 };
std::atomic<uint64_t> g_untimed { 0 };
std::atomic<uint64_t> g_stageSumNs[kStages];
std::atomic<uint64_t> g_stageMaxNs[kStages];
std::atomic<uint64_t> g_gpuMaxNs { 0 };
std::atomic<uint64_t> g_gpuBuckets[kBuckets];
std::atomic<uint64_t> g_cpuFrames { 0 };
std::atomic<uint64_t> g_cpuSumNs { 0 };
std::atomic<uint64_t> g_cpuMaxNs { 0 };
std::atomic<uint64_t> g_intervals { 0 };
std::atomic<uint64_t> g_intervalBuckets[kBuckets];

void RaiseMax (std::atomic<uint64_t>& max, uint64_t value)
{
    uint64_t seen = max.load (std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak (seen, value, std::memory_order_relaxed)) {
    }
}

int64_t Now ()
{
    LARGE_INTEGER ticks = {};
    ::QueryPerformanceCounter (&ticks);
    return ticks.QuadPart;
}

uint64_t QpcToNs (int64_t ticks)
{
    if (ticks <= 0 || g_qpcFrequency <= 0)
        return 0;
    return uint64_t (double (ticks) * 1e9 / double (g_qpcFrequency));
}

bool Create (ID3D11DeviceContext* context)
{
    ID3D11Device* device = nullptr;
    context->GetDevice (&device);
    if (device == nullptr)
        return false;
    const D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    const D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP, 0 };
    bool ok = true;
    for (Slot& slot : g_slots) {
        ok = ok && SUCCEEDED (device->CreateQuery (&disjoint, &slot.disjoint));
        for (ID3D11Query*& query : slot.stamps)
            ok = ok && SUCCEEDED (device->CreateQuery (&stamp, &query));
    }
    device->Release ();
    return ok;
}

void ReleaseQueries ()
{
    for (Slot& slot : g_slots) {
        if (slot.disjoint != nullptr)
            slot.disjoint->Release ();
        for (ID3D11Query*& query : slot.stamps) {
            if (query != nullptr)
                query->Release ();
            query = nullptr;
        }
        slot = Slot {};
    }
}

// Every slot whose results are back is recorded and free again; one not back is left
// for a later Present. Never a wait.
void Collect (ID3D11DeviceContext* context)
{
    for (Slot& slot : g_slots) {
        if (!slot.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock = {};
        if (context->GetData (slot.disjoint, &clock, sizeof (clock), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        UINT64 ticks[kStamps] = {};
        bool ready = true;
        for (uint32_t i = 0; i < kStamps && ready; ++i)
            ready =
                context->GetData (slot.stamps[i], &ticks[i], sizeof (UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ready)
            continue;
        slot.pending = false;
        if (clock.Disjoint || clock.Frequency == 0) {
            g_untimed.fetch_add (1, std::memory_order_relaxed);
            continue;
        }
        const double nsPerTick = 1e9 / double (clock.Frequency);
        const auto span = [&] (uint32_t from, uint32_t to) {
            return ticks[to] > ticks[from] ? uint64_t (double (ticks[to] - ticks[from]) * nsPerTick) : 0ull;
        };
        for (uint32_t stage = 0; stage < kStages; ++stage) {
            const uint64_t ns = span (stage, stage + 1);
            g_stageSumNs[stage].fetch_add (ns, std::memory_order_relaxed);
            RaiseMax (g_stageMaxNs[stage], ns);
        }
        const uint64_t total = span (0, kStages);
        RaiseMax (g_gpuMaxNs, total);
        g_gpuBuckets[BucketOf (total / 1000)].fetch_add (1, std::memory_order_relaxed);
        g_timed.fetch_add (1, std::memory_order_relaxed);
    }
}

double Ms (uint64_t ns)
{
    return double (ns) / 1e6;
}

} // namespace

uint64_t BucketUpperMicros (uint32_t bucket)
{
    double upper = 100.0;
    for (uint32_t i = 0; i < bucket; ++i)
        upper *= 1.5;
    return uint64_t (upper + 0.5);
}

uint32_t BucketOf (uint64_t micros)
{
    double upper = 100.0;
    for (uint32_t bucket = 0; bucket + 1 < kBuckets; ++bucket) {
        if (micros <= uint64_t (upper + 0.5))
            return bucket;
        upper *= 1.5;
    }
    return kBuckets - 1;
}

uint64_t Quantile (const uint64_t counts[kBuckets], double share)
{
    uint64_t total = 0;
    for (uint32_t bucket = 0; bucket < kBuckets; ++bucket)
        total += counts[bucket];
    if (total == 0)
        return 0;
    uint64_t wanted = uint64_t (std::ceil (share * double (total)));
    wanted = wanted < 1 ? 1 : (wanted > total ? total : wanted);
    uint64_t seen = 0;
    for (uint32_t bucket = 0; bucket < kBuckets; ++bucket) {
        seen += counts[bucket];
        if (seen >= wanted)
            return BucketUpperMicros (bucket);
    }
    return BucketUpperMicros (kBuckets - 1);
}

void BeginFrame (ID3D11DeviceContext* context)
{
    g_active = -1;
    g_marked = 0;
    if (g_qpcFrequency == 0) {
        LARGE_INTEGER frequency = {};
        ::QueryPerformanceFrequency (&frequency);
        g_qpcFrequency = frequency.QuadPart;
    }
    const int64_t now = Now ();
    if (g_lastBegin != 0) {
        const uint64_t ns = QpcToNs (now - g_lastBegin);
        if (ns < kFrameIntervalLimitNs) {
            g_intervalBuckets[BucketOf (ns / 1000)].fetch_add (1, std::memory_order_relaxed);
            g_intervals.fetch_add (1, std::memory_order_relaxed);
        }
    }
    g_lastBegin = now;
    g_cpuStart = now;

    if (context == nullptr) {
        g_untimed.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    // Made once, on the thread that presents, like every other device object `Compose` draws with.
    if (!g_created && !g_unavailable) {
        g_created = Create (context);
        if (!g_created) {
            ReleaseQueries ();
            g_unavailable = true;
        }
    }
    if (!g_created) {
        g_untimed.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    Collect (context);
    Slot& slot = g_slots[g_next];
    if (slot.pending) { // still in flight a whole ring later: this frame goes untimed
        g_untimed.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    context->Begin (slot.disjoint);
    context->End (slot.stamps[0]);
    g_active = int (g_next);
}

void Mark (ID3D11DeviceContext* context, Stage stage)
{
    if (g_active < 0 || context == nullptr)
        return;
    const uint32_t index = uint32_t (stage) + 1;
    Slot& slot = g_slots[g_active];
    // A stage skipped since the last mark is stamped here too: it took nothing.
    for (uint32_t i = g_marked + 1; i <= index && i < kStamps; ++i)
        context->End (slot.stamps[i]);
    if (index > g_marked)
        g_marked = index;
}

void EndFrame (ID3D11DeviceContext* context)
{
    if (g_active >= 0 && context != nullptr) {
        Slot& slot = g_slots[g_active];
        for (uint32_t i = g_marked + 1; i < kStamps; ++i)
            context->End (slot.stamps[i]);
        context->End (slot.disjoint);
        slot.pending = true;
        g_next = (g_next + 1) % kRing;
    }
    g_active = -1;
    const uint64_t ns = QpcToNs (Now () - g_cpuStart);
    g_cpuFrames.fetch_add (1, std::memory_order_relaxed);
    g_cpuSumNs.fetch_add (ns, std::memory_order_relaxed);
    RaiseMax (g_cpuMaxNs, ns);
}

void ReleaseDeviceObjects ()
{
    ReleaseQueries ();
    g_next = 0;
    g_created = false;
    g_unavailable = false;
    g_active = -1;
    g_lastBegin = 0;
    // §8: the next session reads none of this one's frames.
    Take ();
}

Window Take ()
{
    Window window;
    window.timed = g_timed.exchange (0, std::memory_order_relaxed);
    window.untimed = g_untimed.exchange (0, std::memory_order_relaxed);
    for (uint32_t stage = 0; stage < kStages; ++stage) {
        const uint64_t sum = g_stageSumNs[stage].exchange (0, std::memory_order_relaxed);
        window.stageMeanMs[stage] = window.timed > 0 ? Ms (sum) / double (window.timed) : 0.0;
        window.stageMaxMs[stage] = Ms (g_stageMaxNs[stage].exchange (0, std::memory_order_relaxed));
    }
    uint64_t gpu[kBuckets] = {};
    uint64_t interval[kBuckets] = {};
    for (uint32_t bucket = 0; bucket < kBuckets; ++bucket) {
        gpu[bucket] = g_gpuBuckets[bucket].exchange (0, std::memory_order_relaxed);
        interval[bucket] = g_intervalBuckets[bucket].exchange (0, std::memory_order_relaxed);
    }
    window.gpuP50Ms = double (Quantile (gpu, 0.50)) / 1000.0;
    window.gpuP95Ms = double (Quantile (gpu, 0.95)) / 1000.0;
    window.gpuMaxMs = Ms (g_gpuMaxNs.exchange (0, std::memory_order_relaxed));
    window.cpuFrames = g_cpuFrames.exchange (0, std::memory_order_relaxed);
    const uint64_t cpuSum = g_cpuSumNs.exchange (0, std::memory_order_relaxed);
    window.cpuMeanMs = window.cpuFrames > 0 ? Ms (cpuSum) / double (window.cpuFrames) : 0.0;
    window.cpuMaxMs = Ms (g_cpuMaxNs.exchange (0, std::memory_order_relaxed));
    window.intervals = g_intervals.exchange (0, std::memory_order_relaxed);
    window.intervalP50Ms = double (Quantile (interval, 0.50)) / 1000.0;
    window.intervalP95Ms = double (Quantile (interval, 0.95)) / 1000.0;
    return window;
}

} // namespace composetiming
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
