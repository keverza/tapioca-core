#ifndef EVP_ARCHVIZ_OVERLAYTEXT_HPP
#define EVP_ARCHVIZ_OVERLAYTEXT_HPP

// ArchViz/OverlayText -- the overlays' text: a HarfBuzz-shaped run laid out as glyph
// quads around an anchor, and the MTSDF atlas pages those quads sample. The Diligent
// guest draws them (ArchViz/OverlayScene.hpp prepares the vertices).
//
// ⚠️ LAID OUT ONCE, PROJECTED EVERY FRAME ON THE GPU. The viewer's SceneTextLayer
// projects each anchor on the CPU and rebuilds its vertices on every draw. The
// overlays cannot: the plan's Present may not allocate (§11), and the 3D camera
// exists only on the GPU (finding 1). So a label is laid out here, ONCE, in pixels
// RELATIVE TO ITS ANCHOR, and the vertex shader adds the anchor's projected pixel --
// a label moves with the model in the frame the model moves in, at no CPU cost.
//
// ⚠️ THE SAME SHAPER, ATLAS AND GLYPH METRICS AS THE VIEWER (SceneTextLayout,
// SceneTextAtlas): one bundled Noto Sans, one MTSDF profile, the same quad
// arithmetic as SceneTextLayer's DrawPrepared, so a label reads the same in the
// viewer and over Archicad's views.
//
// ⚠️ SYNCHRONOUS, AND THAT IS A CHOICE. The viewer shapes and rasterises missing
// glyphs on workers because its frame loop must never wait. Here the caller is a
// verb or a tick on the main thread that runs when content CHANGES, not per frame:
// shaping a label is microseconds, and a glyph the seed atlas lacks costs one page
// generation, once per process. Asynchrony would buy a half-drawn label.
//
// MAIN THREAD ONLY. Pure otherwise -- no Diligent, no ACAPI -- so tests/cpp lays out
// real text against the bundled font.

#include "ArchViz/OverlayLayers.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlaytext {

// One atlas page: RGBA8 MTSDF, LINEAR data -- never an sRGB view.
struct Page {
    uint64_t id = 0; // unique in the process: the renderers' texture cache key
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

// A page id no other page has had in this process: the text pages' and the HUD
// panels' (OverlayHud.hpp) share one texture cache, keyed on it.
uint64_t NewPageId ();

// One glyph's quad in LOGICAL pixels from the label's anchor, x right, y down, and
// the atlas rectangle it samples: (u0, v0) at its top-left corner, (u1, v1) at its
// bottom-right.
struct Quad {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    uint32_t page = 0; // index into `Engine::Pages`
};

struct Label {
    std::vector<Quad> quads;
    // The ink's bounds in the same frame; empty (all zero) for a label with no ink.
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    uint32_t replaced = 0; // glyphs no page could hold, drawn as the replacement glyph
};

struct Stats {
    uint32_t pages = 0;
    uint64_t layouts = 0;
    uint64_t generatedGlyphs = 0;
    uint64_t replacedGlyphs = 0;
    uint32_t seedMilliseconds = 0;
};

class Engine final {
  public:
    // The distance range the atlas was generated with, in atlas pixels. The pixel
    // shader turns it into screen pixels with the uv derivatives.
    static float DistanceRangePixels ();
    // The em the atlas's glyphs were generated at, in atlas pixels: with the range, what
    // the pixel shader measures a text's size on screen by.
    static float EmPixels ();

    Engine ();
    ~Engine ();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    // Shapes the seed text and builds page 0 from it -- the slow part, once.
    bool Init (std::vector<uint8_t> fontBytes, std::string& error);
    bool Ready () const;

    // `utf8` at `sizePixels` per em, placed by `align` and `baseline` around (0, 0).
    // A glyph no page holds yet is generated into a new page, 64 at a time.
    bool Layout (const std::string& utf8, float sizePixels, overlaylayers::Align align,
                 overlaylayers::Baseline baseline, Label& out, std::string& error);

    // Every page so far, in index order. Pages are never removed or changed, so a
    // renderer keyed on `Page::id` uploads each once.
    const std::vector<std::shared_ptr<const Page>>& Pages () const;

    Stats GetStats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace overlaytext
} // namespace archviz
} // namespace geomsrv

#endif
