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

// ⚠️ A PAGE'S OWN TABS ARE ITS TAB'S CHILDREN, AND LOOK IT (the user, 2026-10-21: "Tabs that have
// subtabs should have same selected color for whole group to indicate that it is child tabs.
// Selected subtab then should be darker."). The child row sits on a band in the colour of the
// panel's selected tab; a child at rest is that band, pointed at a step darker, and the one shown
// darker still -- grey ink laid over the band, with no accent line. True when the bar is open; then
// draw its tab items and close it with EndSubTabs.
bool BeginSubTabs (const char* id);
void EndSubTabs ();

} // namespace hudshell
} // namespace archviz
} // namespace geomsrv

#endif
