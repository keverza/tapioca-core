#ifndef EVP_ARCHVIZ_SCENETEXTFONT_HPP
#define EVP_ARCHVIZ_SCENETEXTFONT_HPP

// ArchViz/SceneTextFont -- the bundled Noto Sans, read out of the add-on's own
// resources: the one font the viewer's labels (SceneTextLayer) and the overlays'
// (OverlayText) are shaped and rasterised with, so a label reads the same in both.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {

// The font's bytes, from resource 32581 of the module this code is in. False, with
// the reason, when the resource is missing or malformed.
bool LoadBundledSceneTextFont (std::vector<uint8_t>& bytes, std::string& error);

} // namespace geomsrv::archviz

#endif
