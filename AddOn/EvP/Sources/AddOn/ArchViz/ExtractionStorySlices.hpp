#ifndef EVP_ARCHVIZ_EXTRACTIONSTORYSLICES_HPP
#define EVP_ARCHVIZ_EXTRACTIONSTORYSLICES_HPP

// ArchViz/ExtractionStorySlices — the storey section overlay's half of the
// extraction pass: read Archicad's storeys, cut every element against each of
// them, union the result and hand it to the renderer.
//
// A separate translation unit for exactly the reason ExtractionEnvironment.hpp
// gives, and along the same seam. `ExtractionThread.cpp` is about SLICING IN
// TIME — the gate hops, the cursor, the budget, the abandonment rules — and
// none of that has anything to do with slicing in SPACE. Keeping the two in one
// file put it back over the ~1,000-line cap the moment this feature landed.
//
// ⚠️ TWO DIFFERENT THREADS, AND THE SPLIT IS NOT NEGOTIABLE. `ReadStoreys` is
// ACAPI and runs on the MAIN thread, inside the pass's acquire gate slice.
// Everything on `StorySliceAccumulator` runs on the extraction WORKER, between
// gate slices, while Archicad has its main thread back.

#include "Geometry/Mesh.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace geomsrv {

struct Polyline;

namespace archviz {

// The project's storeys, as cut planes.
//
// ⚠️ WORLD Z, THE SAME FRAME AS THE GEOMETRY. That is what makes a storey level
// meaningful next to a mesh coordinate at all, and it is the convention
// NativeCommands/ProjectCommands.cpp -> GetStoriesCommand already reports. Two
// readings of "storey level" is a whole building drawn at the wrong height.
struct ProjectStoreys {
    std::vector<double> levels;
    std::vector<std::string> names;

    bool Empty () const
    {
        return levels.empty ();
    }
};

// ⚠️ MAIN THREAD ONLY -- it is ACAPI. Called from inside the pass's acquire gate
// slice, alongside the surface pool and the sun, for the reason stated there:
// all three are small, all three are per-project, and a second gate hop would
// cost a round trip to save nothing.
//
// A failed read returns an EMPTY set and says so in archviz.log rather than
// throwing: the overlay is an annotation, and a building that renders without it
// is a far better outcome than a viewer that refuses to open.
ProjectStoreys ReadStoreys ();

// One pass's worth of storey cuts.
//
// ⚠️ IT ACCUMULATES AND ONLY THEN UNIONS, and that ordering is the whole reason
// this is an object rather than a function. The union is over a storey's ENTIRE
// cross-section (StorySliceUnion.hpp), so there is no per-element contribution
// to push as the pass walks -- a per-element "union" is just that element's own
// outline. Every element is cut into per-storey buckets, and the union runs once
// when the last one has been.
//
// ⚠️ WORKER THREAD ONLY. Nothing here is ACAPI or ModelerAPI; it is pure
// arithmetic over the doubles the pass already extracted, which is precisely why
// it may run while Archicad has its main thread back.
class StorySliceAccumulator final {
  public:
    // Arms the accumulator for a pass. `Active` stays false — and every other
    // call becomes a no-op — when there are no storeys or slices were not asked
    // for, so the caller needs no second flag of its own.
    void Begin (const ProjectStoreys& storeys, bool wanted);
    bool Active () const
    {
        return !planes_.empty ();
    }

    // Cut one element against every storey plane and keep the loops.
    //
    // ⚠️ TAKES THE Mesh, WHICH IS DOUBLE, AND THAT IS LOAD-BEARING. The
    // ElementUpload built beside this narrows the same vertices to float, where a
    // survey-placed project has ~10 cm of resolution — and the union welds at
    // 0.1 mm. Cutting after the narrowing would weld every endpoint to every
    // other one on exactly the projects that matter.
    void Cut (const Mesh& mesh);

    // Union every storey, build the ribbon and the fill, and push one
    // SetStorySlices command. Logs what it cut.
    //
    // ⚠️ THE CALLER MUST ONLY CALL THIS FOR A PASS THAT ACTUALLY FINISHED. A
    // union over PART of a storey is not a rougher outline — it is a confident,
    // clean, WRONG one: the walls the pass never reached simply are not in the
    // building, and nothing about the picture says so.
    void FinishAndPush ();

  private:
    std::vector<std::vector<Polyline>> loops_; // one bucket per storey
    std::vector<double> planes_;
};

} // namespace archviz
} // namespace geomsrv

#endif // EVP_ARCHVIZ_EXTRACTIONSTORYSLICES_HPP
