#ifndef EVP_ARCHVIZ_HUDVIRTUALEDITORDETAIL_HPP
#define EVP_ARCHVIZ_HUDVIRTUALEDITORDETAIL_HPP

// What the virtual editor's view host and its panel share: colours and the edits both
// can start (undo, redo, delete). Not an interface: only HudVirtualEditor*.cpp include it.
#include "ArchViz/HudVirtualEditor.hpp"
#include <imgui.h>

namespace geomsrv::archviz::edithud::detail {

inline const ImU32 kAccent = IM_COL32 (255, 176, 32, 255);
inline const ImU32 kSnap = IM_COL32 (255, 214, 10, 255);
inline const ImU32 kGuide = IM_COL32 (180, 110, 240, 255);
inline const ImU32 kHover = IM_COL32 (255, 255, 255, 255);
inline const ImU32 kInk = IM_COL32 (30, 34, 40, 255);
inline const ImU32 kPaper = IM_COL32 (245, 247, 250, 235);
inline const ImU32 kAxis[3] = { IM_COL32 (229, 72, 77, 255), IM_COL32 (70, 167, 88, 255), IM_COL32 (62, 99, 221, 255) };

const edit::Document& Doc (const Session& s);
void History (Session& s, bool undo);
void DeleteSelection (Session& s);

} // namespace geomsrv::archviz::edithud::detail
#endif
