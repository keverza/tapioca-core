#include "ArchViz/SubstanceJoin.hpp"

#include <algorithm>
#include <utility>

namespace geomsrv {
namespace archviz {

std::map<int32_t, SurfaceSubstance> JoinSurfaceSubstances (
    const std::vector<SurfaceSubstanceObservation>& observations, float dominance)
{
    // surface -> (substance -> weight), plus the surface's total weight.
    std::map<int32_t, std::map<int, double>> votes;
    std::map<int32_t, double> totals;

    for (const SurfaceSubstanceObservation& o : observations) {
        // ⚠️ A NON-POSITIVE WEIGHT IS DROPPED, NOT CLAMPED TO ZERO. A body with
        // no polygons carries no evidence either way, and letting it into the
        // denominator would let empty bodies dilute a surface below the
        // dominance bar — which is a refusal caused by nothing at all.
        if (o.weight <= 0.0)
            continue;
        totals[o.surface] += o.weight;
        // Unknown is recorded in `totals` above and deliberately NOT in `votes`:
        // it counts against every candidate and can never win.
        if (o.substance != Substance::Unknown)
            votes[o.surface][static_cast<int> (o.substance)] += o.weight;
    }

    std::map<int32_t, SurfaceSubstance> result;
    for (const auto& entry : totals) {
        const int32_t surface = entry.first;
        const double total = entry.second;
        SurfaceSubstance verdict;

        const auto found = votes.find (surface);
        if (found != votes.end () && total > 0.0) {
            const auto best = std::max_element (
                found->second.begin (), found->second.end (),
                [] (const auto& a, const auto& b) { return a.second < b.second; });
            const double share = best->second / total;
            if (share >= double (dominance)) {
                verdict.substance = static_cast<Substance> (best->first);
                verdict.confidence = float (share);
            }
        }
        // Everything else stays at {Unknown, 0} — the refusal, recorded rather
        // than omitted so a caller can distinguish it from a surface that was
        // never observed at all.
        result[surface] = verdict;
    }
    return result;
}

void ApplySubstanceMemory (const SubstanceMemory& memory, MaterialTable& table)
{
    if (memory.empty ())
        return;
    // ⚠️ A COPY, NOT A REFERENCE TO THE TABLE'S OWN VECTOR. `Set` writes into
    // that vector; iterating it while writing is only safe by the accident that
    // every index here already exists, and an accident is not a guarantee.
    const std::vector<SurfaceMaterial> existingSurfaces = table.All ();
    for (const SurfaceMaterial& existing : existingSurfaces) {
        const auto found = memory.find (existing.index);
        if (found == memory.end () || found->second.first != existing.name)
            continue;
        SurfaceMaterial updated = existing;
        updated.substance = found->second.second.substance;
        updated.substanceConfidence = found->second.second.confidence;
        table.Set (updated);
    }
}

std::unique_ptr<MaterialTable> BuildSubstanceTable (
    const std::vector<SurfaceMaterial>& surfaces,
    const std::vector<SurfaceSubstanceObservation>& observations,
    SubstanceMemory& memory, int& named)
{
    const std::map<int32_t, SurfaceSubstance> joined = JoinSurfaceSubstances (observations);

    // ⚠️ THE MEMORY IS REPLACED, NOT MERGED. A full pass has seen the whole
    // model, so anything the previous one knew and this one did not is a surface
    // that is no longer in the pool -- keeping it would resurrect a verdict for
    // an index that now means something else.
    memory.clear ();
    named = 0;

    auto table = std::make_unique<MaterialTable> ();
    for (SurfaceMaterial surface : surfaces) {
        const auto found = joined.find (surface.index);
        if (found != joined.end ()) {
            surface.substance = found->second.substance;
            surface.substanceConfidence = found->second.confidence;
            if (surface.substance != Substance::Unknown)
                ++named;
        }
        memory[surface.index] =
            std::make_pair (surface.name,
                            SurfaceSubstance { surface.substance, surface.substanceConfidence });
        table->Set (surface);
    }
    return table;
}

} // namespace archviz
} // namespace geomsrv
