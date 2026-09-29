#ifndef EVP_ARCHVIZ_OVERLAYGUESTTEXT_HPP
#define EVP_ARCHVIZ_OVERLAYGUESTTEXT_HPP

// ArchViz/OverlayGuestText -- the overlays' one text engine (OverlayText.hpp), made
// with the bundled font on first use: both overlays lay out with it, so a glyph
// rasterised for the plan is not rasterised again for 3D.
//
// MAIN THREAD ONLY -- the engine is.

namespace geomsrv {
namespace archviz {

namespace overlaytext {
class Engine;
}

namespace guesttext {

// The engine, or null when the bundled font could not be read or its seed atlas not
// built -- said once in archviz.log. The first call pays for the seed atlas.
overlaytext::Engine* Engine ();

} // namespace guesttext
} // namespace archviz
} // namespace geomsrv

#endif
