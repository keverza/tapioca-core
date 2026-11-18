#ifndef EVP_ARCHVIZ_DIAGRAMMODEL_HPP
#define EVP_ARCHVIZ_DIAGRAMMODEL_HPP
// The live exploded diagram (#201 M1): the published massing snapshot, followed on the main
// thread, built on a worker and published with one atomic swap.
//
// Threads:
//  - MAIN THREAD: Follow and Forget. Follow reads massingslicesmodel::Read and ReadStoreys
//    (ACAPI), captures one DiagramInputSnapshot and hands it to a worker; a completed build
//    is adopted only when it answers the latest request -- a superseded one is discarded
//    whole, never partially applied, and a refused snapshot leaves the last coherent
//    publication in place.
//  - ANY THREAD: controls, the publication, the hidden-source set and the status are values
//    behind atomics or a mutex. The viewer's renderer-owned HUD sets controls; the scene
//    reads the hidden set; the diagram draw reads the publication.
//
// Read-only (§5.9): nothing here writes the model, metadata or an undo step. Hiding the
// source is a presentation policy of the viewer's scene, not an element change.
#include "ArchViz/DiagramView.hpp"
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz::diagrammodel {
struct Controls {
    bool enabled = false;     // Diagram Mode in the viewer
    diagram::Spacing spacing; // storey gap in metres, and per-building overrides (#201 2026-11-18)
    bool hideSource = true;   // hide the represented massing slabs while the diagram is shown
    diagram::Colouring colouring = diagram::Colouring::Function;
    diagram::Detail detail = diagram::Detail::Topology;
    auto operator<=> (const Controls&) const = default;
};
Controls ReadControls ();
void SetControls (const Controls& controls);
// ANY THREAD: this frame's input for DiligentScene::DrawDiagram; no publication while
// Diagram Mode is off.
diagram::DiagramView View ();

std::shared_ptr<const diagram::Publication> Read ();
// The represented source GUIDs to hide, sorted; null when nothing is hidden (Diagram Mode
// off, hide off, or no publication yet -- never hide a source with nothing standing in).
std::shared_ptr<const std::vector<std::string>> HiddenSource ();

// The diagram's own selection (§5.9): one storey or none. Diagram-owned, so the scene's
// world-space selection is cleared when it is set -- one highlight source at a time. It
// survives a new publication only while its storey binding does; otherwise it is cleared
// with a note, never moved to another storey.
diagram::EntityId Selected ();
void Select (const diagram::EntityId& id); // ANY THREAD; empty clears

struct Status {
    std::string state; // one line for the HUD
    std::vector<std::string> notes;
    bool building = false;
    uint64_t publications = 0, discarded = 0, refused = 0;
    // Republished inputs whose content matched the last request: no build, no upload.
    uint64_t unchanged = 0;
    std::string savedPath, saveError;
    // Reported by the renderer: geometry uploads since its layer started, their bytes, the
    // last frame's draws and the storey gap on screen. Uploads stay flat while it animates.
    uint64_t uploads = 0;
    size_t uploadedBytes = 0, draws = 0;
    double shownGap = 0;
};
Status ReadStatus ();
// Write the last captured input as a replayable JSON file (DiagramReplay). The main thread
// does the IO on its next Follow; the result lands in Status::savedPath / saveError.
void RequestSave ();
// RENDER THREAD: what the last diagram frame did.
void ReportDraw (uint64_t uploads, size_t uploadedBytes, size_t draws, double shownGap);

void Follow ();
void Forget ();
} // namespace geomsrv::archviz::diagrammodel
#endif
