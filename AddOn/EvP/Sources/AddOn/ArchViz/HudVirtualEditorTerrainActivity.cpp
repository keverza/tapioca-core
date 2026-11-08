#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include <imgui.h>
#include <string>

// #175: the terrain being regenerated, said where the user looks. Read-only over the design's
// status; OVERLAY-INVARIANTS.md §15: drawn on the foreground list, no window, so it takes no
// hover and no part in the cursor claim.
namespace geomsrv::archviz::edithud {

#if defined(TAPIOCA_GEOMETRY_RUST)
void TerrainActivityBadge (const Session& s)
{
    if (!s.terrainDesign || !s.terrainLive)
        return;
    const auto status = GetTerrainDesignStatus (s);
    if (!status.previewPending && !status.pending && !status.previewed)
        return;
    // Three dots that walk while something is computing; still when only the mesh is behind.
    const int dots = 1 + int (ImGui::GetTime () * 3.0) % 3;
    std::string words = status.previewPending ? "Updating terrain preview"
                        : status.pending      ? "Building terrain mesh - showing the preview"
                                              : "Terrain preview - the mesh is not built";
    if (status.previewPending || status.pending)
        words += std::string (size_t (dots), '.');
    const ImU32 amber = IM_COL32 (245, 158, 11, 255);
    ImGui::PushStyleColor (ImGuiCol_Text, amber);
    ImGui::TextUnformatted (words.c_str ());
    ImGui::PopStyleColor ();
    // The view's badge: top centre of the main viewport, on the foreground list.
    const ImGuiViewport* view = ImGui::GetMainViewport ();
    ImDrawList* draw = ImGui::GetForegroundDrawList ();
    if (!view || !draw)
        return;
    const ImVec2 size = ImGui::CalcTextSize (words.c_str ());
    const float pad = 6.0f;
    const ImVec2 at (view->WorkPos.x + (view->WorkSize.x - size.x) * 0.5f, view->WorkPos.y + 8.0f);
    draw->AddRectFilled (ImVec2 (at.x - pad, at.y - pad * 0.5f),
                         ImVec2 (at.x + size.x + pad, at.y + size.y + pad * 0.5f), IM_COL32 (30, 30, 30, 200), 6.0f);
    draw->AddText (at, amber, words.c_str ());
}
#else
void TerrainActivityBadge (const Session&)
{
}
#endif

} // namespace geomsrv::archviz::edithud
