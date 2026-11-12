#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include <imgui.h>
#include <string>

// #175: the terrain being regenerated, said in the main editor panel. Read-only over the
// design's status; no separate viewport badge.
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
}

void TerrainBuildSection (Session& s)
{
    const auto status = GetTerrainDesignStatus (s);
    ImGui::BeginDisabled (status.pending || status.buildRequested || s.edits.InGesture ());
    if (ImGui::Button (status.pending || status.buildRequested ? "Building mesh..." : "Build mesh"))
        RequestTerrainBuild (s);
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        geomsrv::archviz::hudshell::HoverTip (
            "The terrain shown follows the paths as you edit, from its slopes directly. Build mesh "
            "triangulates it as an Archicad Mesh is built from it: seconds on a large site.");
    if (!status.previewed)
        return;
    ImGui::Text ("Preview: cut %.1f m3   fill %.1f m3", status.previewCut, status.previewFill);
    if (!status.previewUnmet.empty ()) {
        ImGui::TextUnformatted ("Not everything asked for is met:");
        for (const auto& line : status.previewUnmet)
            ImGui::BulletText ("%s", line.c_str ());
    }
}
#else
void TerrainActivityBadge (const Session&)
{
}
bool RequestTerrainBuild (Session& s)
{
    s.message = "Terrain design requires the Rust geometry build.";
    return false;
}
void TerrainBuildSection (Session&)
{
}
#endif

} // namespace geomsrv::archviz::edithud
