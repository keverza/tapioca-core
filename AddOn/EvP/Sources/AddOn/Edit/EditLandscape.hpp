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
    ProtectedZone
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
    std::vector<Id> inputs;              // Ordered: Difference is A minus B.
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
struct LandscapeResult {
    uint64_t revision = 0;
    std::vector<DerivedRegion> faces, regions;
    std::vector<PlanarDiagnostic> planarDiagnostics;
    std::vector<LandscapeDiagnostic> diagnostics;
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
bool RebuildLandscape (geomsrv::edit::Document& doc, LandscapeResult& result, std::string& error);
bool StartLandscape (geomsrv::edit::Document& doc, std::array<uint8_t, 16> uuid, std::string& error);
bool AddLandscapeSource (geomsrv::edit::Document& doc, LandscapeSource source, Id& id, std::string& error);
bool DeleteLandscapeSource (geomsrv::edit::Document& doc, Id id, std::string& error);
bool AddLandscapeOperation (geomsrv::edit::Document& doc, LandscapeOperation operation, Id& id, std::string& error);
bool SetLandscapeOperationEnabled (geomsrv::edit::Document& doc, Id id, bool enabled, std::string& error);
bool AddHardscapeObject (geomsrv::edit::Document& doc, HardscapeObject object, Id& id, std::string& error);
bool SetLandscapePropertyLine (geomsrv::edit::Document& doc, Id source, std::string& error);
bool ValidateLandscapeTerrainEdit (const geomsrv::edit::Document& doc, const std::vector<Vec3>& terrainBoundary,
                                   std::string& error);
const DerivedRegion* LandscapeFaceAt (const LandscapeResult& result, Vec3 point);
// Virtual demonstration only. Refuses a nonempty document; never reads/writes the SDK.
bool SeedLandscapeDemo (geomsrv::edit::Document& doc, double z, std::string& error);
// Hole-aware preview triangulation. No third-party types leave Edit/.
bool LandscapePreviewTriangles (const DerivedRegion& region, std::vector<Vec3>& triangles, std::string& error);
bool LandscapeSourcePreview (const geomsrv::edit::Document& doc, const LandscapeSource& source,
                             std::vector<Vec3>& points, bool& closed, std::string& error);

} // namespace tapioca::geometry
#endif
