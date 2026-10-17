#ifndef GEOMETRYSERVER_EXPOSEDSURFACE_HPP
#define GEOMETRYSERVER_EXPOSEDSURFACE_HPP

#include "Mesh.hpp"
#include <functional>
#include <optional>
#include <set>
#include <tuple>

// Stage B: contact-uncovered planar fragments over a detached immutable capture.
// Exterior classification/seed expansion is a caller responsibility, NOT normal
// orientation or visibility. No SDK, gate, camera, cache/store or renderer access.
namespace geomsrv::exposed {
constexpr uint32_t kAlgorithmVersion = 3; // explicit near-contact tolerance, independent of validation precision

struct BodyKey {
    size_t meshIndex = 0;
    int32_t sourceBody = 0; // ModelerAPI 1-based, not C database/picker indices
    bool operator< (const BodyKey& other) const
    {
        return std::tie (meshIndex, sourceBody) < std::tie (other.meshIndex, other.sourceBody);
    }
    bool operator== (const BodyKey&) const = default;
};
struct FaceKey {
    BodyKey body;
    int32_t sourcePgon = 0;
    bool operator< (const FaceKey& other) const
    {
        return std::tie (body, sourcePgon) < std::tie (other.body, other.sourcePgon);
    }
    bool operator== (const FaceKey&) const = default;
};
struct Request {
    uint64_t snapshotId = 0, captureStamp = 0;
    std::string coverScopeId; // caller's explicit model-state/cover-scope identity
    std::vector<FaceKey> faces;
    std::vector<BodyKey> covers; // empty means no covers, never implicit whole model
    // Capture-local MODEL POOL indices. The future native adapter must resolve
    // Archicad surface attributes against this capture, never building materials.
    // nullopt selects all candidate materials; an empty set selects none.
    std::optional<std::set<int32_t>> materialPoolFilter;
    // Cover-facing boundary patches at a normal gap of 0..this distance also
    // cover their projected overlap. Never a lateral buffer or a mesh weld.
    double contactToleranceM = 0;
};
struct Areas {
    double grossGeometricArea = 0, exposedArea = 0, selectedExposedArea = 0; // m2
};
struct Fragment {
    FaceKey source;
    int32_t materialPoolIndex = 0;
    bool selected = true;
    // First loop is outer, subsequent loops are holes. Coordinates are world
    // doubles in metres; no display bias. Triangles cover exactly these loops.
    std::vector<std::vector<double>> loops;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    double areaM2 = 0; // computed FROM the emitted world-space triangles
};
struct FaceAreas {
    FaceKey source;
    int32_t materialPoolIndex = 0;
    Areas areas;
};
struct Statistics {
    size_t candidateCoverPairs = 0, sectionCount = 0, work = 0, outputTriangles = 0;
};
struct Result {
    uint64_t snapshotId = 0, captureStamp = 0;
    uint32_t algorithmVersion = kAlgorithmVersion;
    std::string coverScopeId;
    std::vector<BodyKey> covers; // canonical deduplicated scope actually used
    std::vector<FaceAreas> faces;
    std::vector<Fragment> fragments;
    Areas areas;
    std::map<int32_t, Areas> byMaterialPool;
    std::map<std::string, Areas> byElementGuid;
    Statistics statistics;
    double contactToleranceM = 0;
};
struct Limits {
    size_t maxInputTriangles = 100000, maxFaces = 512, maxCoverBodies = 128;
    size_t maxWork = 2000000, maxOutputTriangles = 200000;
    double maxSeconds = 2;
};

// Atomic: failure/cancellation clears result and explains refusal. No partial
// quantities. Duplicate face/cover requests are deduplicated deterministically;
// same-facing overlaps are owned once; conflicting material ownership refuses.
// Cover bodies must be closed consistently-wound solids. Unsupported topology
// (including unmatched welded/T-junction solid edges) refuses, never approximates.
bool Calculate (const Snapshot& snapshot, const Request& request, Result& result, std::string& error,
                const Limits& limits = {}, const std::function<bool ()>& cancelled = {});
} // namespace geomsrv::exposed
#endif
