#ifndef EVP_ARCHVIZ_OVERLAYGUESTTEXT_HPP
#define EVP_ARCHVIZ_OVERLAYGUESTTEXT_HPP

// ArchViz/OverlayGuestText -- the overlays' one text engine (OverlayText.hpp) and one
// HUD engine (OverlayHud.hpp), each made with the bundled font on first use: both
// overlays lay out with them, so a glyph rasterised for the plan is not rasterised
// again for 3D.
//
// MAIN THREAD ONLY -- the engine is.

#include <cstddef>
#include <string>

namespace geomsrv {
namespace archviz {

namespace overlaytext {
class Engine;
}
namespace overlayhud {
class Engine;
}

namespace guesttext {

// The engine, or null when the bundled font could not be read or its seed atlas not
// built -- said once in archviz.log. The first call pays for the seed atlas.
overlaytext::Engine* Engine ();

// The engine for the font file at `path` (overlayfonts::Resolve made it), made on first
// use with a small seed; the bundled font's for an empty path. Null when the file
// cannot be read or `kMaxFonts` are already made -- said once per path.
constexpr size_t kMaxFonts = 8;
overlaytext::Engine* EngineFor (const std::string& path);

// The HUD panels' ImGui engine, or null when it could not start -- said once.
overlayhud::Engine* Hud ();

} // namespace guesttext
} // namespace archviz
} // namespace geomsrv

#endif
