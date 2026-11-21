#ifndef EVP_ARCHVIZ_RAYTRACEDVIEW_HPP
#define EVP_ARCHVIZ_RAYTRACEDVIEW_HPP

// ArchViz/RayTracedView -- the pure half of the viewer's ray-traced display style (issue #231).
//
// The style replaces the raster pass's shaded colour with a progressively accumulated path-traced
// image, on a D3D12 device with hardware ray tracing and the bundled DXC, and only while the view
// is still. The GPU half is DiligentSceneRayTraced.cpp; this file is what can be tested offline:
// the scene flattened for one acceleration structure, the matrix the rays are generated from, and
// the rule for when the accumulation restarts, shows, or stops.
//
// ⚠️ THE RASTER FRAME STILL RUNS UNDERNEATH. Its depth is what the section caps, the selection and
// every line overlay are measured against, and its coverage is what the overlay composites with;
// the traced image only replaces the colour that goes into the resolve. So nothing that draws
// over the image needs to know which style produced it.
//
// ⚠️ A MOVING VIEW SHOWS THE RASTER IMAGE, NOT ONE NOISY SAMPLE. Every change of the view key
// restarts the accumulation and hands the frame back to the raster path; the first still frame
// traces a small burst so what appears is already readable, and the image then converges until
// `maxSamples`, where the GPU stops tracing and the last image is simply re-shown.
//
// PURE: no Diligent, no DevKit -- tests/cpp/test_raytracedview.cpp.

#include "ArchViz/MeshGroups.hpp"
#include "ArchViz/SectionCaps.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

// The GPU half's constant block (DiligentSceneRayTraced.cpp), named here so DiligentScene.hpp can
// pass it without seeing it.
struct RayTracedConstants;

// The HUD's knobs for the style.
struct RayTracedSettings {
    bool enabled = false;
    uint32_t maxSamples = 512; // per pixel; the image is final after this many
    uint32_t bounces = 2;      // diffuse bounces after the first hit (0 = direct light only)
    // #232: the Rendered style's reflections traced instead of screen-space, where the device can.
    // Independent of `enabled`: it changes the raster image's reflections, not the style.
    bool reflections = true;
    // #233: the Rendered style's sky light occluded by rays (traced visibility in GTAO's place).
    bool skyLight = true;
};

// One element as the renderer holds it: the retained triangles and the material ranges over them.
struct RayTracedSource {
    std::shared_ptr<const SectionMesh> mesh;
    std::vector<MaterialRange> ranges;
};

// Every visible element in one triangle soup -- one bottom-level structure, one instance.
// `triangleMaterial[t]` indexes `materials`, the distinct material ids in first-seen order, so the
// per-frame material table (clay, presets, study model) is a few dozen entries, not one per triangle.
struct RayTracedGeometry {
    std::vector<float> positions;           // xyz per vertex, world metres
    std::vector<float> uvs;                 // #231: Archicad's uv per vertex, (0, 0) where it gave none
    std::vector<uint32_t> indices;          // 3 per triangle, into `positions`
    std::vector<uint32_t> triangleMaterial; // one per triangle, into `materials`
    std::vector<int32_t> materials;         // material ids
    size_t TriangleCount () const
    {
        return indices.size () / 3;
    }
};

// Flattens the sources. A triangle no range covers gets the default material (id -1); indices that
// fall outside their element's vertices are dropped rather than traced into garbage.
RayTracedGeometry BuildRayTracedGeometry (const std::vector<RayTracedSource>& sources);

// #231: the texture array the traced passes sample (Texture2D g_albedo[kRayTracedTextureSlots]).
// Fixed, because the shader declares it; a project naming more images than this traces the rest in
// their surface colour, counted in `overflow` rather than silently.
constexpr uint32_t kRayTracedTextureSlots = 256;

struct RayTracedTextureSlots {
    std::vector<int32_t> slotOf; // per material, -1 = untextured
    std::vector<uint64_t> keys;  // the image key in each used slot, first seen first
    uint32_t overflow = 0;       // materials left untextured because the array was full
};

// `keys[m]` is material m's image key, or 0 when it draws untextured (no image, the image is not on
// the GPU, or textures are gated off). Materials sharing an image share its slot.
RayTracedTextureSlots AssignTextureSlots (const std::vector<uint64_t>& keys, uint32_t capacity);

// General 4x4 inverse in double, row-major storage like MatrixMath. False when singular.
bool InvertMatrix4 (const float in[16], float out[16]);

// What the GPU half does this frame, decided from the view key alone.
struct RayTracedStep {
    bool show = false;        // hand the accumulated image to the resolve
    uint32_t trace = 0;       // samples to trace this frame
    uint32_t firstSample = 0; // the index of the first of them (0 = overwrite the accumulation)
};

// The accumulation's bookkeeping: one per scene.
class RayTracedAccumulator {
  public:
    // ⚠️ THE KEY IS EVERYTHING THE IMAGE DEPENDS ON -- camera, light, materials, geometry, plane,
    // viewport size, settings. A key that misses an input shows a stale image that never updates.
    RayTracedStep Advance (uint64_t key, uint32_t maxSamples);
    void Reset ();
    uint32_t Samples () const
    {
        return samples_;
    }

    // The burst traced on the first still frame.
    static constexpr uint32_t kFirstBurst = 4;

  private:
    uint64_t key_ = 0;
    bool haveKey_ = false;
    bool still_ = false; // the key held for at least one frame
    uint32_t samples_ = 0;
};

// FNV-1a over raw bytes, for the view key.
uint64_t HashBytes (uint64_t h, const void* data, size_t size);
constexpr uint64_t kHashSeed = 1469598103934665603ull;

enum class RayTracedState : uint8_t {
    Off = 0,
    Unavailable = 1, // the device, the compiler or the pipeline said no; `reason` says which
    Moving = 2,      // the view is changing: the raster image is shown
    Accumulating = 3,
    Converged = 4,
};

struct RayTracedStats {
    RayTracedState state = RayTracedState::Off;
    uint32_t samples = 0;
    uint32_t maxSamples = 0;
    uint64_t triangles = 0;
    uint32_t texturedMaterials = 0; // #231: materials the traced passes sample a texture for
    uint32_t textureOverflow = 0;   // #231: textured materials past kRayTracedTextureSlots
    bool reflectionsTraced = false; // #232: last frame's reflections came from rays, not SSR
    bool skyLightTraced = false;    // #233: last frame's sky light was occluded by rays
    std::string reason;
};

const char* RayTracedStateName (RayTracedState state);

} // namespace archviz
} // namespace geomsrv

#endif
