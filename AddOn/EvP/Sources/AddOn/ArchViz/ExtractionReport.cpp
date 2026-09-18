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
        std::string kinds;
        for (const auto& entry : summary.emptyByType) {
            if (!kinds.empty ())
                kinds += ", ";
            kinds += entry.first + " x" + std::to_string (entry.second);
        }
        ArchVizLog ("extraction: no mesh from - " + kinds +
                    "  (2D kinds here are ordinary; a solid kind here is a gap)");
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
