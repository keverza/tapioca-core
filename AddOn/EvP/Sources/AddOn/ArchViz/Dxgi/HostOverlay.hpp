#ifndef EVP_ARCHVIZ_DXGI_HOSTOVERLAY_HPP
#define EVP_ARCHVIZ_DXGI_HOSTOVERLAY_HPP

// Archicad's OWN extracted building, drawn as an analysis overlay: edges, and
// surfaces coloured by a scalar field (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 10).
//
// ⚠️ THE GHOST AND THIS ARE ABOUT DIFFERENT GEOMETRY, AND CONFLATING THEM IS
// WHAT THE PLACEHOLDERS DID. `GhostMesh` draws PROPOSED geometry -- something
// that is not in the model yet -- and grew a wireframe box and a 9x9 gradient
// grid so the three overlay kinds could be exercised before any host geometry
// existed. Those were stand-ins. A wireframe overlay means "the outlines of the
// building that IS there", and a surface heatmap means "the building's OWN
// surfaces, coloured" -- both of which need the extracted model, which
// `HostOccluders` now holds.
//
// ⚠️ SO THIS DRAWS THE OCCLUDER'S BUFFERS AND BUILDS NOTHING OF ITS OWN. It
// borrows `hostocclusion::GetGeometry ()`. Two modules each keeping a copy of
// Archicad's model would drift apart the first time one changed its format, and
// the copy is the largest thing on this path.
//
// ⚠️ AND IT SHARES THE DEPTH BUFFER THE OCCLUDER JUST SEEDED, which is what
// makes the result coherent rather than three overlays each guessing. Draw order
// inside one frame is the whole composition:
//
//     1. hostocclusion::Prepare   opaque building -> private depth
//     2. ghost solid              tests and writes that depth
//     3. host heatmap             tests it, biased, no write   (surfaces)
//     4. host wireframe           tests it, biased, no write   (edges, last)
//
// Edges last because an edge one pixel behind a surface it belongs to is
// invisible for no reason a reader could ever guess from the picture.
//
// THREAD. `Prepare`/`Draw` run on Archicad's render thread inside a detour,
// under `ScopedInjectionGuard`, with a `ScopedPipelineState` guard already
// holding the caller's bindings. No allocation, no lock, never ACAPI.

#include <cstdint>

struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;

namespace geomsrv {
namespace archviz {
namespace dxgi {

namespace overlay {
struct OverlayStyle;
}

namespace hostoverlay {

enum class Kind {
    // Solid host surfaces coloured by the published scalar field. ⚠️ BIASED AND
    // NON-WRITING: it is coplanar with the depth the occluder wrote from the
    // same triangles, so without a bias every pixel fights its own occluder and
    // the surface stipples.
    Heatmap,
    // The same triangles in wireframe fill. The hidden part is drawn faded
    // rather than dropped, because where a member goes after it enters a wall is
    // exactly what an analysis overlay is for -- see `OverlayStyle::hiddenOpacity`.
    Wireframe,
};

// RENDER THREAD. Draw the published building in one style. Does nothing and
// costs one branch when there is no snapshot, no camera, or the style asks for
// an overlay the caller has turned off.
//
// `depthView` is the buffer `hostocclusion::Prepare` returned, already bound by
// the caller; it is passed so this module never has to guess which depth it is
// composing against.
void Draw (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation, Kind kind,
           const overlay::OverlayStyle& style, ID3D11DepthStencilView* depthView);

// MAIN THREAD. Each kind is switched separately, because they answer different
// questions and a reader looking at one does not want the other over it.
void SetEnabled (Kind kind, bool enabled);
bool Enabled (Kind kind);

struct Stats {
    uint64_t heatmapDraws = 0;
    uint64_t wireframeDraws = 0;
    uint64_t hiddenPassDraws = 0;
    uint64_t culledPasses = 0;
    uint64_t skippedNoGeometry = 0;
    uint64_t skippedNoCamera = 0;
    uint32_t trianglesDrawn = 0;
    bool ready = false;
    char lastError[160] = {};
};
Stats GetStats ();

// MAIN THREAD, at teardown.
void Shutdown ();

} // namespace hostoverlay
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
