#ifndef EVP_ARCHVIZ_OVERLAYFONTS_HPP
#define EVP_ARCHVIZ_OVERLAYFONTS_HPP

// ArchViz/OverlayFonts -- the font a caller names for overlay text, found: an installed
// family by the name Windows lists it under ("Arial", "Segoe UI Semibold", "Consolas
// Bold"), or a font file by its path. Absent, the overlays' own bundled font
// (SceneTextFont.hpp) is used.
//
// ⚠️ A NAME IS RESOLVED ONCE, WHEN A VERB READS IT, AND THE PATH IS WHAT TRAVELS. A
// caller learns at once that a family is not installed, in a sentence; the layers, the
// text engines and the HUD key on the file, so two names for one file are one font.
//
// The registry lists a face under "Name (TrueType)" or "Name (OpenType)", several faces
// of a collection as "A & B (TrueType)"; the machine's fonts are HKLM's and a user's own
// HKCU's, each value a file name in the Windows fonts folder or a full path.
//
// Any thread; reads the registry and the disk.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayfonts {

// Whether the registry value named `valueName` lists the face `requested`,
// case-insensitively: "Arial (TrueType)" lists "Arial", "Cambria & Cambria Math
// (TrueType)" lists both. Pure.
bool Lists (const std::string& valueName, const std::string& requested);

// The file `name` means -- a path that exists, or an installed face -- in `path`;
// false with `error` a sentence a caller can act on.
bool Resolve (const std::string& name, std::string& path, std::string& error);

// The bytes of the font file at `path`.
bool Read (const std::string& path, std::vector<uint8_t>& bytes, std::string& error);

} // namespace overlayfonts
} // namespace archviz
} // namespace geomsrv

#endif
