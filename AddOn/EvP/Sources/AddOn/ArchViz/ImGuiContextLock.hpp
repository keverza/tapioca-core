#ifndef EVP_ARCHVIZ_IMGUICONTEXTLOCK_HPP
#define EVP_ARCHVIZ_IMGUICONTEXTLOCK_HPP

// ArchViz/ImGuiContextLock -- the one lock around Dear ImGui in the add-on.
//
// ⚠️ IMGUI'S CURRENT CONTEXT IS ONE GLOBAL FOR THE WHOLE PROCESS, NOT PER THREAD.
// `GImGui` is a plain pointer in the vendored imgui.cpp (1.92.1, no thread-local
// override in Diligent's config), and every ImGui call reads it. Two users run on two
// threads: the viewer's HUD on the Diligent viewport's render thread
// (DiligentHud.hpp), and the overlays' HUD panels, laid out on Archicad's main thread
// (OverlayHud.hpp). Without this lock one could set its context while the other was
// half-way through a frame, and the other would carry on building into the wrong
// one.
//
// Held for a whole frame -- context switch, NewFrame, Render, draw -- never across
// anything that waits on the other thread. It is never taken in a Present hook: the
// overlays draw the triangles the main thread already laid out (§11).

#include <mutex>

namespace geomsrv {
namespace archviz {

std::mutex& ImGuiContextMutex ();

} // namespace archviz
} // namespace geomsrv

#endif
