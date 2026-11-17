#ifndef EVP_ARCHVIZ_DIAGRAMSNAPSHOT_HPP
#define EVP_ARCHVIZ_DIAGRAMSNAPSHOT_HPP
// One coherent, validated input for an exploded diagram, and the scene built from it.
//
// Pure and worker-safe: the caller reads the published massing result and the project
// storeys (ReadStoreys is main-thread ACAPI) and hands them in; nothing here reads a live
// global, writes the model or touches the renderer. An incompatible snapshot builds no
// scene: the caller keeps its last coherent diagram and shows the status instead.
#include "ArchViz/DiagramLayout.hpp"
#include "ArchViz/ExtractionStorySlices.hpp"
#include "ArchViz/MassingBuildings.hpp"
#include "ArchViz/MassingSlices.hpp"
#include <compare>
#include <map>
#include <memory>

namespace geomsrv::archviz::diagram {
struct RevisionSet {
    uint64_t projectEpoch = 0; // massingmodel::ProjectEpoch
    uint64_t modelStamp = 0;   // modelwatch::CaptureStamp
    uint64_t slices = 0;       // published massingslices::Result generation
    uint64_t topology = 0;     // buildingplan::Planner::Revision, when plans join
    uint64_t metadata = 0;
    uint64_t metrics = 0;
    auto operator<=> (const RevisionSet&) const = default;
};

enum class SnapshotIssue : uint8_t {
    MissingSlices,        // no published massing result
    IncompleteSlices,     // sources were skipped: never draw a partial building
    UnknownBuildingKey,   // a row whose slab belongs to no building group
    StoreyDomainMismatch, // one project storey of one building at two elevations
};
const char* Describe (SnapshotIssue issue);

// Which floors the rows are. Project stories carry an Archicad floorId; the massing's
// own floor heights do not, so their bindings are positional only (#222).
enum class StoreyDomain : uint8_t { ProjectStories, MassingFloors };

struct SnapshotStatus {
    bool compatible = false;
    std::vector<SnapshotIssue> failures;
    std::vector<std::string> warnings;
    std::vector<std::string> affected; // stable keys (slab GUIDs, building keys), sorted
};

struct DiagramInputSnapshot {
    std::shared_ptr<const massingslices::Result> slices; // as published
    ProjectStoreys storeys;
    RevisionSet revisions;
    // Derived by Capture:
    std::shared_ptr<const massingslices::Result> floors; // `slices` or its story cut
    StoreyDomain domain = StoreyDomain::MassingFloors;
    std::vector<massingbuildings::Group> groups; // = massingbuildings::Groups (records), sorted by key
    SnapshotStatus status;
};

// Validates and freezes. Never mixes the massing's own floors with the story cut: the
// story cut wins whenever the published result has one or already follows the stories.
DiagramInputSnapshot Capture (std::shared_ptr<const massingslices::Result> slices, ProjectStoreys storeys,
                              RevisionSet revisions);

// Session state that outlives scenes: storey bindings per building key.
struct StoreyBindings {
    std::map<std::string, std::vector<StoreyBinding>> byBuilding;
    uint64_t nextSerial = 1;
};

// What the inspector says about a storey: source fields as published, never re-measured
// (§5.9 synthesized read-only page; metrics authority stays with the massing result, #165).
struct StoreySummary {
    std::string buildingKey, buildingId; // id empty for a slab without an authored building ID
    int sourceStory = 0;
    std::string storyName; // the project story's name, when the domain is project stories
    double z = 0, top = 0;
    double rawArea = 0, allowedArea = 0; // sums of the storey's rows, m2
    std::vector<std::string> functions;  // distinct, in row order
    std::vector<std::string> slabs;      // source GUIDs, sorted
    bool planned = false;
};

struct BuiltScene {
    std::shared_ptr<const Scene> scene;        // null when the snapshot is incompatible
    std::vector<PickMesh> shells;              // one per storey, node-local; also the draw source
    std::vector<std::string> representedGuids; // source slabs the diagram stands in for, sorted
    std::map<EntityId, uint32_t> colours;      // storey -> its first row's function colour, 0xRRGGBBAA
    std::map<EntityId, StoreySummary> storeys; // storey -> inspector fields
    std::vector<EntityId> dropped;             // storey bindings with no successor
    std::vector<std::string> notes;
    RevisionSet revisions;
};

// Buildings from massingbuildings groups, storeys from the snapshot's floor rows as
// shells (planned = false until the floor planner's topology joins). Storey IDs come
// from `bindings`, which is updated only when a scene is built.
BuiltScene BuildScene (const DiagramInputSnapshot& snapshot, StoreyBindings& bindings);
} // namespace geomsrv::archviz::diagram
#endif
