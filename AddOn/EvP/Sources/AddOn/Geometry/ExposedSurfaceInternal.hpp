#ifndef GEOMETRYSERVER_EXPOSEDSURFACEINTERNAL_HPP
#define GEOMETRYSERVER_EXPOSEDSURFACEINTERNAL_HPP

#include "ExposedSurface.hpp"
#include "SurfaceCoverage.hpp"
#include "SurfaceSourceMap.hpp"
#include <chrono>

namespace geomsrv::exposed::detail {
namespace cp = Clipper2Lib;
using V = surfacecoverage::V;
struct Face {
    FaceKey key;
    int32_t material = 0;
    surfacemap::SourceFace source;
    surfacecoverage::Frame frame;
    Aabb bounds;
    cp::PathsD subject;
};
struct Body {
    BodyKey key;
    Mesh mesh;
};
struct Work {
    const Limits& limits;
    const std::function<bool ()>& cancelled;
    Statistics statistics;
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now ();
    bool Check (size_t amount, Refusal& refusal);
    bool Check (size_t amount, std::string& error); // legacy region-discovery compatibility
};
V Point (const std::vector<double>& points, size_t index);
bool Prepare (const Snapshot& snapshot, const Request& request, std::vector<Face>& faces, std::vector<Body>& covers,
              Work& work, Refusal& refusal);
bool Overlaps (const Aabb& a, const Aabb& b, double margin = 0);
bool NearContact (const Face& face, const Body& body, double tolerance, cp::PathsD& paths, Work& work,
                  Refusal& refusal);
} // namespace geomsrv::exposed::detail
#endif
