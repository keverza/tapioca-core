#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include <imgui.h>
#include <algorithm>
#include <set>

// OVERLAY-INVARIANTS.md §§8,12,14: setup authors through existing drawing/history;
// native capture is an explicit queued action, never a layout/Present side effect.
namespace geomsrv::archviz::edithud {
namespace {
bool Idle (const Session& s)
{
    return !s.close && !s.terrainCapture && !GetTerrainDesignStatus (s).pending && !s.edits.InGesture () &&
           !GizmoModeLocked (s) && s.drawing.empty () && !s.landscapeCommand;
}

bool Ready (Session& s)
{
    if (s.edits.Doc ().landscape && Idle (s))
        return true;
    s.message = "Finish the current input or terrain build before changing terrain setup.";
    return false;
}

bool BeginDrawing (Session& s, bool property)
{
    if (!Ready (s))
        return false;
    s.tool = property ? Tool::Polyline : Tool::Path;
    s.landscapeMode = LandscapeMode::DrawSource;
    s.landscapeRole = property ? tapioca::geometry::SourceRole::PropertyLine : tapioca::geometry::SourceRole::Guide;
    s.message = property ? "Draw the property boundary in the view; close it on the first point."
                         : "Draw the design path in the view; press Enter to finish.";
    return true;
}
} // namespace

std::vector<edit::Id> TerrainDesignPaths (const Session& s)
{
    std::vector<edit::Id> result;
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return result;
    std::set<edit::Id> authored;
    for (const auto& source : doc.landscape->sources)
        if (!source.deleted && source.role == tapioca::geometry::SourceRole::PathCenterline && source.pathId)
            authored.insert (source.pathId);
    for (const auto& path : doc.paths)
        if (authored.count (path.id))
            result.push_back (path.id);
    return result;
}

edit::Id ResolveTerrainDesignPath (Session& s)
{
    const auto paths = TerrainDesignPaths (s);
    const auto available = [&] (edit::Id path) {
        return std::find (paths.begin (), paths.end (), path) != paths.end ();
    };
    if (s.terrainPath)
        return available (s.terrainPath) ? s.terrainPath : edit::kNoId;
    std::set<edit::Id> selected;
    for (const auto& ref : s.selection.Items ())
        if (edit::Exists (s.edits.Doc (), ref) && available (ref.entity))
            selected.insert (ref.entity);
    if (selected.size () == 1)
        s.terrainPath = *selected.begin ();
    else if (paths.size () == 1)
        s.terrainPath = paths.front ();
    return s.terrainPath;
}

bool ChooseTerrainDesignPath (Session& s, edit::Id path)
{
    if (!Ready (s))
        return false;
    const auto paths = TerrainDesignPaths (s);
    if (std::find (paths.begin (), paths.end (), path) == paths.end ()) {
        s.message = "Choose a live authored landscape path, not a native element or unbound curve.";
        return false;
    }
    s.terrainPath = path;
    s.message.clear ();
    return true;
}

bool EditTerrainDesignPath (Session& s)
{
    if (!Ready (s))
        return false;
    const auto path = ResolveTerrainDesignPath (s);
    if (!path) {
        s.message = "Draw or select an authored design path first.";
        return false;
    }
    s.selection.Set ({ { path, edit::Part::Entity, 0 } });
    s.profilePath = path;
    s.tool = Tool::Select;
    s.landscapeMode = LandscapeMode::Select;
    s.publishedRevision = UINT64_MAX;
    s.message = "Edit this path's profile and cross-section in the view or the path panel.";
    return true;
}

bool DrawTerrainDesignPath (Session& s)
{
    return BeginDrawing (s, false);
}
bool DrawTerrainPropertyLine (Session& s)
{
    return BeginDrawing (s, true);
}

void TerrainSetupPanel (Session& s)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape || !ImGui::CollapsingHeader ("Topography preview", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::TextWrapped ("Path-driven terrain preview. Draw or select an authored path, then capture one native Mesh. "
                        "No native terrain is written.");
    const auto paths = TerrainDesignPaths (s);
    ResolveTerrainDesignPath (s);
    ImGui::BeginDisabled (!Idle (s));
    if (paths.empty ())
        ImGui::TextWrapped ("No authored design paths yet. Draw one in the view to begin.");
    else {
        const auto available = std::find (paths.begin (), paths.end (), s.terrainPath) != paths.end ();
        const auto label = available ? "Path " + std::to_string (s.terrainPath) : "Select an authored path";
        if (ImGui::BeginCombo ("Design path", label.c_str ())) {
            for (auto path : paths) {
                const auto name = "Path " + std::to_string (path);
                if (ImGui::Selectable (name.c_str (), s.terrainPath == path))
                    ChooseTerrainDesignPath (s, path);
            }
            ImGui::EndCombo ();
        }
    }
    if (ImGui::Button ("Draw design path"))
        DrawTerrainDesignPath (s);
    // Re-resolve after the combo: button readiness must use this frame's choice.
    const auto chosen = ResolveTerrainDesignPath (s);
    if (chosen) {
        ImGui::SameLine ();
        if (ImGui::Button ("Edit design path"))
            EditTerrainDesignPath (s);
    }
    if (!doc.landscape->propertyLineSourceId) {
        ImGui::TextWrapped ("A property line is needed for terrain capture, not for drawing or editing the path.");
        if (ImGui::Button ("Draw property line"))
            DrawTerrainPropertyLine (s);
    }
    if (chosen && doc.landscape->propertyLineSourceId) {
#if defined(TAPIOCA_GEOMETRY_RUST)
        ImGui::TextWrapped ("Select exactly one Archicad Mesh before locking the view, then capture it.");
        if (ImGui::Button ("Capture selected Mesh"))
            RequestTerrainCapture (s, true);
        if (!doc.landscape->sourceTerrainGuid.empty ()) {
            ImGui::SameLine ();
            if (ImGui::Button ("Recapture current terrain"))
                RequestTerrainCapture (s, false);
        }
#else
        ImGui::TextDisabled ("Terrain preview requires the Rust geometry build.");
#endif
    }
    ImGui::EndDisabled ();
    if (s.terrainCapture)
        ImGui::TextUnformatted ("Capturing topography...");
    if (!doc.landscape->sourceTerrainGuid.empty ())
        ImGui::TextWrapped ("Terrain Mesh: %s", doc.landscape->sourceTerrainGuid.c_str ());
}
} // namespace geomsrv::archviz::edithud
