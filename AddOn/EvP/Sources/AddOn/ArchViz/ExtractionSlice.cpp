// ArchViz/ExtractionSlice -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionSlice.hpp"

#include "Geometry/GeometryExtractor.hpp"

#include <Model.hpp>

#include <chrono>

namespace geomsrv {
namespace archviz {
namespace extractionslice {

namespace {

int64_t NowMs ()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
}

} // namespace

// Main-thread capture: record timings without logging inside a host slice.
bool CaptureElementPacket (ModelerAPI::Model& model, int32_t index, std::vector<CapturedMeshPacket>& packets)
{
    CapturedMeshPacket packet;
    const auto started = std::chrono::steady_clock::now ();
    if (!ExtractElementAt (model, index, packet.mesh))
        return false;
    packet.capturedAt = std::chrono::steady_clock::now ();
    packet.captureMilliseconds = std::chrono::duration<double, std::milli> (packet.capturedAt - started).count ();
    packets.push_back (std::move (packet));
    return true;
}

void Run (ModelerAPI::Model& model, int32_t count, SliceState& st, const std::set<std::string>& wanted, int64_t sliceMs,
          bool observeSubstances, const ProjectSubstances& substances)
{
    // The budget is checked INSIDE the slice, on the main thread, so
    // the bound is on how long Archicad is actually held — not on
    // how many elements we guessed would fit.
    //
    // ⚠️ AT LEAST ONE ELEMENT PER SLICE, ALWAYS. A single element
    // can take longer than the whole budget (a curtain wall, a
    // stair), and a loop that checked the clock FIRST would extract
    // nothing, report no progress, and spin forever on that element.
    const int64_t begin = NowMs ();
    int32_t i = st.next.load ();
    // ⚠️ THE MODEL IS ARCHICAD'S LIVE ONE, NOT A COPY: isolating, a layer or a filter
    // changes it between slices, and the rest of the pass read past its end (2026-10-02:
    // 3388 elements). Asked here, where it holds still.
    if (model.GetElementCount () != count) {
        st.changedTo.store (model.GetElementCount ());
        st.completed.store (true);
        return;
    }
    while (i <= count) {
        if (wanted.empty ()) {
            if (!CaptureElementPacket (model, i, st.meshes))
                st.NoteEmpty (ElementTypeNameAt (model, i), EmptyReasonAt (model, i));
        }
        else {
            // ⚠️ THE GUID FIRST, THE GEOMETRY ONLY IF IT MATCHES.
            // Tessellating an element to discover it was not the one
            // that changed is the entire cost of the pass, spent on
            // nothing — which would make a partial refresh as
            // expensive as a full one and leave it no reason to
            // exist.
            const std::string guid = ElementGuidAt (model, i);
            if (!guid.empty () && wanted.count (guid) > 0) {
                // ⚠️ MATCHED ONLY WITH GEOMETRY. An element hidden in 3D can stay in the model's
                // list with nothing to draw (2026-10-02 11:45: three hidden, the count still 3889);
                // counted as seen, it was never removed and its old wireframe stayed on screen.
                if (CaptureElementPacket (model, i, st.meshes))
                    st.matched.push_back (guid);
                else
                    st.NoteEmpty (ElementTypeNameAt (model, i), EmptyReasonAt (model, i));
            }
        }
        // ⚠️ EVERY ELEMENT, NOT ONLY THE TESSELLATED ONES, and it
        // is the same on both pass shapes because the loop already
        // visits every index either way. A vote taken over only the
        // elements a filter happened to match would be a different
        // vote each refresh, so a surface's substance would change
        // as the user edited unrelated parts of the building.
        if (observeSubstances)
            ObserveElementSubstances (model, i, substances, st.observations);
        ++i;
        if (NowMs () - begin >= sliceMs)
            break;
    }
    st.next.store (i);
    st.holdMs.store (NowMs () - begin);
    st.completed.store (true);
}

} // namespace extractionslice
} // namespace archviz
} // namespace geomsrv
