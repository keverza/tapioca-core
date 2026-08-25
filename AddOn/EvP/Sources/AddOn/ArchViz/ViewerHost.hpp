#ifndef EVP_ARCHVIZ_VIEWERHOST_HPP
#define EVP_ARCHVIZ_VIEWERHOST_HPP

// The seam between "what we render into" and "who owns that window".
//
// Today the host is a DG::UserItem inside a Tapioca palette (ArchVizPanel).
// Tomorrow it could be a plain Win32 window, or the click-through overlay of
// Part II/III. The renderer must not care, so everything it is allowed to know
// about its window lives in this one struct: a native handle and a size.
//
// The Diligent viewer specification makes this seam explicit rather than
// retrofitting renderer details into the window owner.
//
// ⚠️ NOTHING IN ArchViz/RenderThread MAY INCLUDE A DG OR ACAPI HEADER. That is
// what this file is for. The render thread never calls ACAPI, never touches DG
// and never calls MainThreadGate (CLAUDE.md, and plan section 2's thread map);
// keeping its types free of Archicad's is how that rule stays checkable by
// reading the includes rather than by discipline.

#include "ArchViz/DiligentViewportTarget.hpp" // SurfaceMode

#include <cstdint>

namespace geomsrv {
namespace archviz {

struct Surface {
    // WHICH KIND of window `nwh` is, which decides how frames reach the screen:
    // a Diligent HWND swap chain on the palette child, or a DirectComposition
    // visual on the click-through overlay. ⚠️ IT IS NOT A RENDERING PREFERENCE --
    // the two paths differ in the swap chain's creation call, its alpha mode, its
    // clear colour, where the depth buffer comes from and how it is presented.
    // DiligentViewportTarget.hpp is the whole account. Both active surfaces
    // are explicit because their swap-chain and alpha rules differ.
    SurfaceMode mode = SurfaceMode::PaletteChild;
    // HWND on Windows, wrapped by Diligent's NativeWindow at the presentation
    // boundary. `void*` keeps this header independent of <windows.h>.
    void* nwh = nullptr;
    // PHYSICAL pixels — what the backbuffer is sized in. The host is
    // responsible for the logical->physical conversion; getting that wrong is
    // what made the plan overlay pan at two-thirds speed on a 150% display
    // (PlanOverlay/OverlayWindow.hpp's DISPLAY SCALING note).
    uint32_t width = 0;
    uint32_t height = 0;
    // Embedded command preview: render retained watch annotations, but never
    // consume the shared full-model queue or add viewer helper geometry/HUD.
    bool retainedAnnotationsOnly = false;

    bool IsValid () const
    {
        return width > 0 && height > 0 && (mode == SurfaceMode::Offscreen || nwh != nullptr);
    }
};

} // namespace archviz
} // namespace geomsrv

#endif
