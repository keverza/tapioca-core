#ifndef EVP_ARCHVIZ_POINTCLOUDLOADER_HPP
#define EVP_ARCHVIZ_POINTCLOUDLOADER_HPP

#include <cstddef>
#include <string>

namespace geomsrv {
namespace archviz {

struct PointCloudBoundsResult {
    bool succeeded = false;
    size_t points = 0;
    double boundsMin[3] = {};
    double boundsMax[3] = {};
    std::string error;
};

struct PointCloudLoadResult {
    bool succeeded = false;
    size_t points = 0;
    size_t nodes = 0;
    size_t queuedBytes = 0;
    double parseMilliseconds = 0.0;
    double hierarchyMilliseconds = 0.0;
    double projectOrigin[3] = {};
    double projectBoundsMin[3] = {};
    double projectBoundsMax[3] = {};
    std::string sourceId;
    std::string error;
};

// Reads only the PLY contract needed by preprocessors that derive a crop.
// No render queue payloads are published.
PointCloudBoundsResult InspectPointCloudPly (const std::wstring& path);

// Parses and indexes a PLY on the calling worker, then publishes bounded,
// coarse-first owning payloads to the render queue. No Archicad API is used.
PointCloudLoadResult LoadPointCloudForDiligent (const std::wstring& path, const std::string& layerId,
                                                const double projectToSurvey[12]);

} // namespace archviz
} // namespace geomsrv

#endif
