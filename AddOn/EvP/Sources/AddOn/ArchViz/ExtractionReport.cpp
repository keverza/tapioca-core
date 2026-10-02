// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See ExtractionReport.hpp.

#include "ArchViz/ExtractionReport.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace extractionreport {

EmptyLists DescribeEmpty (const std::map<std::string, uint32_t>& byType,
                          const std::map<std::string, std::map<std::string, uint32_t>>& reasons)
{
    // ⚠️ ORDINARY AND GAP ON SEPARATE LINES, BECAUSE
    // ONE LIST MADE THEM INDISTINGUISHABLE. A 356-element project reported
    // `beam x1, column x1, railing x1, type61 x18, type62 x18` in one string:
    // two real holes in the overlay sitting in a list of eighteen baluster
    // sets that never had a mesh to begin with. Telling those apart needs the
    // Archicad element model, so the code does it rather than the reader.
    //
    // 2D kinds and `+`-prefixed composite PARTS are ordinary. Everything else
    // produced no mesh and should have -- and says why (`EmptyReasonAt`).
    //
    // ⚠️ AND AN ELEMENT WHOSE GEOMETRY IS ANOTHER'S IS NOT A HOLE. Every window (229), door,
    // column, beam, stair, railing and the curtain wall of a 3889-element project read as gaps
    // (2026-10-02) -- `tessellated bodies: no vertices; mesh bodies beside, no polygons` -- while
    // the overlay drew all of them: an opening's geometry is its host wall's, a container's is on
    // its parts (the user). They are CARRIED, and the GAP line keeps only what is missing.
    static const char* const kOrdinary[] = { "dimension", "text", "label", "fill", "change marker" };
    static const char* const kCarried[] = { "window", "door",  "skylight", "opening",    "column",
                                            "beam",   "stair", "railing",  "curtainwall" };
    // Reasons that are a gap whatever the kind: never read, or not an element at all.
    static const char* const kStillGaps[] = { "past the model's end", "invalid" };
    const auto append = [] (std::string& into, const std::string& kind, uint32_t count) {
        if (count == 0)
            return;
        if (!into.empty ())
            into += ", ";
        into += kind + " x" + std::to_string (count);
    };
    EmptyLists lists;
    for (const auto& entry : byType) {
        const bool part = !entry.first.empty () && entry.first[0] == '+';
        bool flat = false;
        for (const char* name : kOrdinary)
            flat = flat || entry.first == name;
        if (part || flat) {
            append (lists.ordinary, part ? entry.first.substr (1) : entry.first, entry.second);
            continue;
        }
        bool carried = false;
        for (const char* name : kCarried)
            carried = carried || entry.first == name;
        const auto why = reasons.find (entry.first);
        std::vector<std::pair<std::string, uint32_t>> ranked;
        if (why != reasons.end ())
            ranked.assign (why->second.begin (), why->second.end ());
        uint32_t missing = entry.second;
        if (carried) {
            // What the model changed away from under the pass was never read, and an invalid
            // element is carried by nothing: still gaps.
            uint32_t stillGaps = 0;
            std::vector<std::pair<std::string, uint32_t>> kept;
            for (const auto& reason : ranked) {
                bool gap = false;
                for (const char* prefix : kStillGaps)
                    gap = gap || reason.first.rfind (prefix, 0) == 0;
                if (gap) {
                    stillGaps += reason.second;
                    kept.push_back (reason);
                }
            }
            append (lists.carried, entry.first, entry.second - std::min (stillGaps, entry.second));
            missing = std::min (stillGaps, entry.second);
            ranked = kept;
        }
        if (missing == 0)
            continue;
        append (lists.gaps, entry.first, missing);
        if (ranked.empty ())
            continue;
        std::stable_sort (ranked.begin (), ranked.end (),
                          [] (const auto& a, const auto& b) { return a.second > b.second; });
        lists.gaps += " (";
        for (size_t i = 0; i < ranked.size (); ++i)
            lists.gaps += (i > 0 ? ", " : "") + ranked[i].first + " x" + std::to_string (ranked[i].second);
        lists.gaps += ")";
    }
    return lists;
}

void Pass (const ExtractionWorker::Progress& progress, bool partial, size_t elementsSeen, uint32_t removed)
{
    const ExtractionWorker::Progress& summary = progress;
    // Printed only when something drew nothing, so a clean pass stays quiet.
    if (!summary.emptyByType.empty ()) {
        const EmptyLists lists = DescribeEmpty (summary.emptyByType, summary.emptyReasons);
        if (!lists.ordinary.empty ())
            ArchVizLog ("extraction: no mesh, ORDINARY (2D, or a part with nothing of its own to draw) - " +
                        lists.ordinary);
        if (!lists.carried.empty ())
            ArchVizLog ("extraction: no mesh, CARRIED (an opening's geometry is its host's; a column's, beam's, "
                        "stair's, railing's or curtain wall's is on its parts) - " +
                        lists.carried);
        if (!lists.gaps.empty ())
            ArchVizLog ("extraction: no mesh, GAP - " + lists.gaps + "  (solid elements the overlay is not drawing)");
    }

    ArchVizLog ("extraction: " + summary.phase + (partial ? " (partial)" : " (full)") + " - " +
                std::to_string (summary.pushed) + "/" + std::to_string (elementsSeen) + " elements, " +
                std::to_string (removed) + " removed, " + std::to_string (summary.triangles) + " triangles, " +
                std::to_string (summary.materials) + " surfaces, " + std::to_string (summary.slices) +
                " slices, longest hold " + std::to_string (summary.longestHoldMs) + " ms, acquire " +
                std::to_string (summary.acquireMs) + " ms, total " + std::to_string (summary.elapsedMs) + " ms");
}

void SinglePassNotice ()
{
    ArchVizLog ("extraction: live sync is running as a SINGLE PASS - per-element "
                "change observers are disabled because attaching them writes a link "
                "into the project database and makes Archicad autosave continuously "
                "(PLAT-RE68). The scene will not follow edits unless the model watch "
                "is armed; see ModelWatch.hpp.");
}

void SubstanceJoin (size_t named, size_t surfaces, uint32_t classified, uint32_t total, size_t observations,
                    const std::string& error)
{
    ArchVizLog ("extraction: substance join - " + std::to_string (named) + "/" + std::to_string (surfaces) +
                " surfaces named from " + std::to_string (classified) + "/" + std::to_string (total) +
                " classified building materials, " + std::to_string (observations) + " observations" +
                (error.empty () ? std::string () : std::string (" (") + error + ")"));
}

} // namespace extractionreport
} // namespace archviz
} // namespace geomsrv
