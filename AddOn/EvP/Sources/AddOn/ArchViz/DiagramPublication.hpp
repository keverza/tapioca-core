#ifndef EVP_ARCHVIZ_DIAGRAMPUBLICATION_HPP
#define EVP_ARCHVIZ_DIAGRAMPUBLICATION_HPP
// One immutable, coherent diagram: scene, plan, draw source and pick meshes from one
// snapshot (#201 §5.10). Built on a worker, published with one atomic swap, never edited
// after: a newer snapshot makes a new publication, and a refused one makes none, so the
// renderer always holds the last coherent diagram.
//
// Pure: the live wiring (DiagramModel) and the offline replay share this one step.
#include "ArchViz/DiagramGeometry.hpp"

namespace geomsrv::archviz::diagram {
struct Publication {
    uint64_t generation = 0; // the request that produced it; a newer request supersedes it
    RevisionSet revisions;
    StoreyDomain domain = StoreyDomain::MassingFloors;
    std::shared_ptr<const Scene> scene;
    LayoutPlan plan;
    DiagramGeometry geometry;
    std::vector<PickMesh> shells;
    std::vector<std::string> representedGuids; // sorted; what "hide source" hides
    std::vector<std::string> notes;
    size_t buildings = 0, storeys = 0;
    double buildMs = 0;
};

struct Prepared {
    std::shared_ptr<const Publication> publication; // null when the snapshot was refused
    StoreyBindings bindings;                        // adopt only with the publication
    SnapshotStatus status;
    std::vector<std::string> notes;
};

// Default M1 request: storeys stack along +Z, buildings spread along +X.
LayoutRequest DefaultRequest ();

// Snapshot -> scene -> plan -> geometry. `bindings` is the session's copy; the result
// carries the updated copy so a superseded build never moves the session's identities.
Prepared Prepare (const DiagramInputSnapshot& snapshot, StoreyBindings bindings, const LayoutRequest& request,
                  uint64_t generation);
} // namespace geomsrv::archviz::diagram
#endif
