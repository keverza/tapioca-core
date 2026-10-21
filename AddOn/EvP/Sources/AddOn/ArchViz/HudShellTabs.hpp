#ifndef EVP_ARCHVIZ_HUDSHELLTABS_HPP
#define EVP_ARCHVIZ_HUDSHELLTABS_HPP

// ArchViz/HudShellTabs -- one tab of the floating panel's tab row, for HudShell.cpp's Host: its
// width in the row, and the tab itself with its glyph, badge and tip. Its own file so the tab row
// can grow without growing HudShell.cpp past its cap.
//
// ⚠️ PURE IMGUI, as HudShell.hpp: the caller has a context current under ImGuiContextLock and is
// inside the panel's BeginTabBar.

#include "ArchViz/HudShell.hpp"

#include <imgui.h>

namespace geomsrv {
namespace archviz {
namespace hudshell {

// `tab`'s width in the row, its padding included.
float TabWidth (const HostTab& tab);

// `tab` as an ImGui tab item asked with `flags`: its title, or its glyph and badge with the title
// as its tip. True while its page is shown -- the caller then draws the page and EndTabItem.
bool TabItem (const HostTab& tab, ImGuiTabItemFlags flags);

} // namespace hudshell
} // namespace archviz
} // namespace geomsrv

#endif
