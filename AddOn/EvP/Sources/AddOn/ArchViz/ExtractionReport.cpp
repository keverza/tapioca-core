// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// See ExtractionReport.hpp.

#include "ArchViz/ExtractionReport.hpp"

#include "ArchViz/ArchVizLog.hpp"

namespace geomsrv {
namespace archviz {
namespace extractionreport {

void Pass (const ExtractionWorker::Progress& progress, bool partial, size_t elementsSeen, uint32_t removed)
{
    const ExtractionWorker::Progress& summary = progress;
    // Printed only when something drew nothing, so a clean pass stays quiet.
    if (!summary.emptyByType.empty ()) {
        // ⚠️ ORDINARY AND GAP ON SEPARATE LINES, BECAUSE
        // ONE LIST MADE THEM INDISTINGUISHABLE. A 356-element project reported
        // `beam x1, column x1, railing x1, type61 x18, type62 x18` in one string:
        // two real holes in the overlay sitting in a list of eighteen baluster
        // sets that never had a mesh to begin with. Telling those apart needs the
        // Archicad element model, so the code does it rather than the reader.
        //
        // 2D kinds and `+`-prefixed composite PARTS are ordinary. Everything else
        // produced no mesh and should have.
        static const char* const kOrdinary[] = { "dimension", "text", "label", "fill", "change marker" };
        std::string ordinary;
        std::string gaps;
        for (const auto& entry : summary.emptyByType) {
            const bool part = !entry.first.empty () && entry.first[0] == '+';
            bool flat = false;
            for (const char* name : kOrdinary)
                flat = flat || entry.first == name;
            std::string& into = (part || flat) ? ordinary : gaps;
            if (!into.empty ())
                into += ", ";
            into += (part ? entry.first.substr (1) : entry.first) + " x" + std::to_string (entry.second);
        }
        if (!ordinary.empty ())
            ArchVizLog ("extraction: no mesh, ORDINARY (2D, or a part whose owner carries it) - " + ordinary);
        if (!gaps.empty ())
            ArchVizLog ("extraction: no mesh, GAP - " + gaps + "  (solid elements the overlay is not drawing)");
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
