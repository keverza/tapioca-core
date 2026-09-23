#ifndef EVP_ARCHVIZ_SUNSTUDYOVERLAY_HPP
#define EVP_ARCHVIZ_SUNSTUDYOVERLAY_HPP

// ArchViz/SunStudyOverlay — the sun study's atlas as something the Diligent
// viewer can draw, and the map from a rendered fragment back into it.
//
// ⚠️ NO Diligent, NO ACAPI, NO GPU TYPE. Everything here is the arithmetic and
// the ownership that decide whether the tint lands on the right surface, which
// is precisely the part that cannot be debugged inside Archicad. It compiles in
// tests/cpp against the real SunStudy core, and test_sunstudyoverlay.cpp checks
// the mapping against the atlas's own scatter table numerically.
//
// ---------------------------------------------------------------------------
// WHY THIS IS PER TRIANGLE AND NOT PER VERTEX
//
// The plan asked for a per-element `StructuredBuffer<float2>` of atlas UVs
// indexed by `SV_VertexID`. Tracing the geometry path first — which is what the
// task required before touching a shader — shows that cannot be correct here:
//
//   * a sun study "face" IS a source triangle (`SampleGrid::faces` is the
//     triangle index, and `SunStudyAtlas::tiles` is one tile per triangle), and
//   * `Geometry/VertexWeld` merges two corners that agree on source vertex AND
//     normal, so every corner of a flat quad is SHARED by both of its triangles
//     — two different faces, two different atlas tiles, one vertex.
//
// A float2 per vertex therefore has no correct value to hold: whichever face's
// UV is written, the other triangle reads the wrong tile. It would not fail —
// it would render a smooth plausible study with every second triangle sampling
// its neighbour. `ArchVizVertex` is left alone for the same reason the plan
// wanted it left alone, and no per-vertex data is introduced at all.
//
// So the side car is one record per RENDERED TRIANGLE carrying that face's own
// cell lattice, and the pixel shader maps its interpolated world position into
// it — the same affine map `SunStudySampler::FaceLayout` was written to make
// possible, evaluated where the face is unambiguous. It costs no vertex
// bandwidth, needs no change to `Shaders/varying.def.sc`, and is bound by one
// pass only.
//
// ⚠️ AND IT IS INDEXED BY THE MATERIAL-REORDERED TRIANGLE, NOT THE SOURCE ONE.
// `BuildMaterialGroups` sorts triangles by material before upload, so
// `SV_PrimitiveID` inside an element's draw counts triangles in THAT order. The
// builder below walks the same permutation — taken from the same function, not
// re-derived — and `MeshIndexHash` refuses the whole element if the geometry
// the viewer holds is not the geometry the study measured.

#include "ArchViz/MeshGroups.hpp"
#include "SunStudy/SunStudyAtlas.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

// One rendered triangle's window into the atlas, laid out as four float4s so the
// C++ side and the HLSL `StructuredBuffer<SunFaceMap>` cannot disagree about
// padding.
//
// ⚠️ `tile.zw == 0` MEANS "THIS FACE IS NOT IN THE ATLAS" — degenerate, or below
// the grid and carrying only a centroid sample. It is the fall-through to
// ordinary shading, and it is a different thing from a texel of zero hours.
struct SunFaceMap {
    // xyz = the face's grid origin in world metres (FaceLayout::origin);
    // w = 1 / grid spacing, so the shader multiplies instead of dividing.
    float originAndInvSpacing[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // xyz = the face's in-plane u axis, unit; w = FaceLayout::uStart in cells.
    float uAxisAndStart[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float vAxisAndStart[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // xy = the tile's origin in texels, zw = its size in texels.
    float tile[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

static_assert (sizeof (SunFaceMap) == 64, "the HLSL StructuredBuffer assumes four tightly packed float4s");

// One element's side car. Nothing here is a GPU object; the render thread turns
// `faces` into a structured buffer and forgets this.
struct SunStudyElementMap {
    std::string guid;
    // Triangles in RENDER order. `faces.size()` must equal the element's
    // index count / 3, and the render thread checks it.
    std::vector<SunFaceMap> faces;
    // What the study measured, so a viewer holding a different extraction of the
    // same element is refused rather than tinted wrongly.
    uint64_t topologyHash = 0;

    size_t Bytes () const;
};

// A completed study, whole, ready to replace whatever the viewer held.
//
// ⚠️ THE IMAGE IS SHARED, NOT COPIED. A 256x256 atlas is 256 KB and an 8192 one
// is 256 MB; the queue is unbounded and a study can be re-pushed on every
// display change (a threshold, a palette, a debug mode), so copying the texels
// through the queue would be the one place this feature could cost a frame.
// The producer builds the image once and never touches it again — the same
// hand-over rule as every other payload in SceneCmdQueue, expressed as a
// shared_ptr because a display-only change reuses the identical image.
struct SunStudyAtlasUpload {
    std::string studyId;
    // Bumped by the producer whenever the CONTENT changes. The render thread
    // keeps it so a HUD can say which study is on screen, and so a stale push
    // that lost a race can be recognised.
    uint64_t version = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<const std::vector<float>> texels;

    std::vector<SunStudyElementMap> elements;

    // The top of the tint ramp in hours. ⚠️ CARRIED, NOT DERIVED IN THE SHADER:
    // a per-pixel max is impossible, and normalising to the study's own maximum
    // instead of to its daylight length would make two studies of the same
    // building incomparable.
    float hoursMax = 1.0f;
    // 0 = sun hours. The rest are the diagnostic atlases: 1 = tile id, 2 = cell
    // gradient, 3 = cell checker. See `SunStudyDebugMode`.
    uint32_t debugMode = 0;
    // How the tint pass tests depth. See `SunStudyDepthMode`.
    //
    // ⚠️ IT IS A DIAGNOSTIC KNOB, NOT A SETTING, and it exists because the three
    // answers separate three different causes of a flickering overlay that look
    // identical on screen. EQUAL is the correct mode and the default: the tint is
    // a decal over geometry the shaded pass already drew, through the same vertex
    // shader and the same view-projection, so every surviving fragment's depth is
    // bit-identical. If the tint only behaves under LESS_EQUAL or with the test
    // off, the two passes are NOT seeing the same depth buffer -- which is a
    // render-lifecycle fault, not a shading one.
    uint32_t depthMode = 0;

    // The study's timestep in hours -- the quantum of the hours ramp. A study
    // can only report multiples of it, so the ramp snaps to it (as the web
    // viewer's does) rather than inventing gradations it never measured.
    float quantumHours = 0.25f;

    // The per-step lit bits for the SHADOW views (SunStudy/SunStudyStepAtlas.hpp):
    // `stepWords` slices of width x height uint32, slice-major, the same texels
    // as `texels`. Shared, like the hours image, and for the same reason.
    std::shared_ptr<const std::vector<uint32_t>> stepMasks;
    uint32_t stepWords = 0;
    uint32_t stepCount = 0;
    // Solar noon, the AM / PM split; and each step's time of day in minutes,
    // for the HUD's time slider.
    uint32_t noonStep = 0;
    std::vector<uint16_t> stepMinutes;

    size_t Bytes () const;
};

// How the tint pass tests depth. ⚠️ AN ABI with the `depth=` argument of
// Tapioca.ShowSunStudy and with the PSO array in DiligentSceneImpl.
enum class SunStudyDepthMode : uint32_t {
    Equal = 0,     // the correct one: the FRONTMOST surface, glass included (see the tint PSO)
    LessEqual = 1, // tolerant of a depth buffer that is not bit-identical
    Always = 2,    // no depth test at all -- draws through the model
};

// What `debugMode` selects. ⚠️ AN ABI WITH kArchVizSunTintPS's `if` ladder and
// with the `debug=` argument of Tapioca.ShowSunStudy.
enum class SunStudyDebugMode : uint32_t {
    Hours = 0,    // the study itself
    TileId = 1,   // a deterministic colour per atlas tile
    Gradient = 2, // cell column/row as red/green inside each tile
    Checker = 3,  // one-cell checkerboard inside each tile
    Roles = 4,    // each element by its role: analysis / context / ignored
    // The web study's shadow views, from the per-step bits.
    SingleShadow = 5, // lit or shadowed at ONE step (the HUD's time slider)
    AmPm = 6,         // never / AM only / PM only / AM + PM, split at solar noon
    ShadowFan = 7,    // the LAST chosen step at which each surface was shadowed
};

// The top of the HUD's hours-range slider, as on the web page: "9+" -- at it,
// nothing above is filtered out.
constexpr float kSunHoursFilterOpenTop = 10.0f;

// What the HUD sets on the tint each frame: the hours range, the view it has
// chosen over the commanded one (-1 = as commanded), and the step the single
// shadow view shows. HUD-ONLY STATE; ShowSunStudy never writes it.
struct SunStudyViewSettings {
    float lo = 0.0f;
    float hi = kSunHoursFilterOpenTop;
    bool hide = false;
    int viewOverride = -1;
    uint32_t step = 0;
    // The shadow fan's chosen steps, as bits (SunStudy/SunStudyStepAtlas.hpp
    // kFanWords), and how many there are -- the colour ramp's length.
    uint32_t fanMask[3] = { 0u, 0u, 0u };
    uint32_t fanCount = 0;
};

// Build one element's side car for the ROLE view.
//
// Same shape as BuildSunStudyElementMap's -- one record per RENDERED triangle,
// the same permutation, the same topology hash -- so the binder treats it
// identically. Every record carries a 1x1 tile with the element's role
// (an evp::sunstudy::ElementRole value) in `tile[0]`, which the shader's mode 4
// reads before any lattice or atlas lookup.
//
// ⚠️ ONE ROLE FOR EVERY TRIANGLE OF THE ELEMENT. Roles belong to elements, and
// an element half one colour would mean the mapping, not the model, is wrong.
bool BuildSunStudyRoleMap (uint8_t role, const std::vector<uint32_t>& triangles,
                           const std::vector<int32_t>& triMaterial, SunStudyElementMap& out);

// Build one element's side car.
//
// `tiles` and `layouts` are the study's own per-face arrays -- `SunStudyAtlas::tiles`
// and `SampleGrid::layouts`, both one entry per SOURCE face, in source order.
// ⚠️ THE TWO VECTORS RATHER THAN THE TWO AGGREGATES THEY LIVE IN, because that
// is all this needs and a study's sample arrays are megabytes: asking for the
// whole SampleGrid would make a display change copy every position, normal and
// area in the study to read a few hundred bytes of layout.
//
// `triangles` and `triMaterial` are the SOURCE mesh's, exactly as they were
// handed to BuildMaterialGroups for the upload. `faceBase` is where this mesh's
// triangles start in the study's concatenated face list -- StartSunStudy
// concatenates the snapshot's meshes in order, so it is the running triangle
// count of the meshes before this one.
//
// Returns false when the study has no atlas, or when `faceBase` plus the
// triangle count runs off the end of it -- which means the caller paired an
// element with the wrong study and must not draw either way.
bool BuildSunStudyElementMap (const std::vector<evp::sunstudy::AtlasTile>& tiles,
                              const std::vector<evp::sunstudy::FaceLayout>& layouts, double spacing,
                              const std::vector<uint32_t>& triangles, const std::vector<int32_t>& triMaterial,
                              uint32_t faceBase, SunStudyElementMap& out);

// What should happen to one element's side buffer when a study is bound to the
// scene as it stands right now.
//
// ⚠️ IT IS A FIVE-WAY ANSWER AND EVERY ONE OF THEM IS DIFFERENT. Collapsing
// any two produces a viewer that is confidently wrong or silently blank:
//
//   * NotYetReceived and RefusedTopologyHash both mean "no tint on this element"
//     and mean OPPOSITE things about what to do -- one resolves itself at the
//     next batch, the other never will.
//   * AlreadyBound and Attach both end with a bound buffer, and rebuilding one
//     GPU buffer per element on every EndBatch is what would make the overlay
//     cost something on a live-sync session that changed nothing.
//   * RefusedTriangleCount is a HARD safety stop, not a quality setting: the
//     buffer is indexed by SV_PrimitiveID with no bounds check in hardware.
enum class SunFaceBinding {
    Attach,               // create the buffer: the element is here and agrees
    AlreadyBound,         // a valid buffer is bound; leave it alone
    NotYetReceived,       // the scene does not hold this element YET
    RefusedTriangleCount, // it holds a different number of triangles
    RefusedTopologyHash,  // same count, different geometry or material order
};

// What the scene knows about one element, as the binder needs it.
struct SceneElementFacts {
    bool present = false;       // the scene holds an element with this GUID
    bool alreadyBound = false;  // ...and it already has this study's side buffer
    uint32_t triangleCount = 0; // its index count / 3
    uint64_t topologyHash = 0;  // MeshIndexHash of its reordered index buffer
};

// Pure, and the one place the rule lives. `AttachSunStudy` on the render thread
// is a loop over this plus the GPU calls it decides.
SunFaceBinding ClassifySunFaceBinding (const SunStudyElementMap& map, const SceneElementFacts& element);

// The CPU twin of the tint shader's lookup: the atlas texel a point on `face`
// resolves to, or -1 when the face is not in the atlas.
//
// ⚠️ IT IS A NEAREST-CELL LOOKUP, NOT `AtlasUv`. AtlasUv returns a continuous
// coordinate clamped to [0, tileSize] for bilinear filtering, which at a tile's
// far edge lands ON the gutter texel; the first tint pass point-samples, so it
// wants the CELL a point falls in and its texel centre. The two agree
// everywhere inside the tile and differ exactly where the gutter would bleed.
//
// It exists so the offline test can check the mapping against
// `SunStudyAtlas::texels` — the table the hours were scattered through — rather
// than against a second copy of the same arithmetic.
int64_t SunStudyTexelAt (const SunFaceMap& face, const double point[3], uint32_t atlasWidth, uint32_t atlasHeight);

} // namespace archviz
} // namespace geomsrv

#endif
