#ifndef GEOMETRYSERVER_SURFACETAKEOFFRESULT_HPP
#define GEOMETRYSERVER_SURFACETAKEOFFRESULT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::surfacetakeoff {

struct Seed {
    std::string guid;
    std::vector<double> point, normal;
};
struct Request {
    uint64_t snapshotId = 0, captureStamp = 0;
    std::vector<std::string> targetGuids, coverGuids;
    std::string coverScopeId, regionMode;
    std::optional<double> contactToleranceM;
    std::vector<Seed> seeds;
    std::vector<int32_t> surfaceAttributeIndices;
    std::optional<std::vector<uint32_t>> surfaceColors;
    bool publishOverlay = false, compareNative = false;
};

struct SurfaceRow {
    int32_t surfaceAttributeIndex = 0;
    std::string name;
    uint32_t rgba = 0;
    double areaM2 = 0;
};
struct NativeSurfaceRow {
    int32_t surfaceAttributeIndex = 0;
    double areaM2 = 0;
};
struct FragmentMesh {
    std::string guid;
    int32_t sourceBody = 0, sourcePgon = 0, surfaceAttributeIndex = 0;
    double areaM2 = 0;
    std::vector<double> points;
    std::vector<uint32_t> triangles;
};
struct Result {
    uint64_t snapshotId = 0, captureStamp = 0;
    std::string classification, unit, coverScopeId;
    double contactToleranceM = 0;
    std::vector<std::string> coverGuids, regionElementGuids;
    // Resolved candidates retained for the publish-time basic metadata batch.
    // This is controller-owned data, not an additional wire/result quantity.
    std::vector<std::string> targetGuids;
    size_t regionFaceCount = 0, coverBodyCount = 0, selectedTriangleCount = 0;
    double selectedAreaM2 = 0;
    bool overlayPublished = false, nativeComparisonAvailable = false;
    // Controller workers defer SDK native quantities to main-thread publication.
    bool nativeComparisonRequested = false;
    std::string nativeComparisonNote;
    std::vector<SurfaceRow> surfaces;
    std::vector<FragmentMesh> meshes;
    std::vector<NativeSurfaceRow> nativeSurfaces;
};
struct Calculation {
    std::optional<Result> result;
    std::string error;
};

} // namespace geomsrv::surfacetakeoff

#endif
