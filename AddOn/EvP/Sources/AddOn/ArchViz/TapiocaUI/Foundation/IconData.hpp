#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_ICONDATA_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_ICONDATA_HPP

// TapiocaUI/Foundation/IconData -- the shape of the generated glyph tables (Iconoir.gen.cpp), for
// Icons.cpp and the tests. A caller draws by IconId through Icons.hpp and never reads these.
//
// ⚠️ FLATTENED ONCE, AT BUILD TIME. Every curve of a glyph was turned into a polyline by
// tools/generate_iconoir_icons.py, in units of the glyph's 24 x 24 grid divided by 24 (0 to 1),
// so drawing is a transform of points already there -- nothing is tessellated per frame and
// nothing is parsed at run time.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace ui {
namespace icondata {

// One subpath: `count` points from `first` in the point table, joined back to its start when
// `closed`. A subpath whose points all lie within a hair of each other is one of Iconoir's dots
// -- a zero-length stroke its round cap makes visible -- and is drawn as a disc.
struct Stroke {
    uint32_t first = 0;
    uint16_t count = 0;
    bool closed = false;
};

// One glyph: `count` strokes from `first` in the stroke table, the file it came from and its
// name in IconId.hpp.
struct Glyph {
    const char* file = "";
    const char* name = "";
    uint32_t first = 0;
    uint16_t count = 0;
};

// The stroke width every glyph is drawn with, in the same units: Iconoir's 1.5 on its 24 grid.
constexpr float kStrokeWidth = 1.5f / 24.0f;

// The tables, indexed by IconId's value; an id past the end is `None`.
uint32_t GlyphCount ();
const Glyph& GlyphAt (uint32_t index);
const Stroke* Strokes ();
const float* Points (); // x, y pairs

} // namespace icondata
} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif
