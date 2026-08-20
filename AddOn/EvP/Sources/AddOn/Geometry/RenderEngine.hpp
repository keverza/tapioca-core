#ifndef GEOMETRYSERVER_RENDERENGINE_HPP
#define GEOMETRYSERVER_RENDERENGINE_HPP

#include "QueryEngine.hpp"

#include <vector>
#include <cstdint>

// CPU raycast renderer (M7): shoots one ray per pixel through the snapshot's
// triangle BVH and records depth / normal / object-id buffers for an arbitrary
// camera. Pure C++ over the immutable snapshot — runs on HTTP worker threads.
namespace geomsrv {

struct Camera {
    double eye[3]    = { 0, 0, 0 };
    double target[3] = { 0, 0, -1 };  // look-at point
    double up[3]     = { 0, 0, 1 };   // world up (Archicad Z-up)
    double fovYdeg   = 50.0;          // vertical field of view (perspective)
    bool   ortho     = false;
    double orthoHeight = 10.0;        // view height in meters (orthographic)
};

struct RenderResult {
    int w = 0, h = 0;
    std::vector<int32_t> id;      // element index per pixel, -1 = background
    std::vector<float>   depth;   // ray distance (m) from eye, 0 = background
    std::vector<float>   normal;  // per-pixel xyz smooth normal, 0 = background

    bool Valid () const { return w > 0 && h > 0; }
};

// Renders w x h pixels. Row 0 is the top of the image; pixel (r,c) -> r*w + c.
// Uses up to `threads` worker threads (0 = auto by hardware concurrency).
RenderResult Render (const QueryEngine& eng, const Camera& cam, int w, int h, unsigned threads = 0);

} // namespace geomsrv

#endif
