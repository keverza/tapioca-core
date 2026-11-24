// RenderBench/SunVisibility -- the sun's visibility at every pixel, traced on the CPU.
//
// The raster shadow map's ground truth for #234, and the one reference the bench can draw on a
// machine without hardware ray tracing: one primary ray per pixel centre, then one shadow ray
// toward the sun from what it hit. The picture it writes matches the raster's "shadow" style
// (DiligentShadowMap's shadowsOnly view) pixel for pixel, so a leak is a pixel this calls dark
// and the raster calls lit.
//
// ⚠️ THE SAME CASTERS AS THE SHADOW MAP. A surface that is not opaque casts nothing there
// (DiligentScene::RenderShadowMap), so it casts nothing here either; otherwise every glazed
// opening would read as a leak.

#ifndef TAPIOCA_RENDERBENCH_SUNVISIBILITY_HPP
#define TAPIOCA_RENDERBENCH_SUNVISIBILITY_HPP

#include <cstdint>
#include <memory>
#include <vector>

namespace renderbench {

struct ObjScene;

// Each pixel of a visibility picture.
constexpr uint8_t kSunLit = 255;
constexpr uint8_t kSunOccluded = 0;
// Not a shadow question: sky, glazing, or a surface turned away from the sun (N.L below a small
// threshold), whose darkness is the lighting's and not the shadow map's.
constexpr uint8_t kSunExcluded = 128;
// A sun with a diameter (Render's sunDiameterDegrees > 0) answers with the fraction of its disk
// in view, 0..kSoftSunScale, and marks the pixels that are not a shadow question kSoftSunExcluded.
constexpr uint8_t kSoftSunScale = 250;
constexpr uint8_t kSoftSunExcluded = 255;

class SunVisibility {
  public:
    SunVisibility ();
    ~SunVisibility ();
    // Copies the opaque triangles out of `scene`; call before its elements are handed away.
    void Build (const ObjScene& scene);
    bool Blocked (const double* point, const double* direction, double near) const;
    // One byte per pixel, rows top to bottom. `sun` points toward the sun; the camera is
    // LookAtRH(eye, target, +Z) with a vertical field of view in degrees, as the bench's.
    // sunDiameterDegrees > 0 traces the sun as a disk of that angular diameter (see kSoftSunScale).
    std::vector<uint8_t> Render (const float eye[3], const float target[3], float fovDegrees, uint32_t width,
                                 uint32_t height, const float sun[3], float sunDiameterDegrees = 0.0f) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderbench

#endif
