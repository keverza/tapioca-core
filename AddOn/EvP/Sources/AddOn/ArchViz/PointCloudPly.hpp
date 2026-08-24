#ifndef EVP_ARCHVIZ_POINTCLOUDPLY_HPP
#define EVP_ARCHVIZ_POINTCLOUDPLY_HPP

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

struct PointCloudVertex {
    float position[3] = {};
    uint32_t rgba = 0xFFFFFFFFu;
    float normal[3] = { 0.0f, 0.0f, 1.0f };
};

struct PointCloudData {
    std::vector<PointCloudVertex> vertices;
    std::vector<std::string> scalarFields;

    // PLY coordinates are retained relative to this double-precision origin.
    // CloudCompare restores global coordinates on export; project placement is
    // applied later from Archicad's survey-point transformation.
    double origin[3] = {};
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    double reportedGlobalShift[3] = {};
    bool hasReportedGlobalShift = false;
    bool hasColours = false;
    bool hasNormals = false;

    size_t Bytes () const;
};

// Reads the CloudCompare intermediate contract: binary little-endian PLY with
// scalar vertex properties. No DevKit, Diligent, or Archicad calls are made.
bool LoadPointCloudPly (std::istream& input, PointCloudData& output, std::string& error);

// Converts global Survey Point coordinates to Project Origin coordinates. The
// supplied affine matrix maps Project Origin to Survey Point and is inverted
// here in double precision. Both systems and PLY coordinates are meters.
bool PlacePointCloudInProject (PointCloudData& cloud, const double projectToSurvey[12], std::string& error);

} // namespace archviz
} // namespace geomsrv

#endif
