#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorTerrainState.hpp"
#include "Edit/EditDisplayUnits.hpp"
#include "Edit/EditTerrainDetail.hpp"
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

// Mesh detail: how closely the shown and baked surface follows the build. The existing ground,
// fixed heights and the authored lines are exact at every choice.
void MeshDetailControl (Session& s)
{
    if (!s.terrainDesign)
        return;
    namespace lg = tapioca::geometry;
    const double choices[] = { lg::kMeshDetailFine, lg::kMeshDetailStandard, lg::kMeshDetailCoarse,
                               lg::kMeshDetailFull };
    const char* names[] = { "Fine", "Standard", "Coarse", "Full" };
    const auto units = edit::CurrentDisplayUnits ();
    const auto label = [&] (int i) {
        return choices[i] > 0.0 ? std::string (names[i]) + " (" + edit::FormatLength (units, choices[i]) + ")"
                                : std::string ("Full (every built triangle)");
    };
    const double now = s.terrainDesign->options.meshDetail;
    int current = -1;
    for (int i = 0; i < 4; ++i)
        if (choices[i] == now)
            current = i;
    const std::string shown = current >= 0 ? label (current) : edit::FormatLength (units, now);
    ImGui::SetNextItemWidth (190.0f);
    if (ImGui::BeginCombo ("Mesh detail", shown.c_str ())) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable (label (i).c_str (), i == current))
                SetMeshDetail (s, choices[i]);
        ImGui::EndCombo ();
    }
    if (ImGui::IsItemHovered ())
        geomsrv::archviz::hudshell::HoverTip (
            "How closely the mesh shown and baked into Archicad follows the built terrain, in height. The "
            "existing ground, fixed heights and the paths' and regions' lines stay exact; the slopes and "
            "the rest keep only the points they need. Volumes are always the full build's.");
    if (const auto* detailed = ShownTerrainDetail (s))
        ImGui::TextDisabled ("Mesh: %zu triangles (%zu built)", detailed->triangles.size () / 3,
                             detailed->builtTriangles);
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
    MeshDetailControl (s);
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
void MeshDetailControl (Session&)
{
}
#endif

} // namespace geomsrv::archviz::edithud
