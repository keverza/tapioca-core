#ifndef EVP_ARCHVIZ_DXGI_COMPOSETIMING_HPP
#define EVP_ARCHVIZ_DXGI_COMPOSETIMING_HPP

// ArchViz/Dxgi/ComposeTiming -- what the 3D overlay's composition costs per Present:
// GPU time per stage (D3D11 timestamp queries), CPU time inside `Compose`, and the
// interval between composes, which is Archicad's frame interval while it composes every
// Present. The runtime report prints it as the COST line beside LIVE.
//
// ⚠️ READ private/docs/architecture/diligent/OVERLAY-INVARIANTS.md BEFORE EDITING. Composition
// stays at Present (§2); this only brackets it. §7: what is printed is what was measured
// since the last line -- deltas, never totals -- and a frame not timed is counted, not
// dropped in silence.
//
// ⚠️ IT NEVER WAITS FOR THE GPU. Each frame's queries go into one slot of a ring and are
// read back on later Presents with `D3D11_ASYNC_GETDATA_DONOTFLUSH`; a result not ready
// is read later, and a slot still in flight when its turn comes round leaves that frame
// untimed. A disjoint interval (a clock change) is discarded.
//
// WHY IT EXISTS (2026-10-02): on a detailed project the overlay was laggy, and every Present
// drew 486,417 occluder triangles and 267,922 line segments -- but nothing said how many
// milliseconds of the frame were ours and how many Archicad's own.

#include <cstdint>

struct ID3D11DeviceContext;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace composetiming {

// The stages `overlaycompose::Compose` draws, in its order.
enum class Stage : uint32_t {
    Occluder = 0, // the host occluder's depth (`hostocclusion::Prepare`)
    Host,         // the ghost mesh and the host heatmap and wireframe
    Layers,       // the caller's layers (`layers3d::Draw`)
    Guest,        // the Diligent guest: texts, slices, the HUD (`sceneguest::Draw`)
};
inline constexpr uint32_t kStages = 4;

// RENDER THREAD, from `Compose`, in this order: `BeginFrame`, a `Mark` as each stage ends,
// `EndFrame`. A stage not marked (the user's hide draws the HUD alone) costs nothing.
void BeginFrame (ID3D11DeviceContext* context);
void Mark (ID3D11DeviceContext* context, Stage stage);
void EndFrame (ID3D11DeviceContext* context);

// RENDER THREAD or teardown, once no Present can reach the queries (`overlaycompose::Shutdown`).
void ReleaseDeviceObjects ();

struct Window {
    uint64_t timed = 0;   // frames whose GPU times came back
    uint64_t untimed = 0; // ... their slot still in flight, the interval disjoint, or no queries
    double stageMeanMs[kStages] = {};
    double stageMaxMs[kStages] = {};
    double gpuP50Ms = 0.0, gpuP95Ms = 0.0, gpuMaxMs = 0.0; // the whole composition
    uint64_t cpuFrames = 0;
    double cpuMeanMs = 0.0, cpuMaxMs = 0.0;
    uint64_t intervals = 0; // composes under a second after the previous one
    double intervalP50Ms = 0.0, intervalP95Ms = 0.0;
};

// MAIN THREAD. What was measured since the previous call, and the counts start again.
Window Take ();

// ---- the histogram the percentiles come from: pure, so tests/cpp pins it ----------------
// Bucket upper bounds grow by half each step from 0.1 ms, past a second at the last.
inline constexpr uint32_t kBuckets = 24;
uint32_t BucketOf (uint64_t micros);
uint64_t BucketUpperMicros (uint32_t bucket);
// The upper bound, in microseconds, of the bucket holding the `share` quantile; 0 when empty.
uint64_t Quantile (const uint64_t counts[kBuckets], double share);

} // namespace composetiming
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
