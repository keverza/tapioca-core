#ifndef TAPIOCA_EDIT_LANDSCAPE_HPP
#define TAPIOCA_EDIT_LANDSCAPE_HPP

#include "Edit/EditPlanar.hpp"
#include <array>
#include <string>
#include <vector>

namespace geomsrv::edit {
struct Document;
}

namespace tapioca::geometry {

enum class SourceRole : uint8_t {
    PropertyLine,
    Guide,
    PathCenterline,
    Cutter,
    MaterialBoundary,
    GradingLimit,
    ProtectedZone,
    // The session's native base terrain: no curve or path; its GUID is the
    // session's sourceTerrainGuid. Captured terrain rows cite it.
    Terrain,
    // A closed building footprint: a boundary like any other, and the outline its
    // entrances (LandscapeEntrance) sit on.
    Building
};
enum class MaterialPreset : uint8_t { None, HardPaving, RubberPlayground, Gravel, Asphalt, ReinforcedGrass, Grass };
struct MaterialSpec {
    MaterialPreset preset = MaterialPreset::None;
    std::string fillGuid, fillName, surfaceGuid, surfaceName;
    double hatchDirection = 45.0, hatchDensity = 2.0;
    bool operator== (const MaterialSpec&) const = default;
};

// Geometry stays in the universal document. The source owns the semantic role,
// native link and optional analytic arc angles, never a second editable curve.
struct LandscapeSource {
    Id id = kNoId, entityId = kNoId, pathId = kNoId;
    SourceRole role = SourceRole::Guide;
    std::string archicadGuid;
    std::vector<double> arcAngles;
    uint64_t revision = 0;
    bool deleted = false;
    // Archicad's own drafting brought in by reference (`archicadGuid` names it): the editor
    // shows it locked and only Archicad changes its geometry. Otherwise the editor owns the
    // line and `archicadGuid` is only where native storage writes it.
    bool reference = false;
    bool operator== (const LandscapeSource&) const = default;
};

// A boundary's authored segment support, distinct from H1's local piece ordinal.
// Repeated flattened chords coalesce; cycle rotation does not affect matching.
struct BoundaryRef {
    Id source = kNoId, segment = kNoId;
    bool reversed = false;
    bool operator== (const BoundaryRef&) const = default;
};
struct RegionDescriptor {
    Id object = kNoId;
    std::vector<Id> sources, operations;
    std::vector<std::vector<BoundaryRef>> boundaries; // Outer, then canonical sorted holes.
    bool operator== (const RegionDescriptor&) const = default;
};

enum class OperationKind : uint8_t {
    Stroke,
    Offset,
    Union,
    Difference,
    Intersection,
    Xor,
    Split,
    RegionSelect,
    MaterialMask,
    GradingLimit,
    ProtectedZone
};
struct LandscapeOperation {
    Id id = kNoId;
    OperationKind kind = OperationKind::Union;
    std::vector<Id> inputs;              // Ordered: Difference is A minus B. Empty RegionSelect is an empty face set.
    std::vector<RegionDescriptor> faces; // RegionSelect; no persisted face locators.
    bool enabled = true;
    double width = 1.5, offset = 0.0;
    MaterialSpec material;
    int priority = 0;
    bool operator== (const LandscapeOperation&) const = default;
};
enum class ObjectKind : uint8_t { Path, Playground, Plaza, Parking, MaterialRegion, Edge };
struct HardscapeObject {
    Id id = kNoId, pathId = kNoId, footprintOperation = kNoId;
    ObjectKind kind = ObjectKind::Playground;
    std::string name;
    MaterialSpec baseMaterial;
    double thickness = 0.0;
    std::vector<Id> maskOperations;
    bool operator== (const HardscapeObject&) const = default;
};
struct RegionIdentity {
    Id id = kNoId;
    RegionDescriptor descriptor;
    std::vector<Id> predecessors;
    bool active = true;
    bool operator== (const RegionIdentity&) const = default;
};
struct MaterialAssignment {
    Id id = kNoId, region = kNoId, mask = kNoId;
    MaterialSpec material;
    int priority = 0;
    bool valid = true; // False requires explicit reassignment; never silently retarget.
    bool operator== (const MaterialAssignment&) const = default;
};
struct LandscapePatch {
    Id key = kNoId, object = kNoId, assignment = kNoId;
    std::string role, representation;
    std::vector<Id> sources, predecessorRegions, predecessorPatches;
    bool operator== (const LandscapePatch&) const = default;
};
enum class BindingKind : uint8_t { Source, Operation, Object };
struct LandscapeBinding {
    BindingKind kind = BindingKind::Source;
    Id logicalId = kNoId;
    std::vector<std::string> nativeGuids;
    std::string metadataUuid, role;
    uint64_t revision = 0;
    bool operator== (const LandscapeBinding&) const = default;
};
// A fixed-height constraint references existing authored geometry, never copied
// coordinates. vertex=0 means the whole open straight polyline; otherwise it is
// an authored vertex ID on the source's entity. Disabled/orphaned references
// persist for explicit resolution; enabled orphans refuse terrain evaluation.
struct LandscapeTerrainControl {
    Id source = kNoId, vertex = kNoId;
    bool enabled = true;
    bool operator== (const LandscapeTerrainControl&) const = default;
};
// A place on a Building source's outline that a path must reach, at its threshold height.
// `along` is plan metres along the outline from its first vertex, so the entrance follows the
// outline as it is edited; `doorGuid` names the Archicad door it was read from, if any.
struct LandscapeEntrance {
    Id id = kNoId, source = kNoId;
    double along = 0.0, z = 0.0;
    std::string doorGuid;
    bool operator== (const LandscapeEntrance&) const = default;
};
// A step in the designed ground along a straight baseline (a path in Document::paths): a
// curb, a retaining (holding) wall or a stair. The baseline runs from the low end to the high
// end of a stair; for a curb or wall `highSide` (looking along it) is the upper ground. The
// native stair stays Archicad's: `stairGuid` links it, `wallGuid` names a committed curb/wall.
enum class EdgeKind : uint8_t { Curb, RetainingWall, Stair };
enum class EdgeSide : uint8_t { Left, Right };
struct EdgeSpec {
    Id id = kNoId;
    EdgeKind kind = EdgeKind::Curb;
    Id pathId = kNoId;
    double width = 0.15;              // curb top width / wall thickness / flight width
    double topZ = 0.0, bottomZ = 0.0; // upper and lower ground levels (a stair's top and foot)
    double footingDepth = 0.0;        // a wall's foot below bottomZ
    double stairRun = 0.30, stairRise = 0.17;
    int stairSteps = 0;
    double landingDepth = 0.0; // level run at a stair's top, inside the baseline
    double clearance = 0.05;   // a stair's underside above the terrain
    EdgeSide highSide = EdgeSide::Left;
    MaterialSpec material;
    std::string stairGuid, wallGuid;
    bool operator== (const EdgeSpec&) const = default;
};

struct LandscapeSession {
    std::array<uint8_t, 16> sessionUuid {};
    std::string sourceTerrainGuid;
    Id propertyLineSourceId = kNoId;
    std::vector<LandscapeSource> sources;
    std::vector<LandscapeOperation> operations;
    std::vector<HardscapeObject> objects;
    std::vector<RegionIdentity> regions; // Durable semantic matching records, not geometry.
    std::vector<MaterialAssignment> assignments;
    std::vector<LandscapePatch> patches;
    std::vector<LandscapeBinding> bindings;
    std::vector<LandscapeTerrainControl> terrainControls;
    std::vector<LandscapeEntrance> entrances;
    std::vector<EdgeSpec> edges;
    // Document::nextId/revision are the sole allocator/publication authority.
    bool operator== (const LandscapeSession&) const = default;
};

enum class LandscapeDiagnosticCode {
    ProvenanceUnattributable,
    ProvenanceConflict,
    ProvenanceStale,
    PatchKeyUnstable,
    FaceKeyAmbiguous,
    AmbiguousFace,
    OrphanedOperation,
    DagCycle,
    PropertyLineOutside,
    PropertyLineArcUnsupported,
    OffsetCollapsed,
    OffsetSelfIntersection,
    ClipEmpty
};
struct LandscapeDiagnostic {
    LandscapeDiagnosticCode code = LandscapeDiagnosticCode::ProvenanceUnattributable;
    Id source = kNoId, operation = kNoId, object = kNoId;
    std::string message;
};
struct RegionBoundaryPiece {
    Vec3 a, b;
    std::vector<BoundaryRef> support;
    std::vector<PlanarEdgePiece> parameters; // Source-local subdomains, current revision only.
    bool generated = false;
};
struct DerivedRegion {
    Id regionId = kNoId, objectId = kNoId;
    uint64_t faceId = 0; // Current evaluation only.
    RegionDescriptor descriptor;
    std::vector<std::vector<Vec3>> rings;
    std::vector<std::vector<RegionBoundaryPiece>> boundary;
    std::vector<Id> sources, operations, predecessorRegions;
    double area = 0.0;
    Vec3 seed;
    MaterialSpec material;
};
// Derived constraint inputs, not stored terrain or a second authoring model.
// Only masks reaching a footprint participate; full mask extents are retained
// because grading/protection also acts on the surrounding transition.
struct LandscapeTerrainConstraint {
    Id operation = kNoId;
    OperationKind kind = OperationKind::GradingLimit;
    bool uniformScope = true; // Every disconnected input region reaches the same mask pieces.
    std::vector<DerivedRegion> masks;
};
struct LandscapeResult {
    uint64_t revision = 0;
    std::vector<DerivedRegion> faces, regions;
    std::vector<PlanarDiagnostic> planarDiagnostics;
    std::vector<LandscapeDiagnostic> diagnostics;
    std::vector<LandscapeTerrainConstraint> terrainConstraints; // Opt-in terrain evaluation only.
    std::vector<Vec3> propertyLine;
    uint64_t propertyLineRevision = 0;
    double boundaryTolerance = 0.001;
    std::string error;
};

const char* LandscapeDiagnosticCodeName (LandscapeDiagnosticCode code);
// Returns uppercase dashed non-nil UUID/GUID, refusing malformed input.
bool CanonicalLandscapeGuid (const std::string& text, std::string& canonical);
bool ValidateLandscape (const geomsrv::edit::Document& doc, std::string& error);
bool ValidateLandscapeDag (const LandscapeSession& session, std::vector<Id>& order, std::string& error);
bool EvaluateLandscape (const geomsrv::edit::Document& doc, LandscapeResult& result);
// Transactional: publish durable IDs/invalidated assignments only on success.
// `before` is the immutable document whose identity catalog `doc` still carries.
// A new region's predecessors are the identities whose footprint there overlaps
// it; when one is needed and that before-state is absent or another catalog's,
// lineage is unprovable and the rebuild refuses.
bool RebuildLandscape (geomsrv::edit::Document& doc, LandscapeResult& result, std::string& error,
                       const geomsrv::edit::Document* before = nullptr);
bool StartLandscape (geomsrv::edit::Document& doc, std::array<uint8_t, 16> uuid, std::string& error);
bool AddLandscapeSource (geomsrv::edit::Document& doc, LandscapeSource source, Id& id, std::string& error);
bool DeleteLandscapeSource (geomsrv::edit::Document& doc, Id id, std::string& error);
bool AddLandscapeOperation (geomsrv::edit::Document& doc, LandscapeOperation operation, Id& id, std::string& error);
bool SetLandscapeOperationEnabled (geomsrv::edit::Document& doc, Id id, bool enabled, std::string& error);
bool AddHardscapeObject (geomsrv::edit::Document& doc, HardscapeObject object, Id& id, std::string& error);
bool SetLandscapePropertyLine (geomsrv::edit::Document& doc, Id source, std::string& error);
// A virtual line (an entity, not a path) takes `role`: its live source is re-roled, or one is
// added. PropertyLine makes it the session's property line and returns the previous one to Guide.
// A path's centreline and the terrain are not roles a line can be given; Building needs a closed
// line. A line that stops being a building loses its entrances.
bool SetLandscapeSourceRole (geomsrv::edit::Document& doc, Id entity, SourceRole role, std::string& error);
// Names the session's base terrain: sets sourceTerrainGuid and keeps exactly one
// live Terrain source carrying that GUID (created on first use, otherwise
// updated), returning its ID. One authored edit; no geometry is read.
bool SetLandscapeTerrain (geomsrv::edit::Document& doc, const std::string& guid, Id& source, std::string& error);
// One document edit; repeated values are a no-op. Height follows the authored
// guide geometry. This does not set a positional edit lock or deform terrain.
bool SetLandscapeTerrainControl (geomsrv::edit::Document& doc, Id source, Id vertex, bool enabled, std::string& error);
bool ValidateLandscapeTerrainEdit (const geomsrv::edit::Document& doc, const std::vector<Vec3>& terrainBoundary,
                                   std::string& error);
const DerivedRegion* LandscapeFaceAt (const LandscapeResult& result, Vec3 point);
// Virtual demonstration only. Refuses a nonempty document; never reads/writes the SDK.
bool SeedLandscapeDemo (geomsrv::edit::Document& doc, double z, std::string& error);
struct LandscapePreviewMesh {
    std::vector<Vec3> vertices;
    std::vector<uint32_t> triangles;
};
// One already-classified outer plus holes, in raw ring order, at seed.z.
// Exact covers and area-checked constrained covers with holes are displayed;
// approximate covers and generated crossing vertices refuse. Failure leaves
// mesh untouched for the caller's existing outlines-only warning.
bool BuildLandscapePreviewMesh (const DerivedRegion& region, LandscapePreviewMesh& mesh, std::string& error);
// Legacy expanded triangulation retained for terrain masks: its rings may be
// multiple disconnected outers, not the single-outer indexed preview contract.
// No third-party types leave Edit/.
bool LandscapePreviewTriangles (const DerivedRegion& region, std::vector<Vec3>& triangles, std::string& error);
bool LandscapeSourcePreview (const geomsrv::edit::Document& doc, const LandscapeSource& source,
                             std::vector<Vec3>& points, bool& closed, std::string& error);

} // namespace tapioca::geometry
#endif
