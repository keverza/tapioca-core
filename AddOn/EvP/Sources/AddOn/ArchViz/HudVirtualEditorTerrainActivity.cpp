#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorTerrainState.hpp"
#include "Edit/EditDisplayUnits.hpp"
#include "Edit/EditTerrainDetail.hpp"
#include "Geometry/GeometryRustTerrain.hpp"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

// #175: the terrain being regenerated, said in the main editor panel. Read-only over the
// design's status; no separate viewport badge.
namespace geomsrv::archviz::edithud {

namespace {
std::string Clock (double seconds)
{
    const int whole = int (std::max (seconds, 0.0) + 0.5);
    char text[16];
    std::snprintf (text, sizeof text, "%d:%02d", whole / 60, whole % 60);
    return text;
}
} // namespace

// Most of a large build is the refinement: its share of the bar is how far the worst error has
// come down from the first round towards the tolerance, on a log scale (each round roughly
// divides it). The angle work after the error is met is its last tenth.
TerrainBuildBar MakeTerrainBuildBar (const TerrainBuildBarInput& in)
{
    TerrainBuildBar bar;
    const auto units = edit::CurrentDisplayUnits ();
    float fraction = 0.0f;
    if (in.requested)
        bar.label = "Starting the build";
    else if (in.stage == 2) {
        double done = 0.0;
        if (in.residual <= in.tolerance)
            done = 1.0;
        else if (in.first > in.tolerance && in.residual > 0.0)
            done = std::clamp (std::log (in.first / in.residual) / std::log (in.first / in.tolerance), 0.0, 1.0);
        fraction = float (0.1 + 0.8 * done);
        bar.label = in.residual <= in.tolerance ? "Refining: evening out triangles"
                                                : "Refining: " + edit::FormatLength (units, in.residual) + " off";
        bar.detail = "Round " + std::to_string (in.round) + ", " + std::to_string (in.added) + " points added, " +
                     std::to_string (in.pending) + " triangles to fix";
    }
    else if (in.stage == 3) {
        fraction = 0.92f;
        bar.label = "Heights and volumes";
    }
    else if (in.stage == 1 || in.shown < 0.5f) {
        fraction = 0.05f;
        bar.label = "Preparing the slopes";
    }
    else { // the kernel is done; the job simplifies the mesh to its detail
        fraction = 0.96f;
        bar.label = "Simplifying to the Mesh detail";
    }
    bar.fraction = std::clamp (std::max (fraction, in.shown), 0.0f, 1.0f);
    bar.label += "  " + Clock (in.seconds);
    if (in.lastSeconds > 0.0)
        bar.detail += std::string (bar.detail.empty () ? "" : "; ") + "the last build took " + Clock (in.lastSeconds);
    return bar;
}

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
    // The panel's width: the bar has room for what the build is doing (the user, 2026-11-18).
    const float width = std::max (ImGui::GetContentRegionAvail ().x, 120.0f);
    if ((status.pending || status.buildRequested) && s.terrainDesign) {
        auto& state = *s.terrainDesign;
        const auto progress = lg::ReadTerrainBuildProgress ();
        TerrainBuildBarInput input;
        input.requested = !status.pending;
        input.stage = uint32_t (progress.stage);
        input.round = progress.round, input.added = progress.added, input.pending = progress.pending;
        input.residual = progress.residual, input.first = progress.first, input.tolerance = progress.tolerance;
        input.seconds =
            status.pending
                ? std::chrono::duration<double> (std::chrono::steady_clock::now () - state.buildStarted).count ()
                : 0.0;
        input.lastSeconds = state.lastBuildSeconds;
        input.shown = state.buildShown;
        const auto bar = MakeTerrainBuildBar (input);
        state.buildShown = bar.fraction;
        ImGui::ProgressBar (bar.fraction, ImVec2 (width, 0.0f), bar.label.c_str ());
        if (ImGui::IsItemHovered ())
            geomsrv::archviz::hudshell::HoverTip (
                "Building the terrain mesh. Most of the time goes into refining the slopes until every "
                "triangle is within a centimetre of them; the preview stays on screen meanwhile.");
        if (!bar.detail.empty ())
            ImGui::TextDisabled ("%s", bar.detail.c_str ());
    }
    else {
        ImGui::BeginDisabled (s.edits.InGesture ());
        if (ImGui::Button ("Build mesh", ImVec2 (width, 0.0f)))
            RequestTerrainBuild (s);
        ImGui::EndDisabled ();
        if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
            geomsrv::archviz::hudshell::HoverTip (
                "The terrain shown follows the paths as you edit, from its slopes directly. Build mesh "
                "triangulates it as an Archicad Mesh is built from it: a minute or more on a large site.");
    }
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
