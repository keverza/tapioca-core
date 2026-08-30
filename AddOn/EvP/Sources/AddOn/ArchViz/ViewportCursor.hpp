#ifndef EVP_ARCHVIZ_VIEWPORTCURSOR_HPP
#define EVP_ARCHVIZ_VIEWPORTCURSOR_HPP

// ArchViz/ViewportCursor — native input and cursor for the viewport child HWND.
//
// THE SYMPTOM: the pointer keeps whatever shape it had when it crossed into the
// viewport. Entering across the palette's splitter leaves a horizontal
// resize arrow sitting over the 3D view, and it stays there until the pointer
// leaves and comes back some other way.
//
// THE CAUSE: a cursor is not a property of where the pointer IS, it is decided
// by whichever window answers `WM_SETCURSOR`. The viewport is a `DG::UserItem`
// child created by Archicad's own DG layer, and neither its window class nor DG
// answers that message for us -- so nobody sets a cursor at the moment the
// pointer arrives, and the last window to set one wins by default.
//
// ⚠️ THE FIX IS PER-WINDOW SUBCLASSING, NEVER `SetClassLongPtr(GCLP_HCURSOR)`.
// The class here is `DGUserItemClass`, which every DG user item in Archicad
// shares -- setting the cursor on the CLASS would change it for unrelated
// controls in unrelated palettes, including Archicad's own. The blast radius of
// a class change is the whole application; a subclass reaches exactly one HWND.
//
// ⚠️ IT MUST BE REMOVED BEFORE THE WINDOW DIES. The subclass stores the previous
// WndProc and restores it on Detach; a subclass left installed over a destroyed
// window, or over one whose owner has unloaded, calls into freed code -- the same
// rule the nav timer and the selection bridge live under.
//
// MAIN THREAD ONLY: window procedures belong to the thread that created the
// window and pumps its messages.

namespace geomsrv {
namespace archviz {
namespace viewportcursor {

// Install native pointer/button/wheel handling and the arrow cursor on `hwnd`.
// DG remains the palette shell and does not feed the renderer's input stream.
bool Attach (void* hwnd);

// Restore the original WndProc. Safe to call when nothing is attached.
void Detach ();

} // namespace viewportcursor
} // namespace archviz
} // namespace geomsrv

#endif
