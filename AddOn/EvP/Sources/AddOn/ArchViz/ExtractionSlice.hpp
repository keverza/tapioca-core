#ifndef GEOMSRV_ARCHVIZ_EXTRACTIONSLICE_HPP
#define GEOMSRV_ARCHVIZ_EXTRACTIONSLICE_HPP

// ArchViz/ExtractionSlice -- what ONE extraction slice does on the main thread: the elements it
// walks, what each yields, and what it reports back to the pass.
//
// ⚠️ EXTRACTED BECAUSE THE PASS MAY NOT GROW. ExtractionThread.cpp schedules slices -- the gate
// hops, the cursor, the budget, the abandonment rules; this is the work inside one. Moved out
// verbatim, with the state it fills.

#include "ArchViz/ElementPacket.hpp"       // CapturedMeshPacket
#include "ArchViz/ExtractionSubstance.hpp" // ProjectSubstances, SurfaceSubstanceObservation

#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ModelerAPI {
class Model;
}

namespace geomsrv {
namespace archviz {
namespace extractionslice {

// What one extraction slice reports back. ATOMICS and shared BY VALUE, because
// a timed-out Invoke may still run the job LATER, after this loop iteration has
// moved on — the gate's contract says so explicitly. `meshes` is plain (not
// atomic) and is read ONLY when `completed` is true AND the Invoke returned ok;
// a slice that timed out is ABANDONED whole, never harvested, and the shared_ptr
// keeps it alive for the late writer.
struct SliceState {
    std::atomic<int32_t> next { 1 };       // 1-BASED: ModelerAPI indices are
    std::atomic<int32_t> changedTo { -1 }; // the model's count, once it is not the pass's
    std::atomic<uint32_t> empty { 0 };
    // Slice local and touched only by the slice's own main-thread body, so a
    // plain map is correct here where `empty` needs an atomic. Merged into
    // `Progress::emptyByType` under the pass mutex.
    std::map<std::string, uint32_t> emptyByType;
    std::map<std::string, std::map<std::string, uint32_t>> emptyReasons;
    // ⚠️ ONE CALL, THREE FACTS: it drew nothing, it was a <kind>, and why. The count
    // alone cannot tell a dimension from a missing morph, nor a NURBS body from none.
    void NoteEmpty (const std::string& kind, std::string reason)
    {
        empty.fetch_add (1);
        ++emptyByType[kind];
        ++emptyReasons[kind][std::move (reason)];
    }
    std::atomic<int64_t> holdMs { 0 };
    std::atomic<bool> completed { false };
    std::vector<CapturedMeshPacket> meshes;
    // RE51: what this slice saw about which surfaces sit on which substances.
    // ⚠️ PLAIN, LIKE `meshes`, AND HARVESTED ON THE SAME TERMS -- only when the
    // slice completed AND the Invoke returned ok. A timed-out slice is abandoned
    // whole; half its observations would bias the vote it feeds.
    std::vector<SurfaceSubstanceObservation> observations;
    // PARTIAL PASSES ONLY: which of the wanted GUIDs this slice actually saw in
    // the model. What is NOT seen by the end of the pass is what was deleted (or
    // hidden, or moved off the storey), and that is how a removal is derived
    // without interpreting an event id.
    std::vector<std::string> matched;
};

// MAIN THREAD. Capture one element: its packet appended to `packets`, its timings recorded
// without logging inside a host slice. False when it yields no mesh.
bool CaptureElementPacket (ModelerAPI::Model& model, int32_t index, std::vector<CapturedMeshPacket>& packets);

// MAIN THREAD, inside one gate slice: walk from `st.next` for at most `sliceMs` (at least one
// element), every element on a full pass (`wanted` empty) or only those named in `wanted`,
// observing substances when asked; then say where it stopped and that it completed. A model
// whose element count is no longer `count` ends the slice at once with `changedTo` set.
void Run (ModelerAPI::Model& model, int32_t count, SliceState& st, const std::set<std::string>& wanted, int64_t sliceMs,
          bool observeSubstances, const ProjectSubstances& substances);

} // namespace extractionslice
} // namespace archviz
} // namespace geomsrv

#endif
