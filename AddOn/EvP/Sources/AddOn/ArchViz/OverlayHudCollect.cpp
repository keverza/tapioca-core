#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudClip.hpp"
#include "ArchViz/RoofSlopeCanvas.hpp"

namespace geomsrv::archviz::overlayhud {
// Panel clipping is unchanged. Roof furniture carries model anchors into the
// existing glyph stream instead of baking yesterday's projection into pixels.
void Engine::Impl::Collect (Layout& out, uint32_t& unsampled)
{
    const ImDrawData* data = ImGui::GetDrawData ();
    if (!data)
        return;
    std::vector<hudclip::Corner> cut;
    for (const ImDrawList* list : data->CmdLists) {
        const int panel = PanelOf (list);
        Built& into = panel < 0                              ? out.overlay
                      : size_t (panel) < out.panels.size ()  ? out.panels[size_t (panel)]
                      : size_t (panel) == out.panels.size () ? out.dock
                                                             : out.host;
        const ImVec2 origin = panel >= 0 ? windows[size_t (panel)]->Pos : ImVec2 (0, 0);
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback || command.ElemCount == 0)
                continue;
            const ImTextureID id = command.GetTexID ();
            const size_t slot = id == ImTextureID_Invalid ? pages.size () : size_t (id - 1);
            if (slot >= pages.size () || !pages[slot]) {
                ++unsampled;
                continue;
            }
            const hudclip::Rect clip { command.ClipRect.x, command.ClipRect.y, command.ClipRect.z, command.ClipRect.w };
            const auto corner = [&] (unsigned int e) {
                const auto& v = list->VtxBuffer[int (command.VtxOffset + list->IdxBuffer[int (command.IdxOffset + e)])];
                return hudclip::Corner { v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col };
            };
            for (unsigned int e = 0; e + 2 < command.ElemCount; e += 3) {
                cut.clear ();
                hudclip::Clip (corner (e), corner (e + 1), corner (e + 2), clip, cut);
                for (const auto& c : cut) {
                    Vertex v { c.x - origin.x, c.y - origin.y, c.u, c.v, hudshell::Unpacked (c.col), uint32_t (slot) };
                    if (panel < 0 && store->roofEditor)
                        roofcanvas::Bind (store->roofEditor->canvas, list, int (command.IdxOffset + e), v);
                    else if (panel < 0 && store->virtualEditor)
                        roofcanvas::Bind (store->virtualEditor->canvas, list, int (command.IdxOffset + e), v);
                    into.vertices.push_back (v);
                }
            }
        }
    }
}
} // namespace geomsrv::archviz::overlayhud
