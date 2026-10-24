#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditPathEdit.hpp"
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
    if (s.terrainSectionDraft && s.terrainSectionDraft->path != path)
        s.terrainSectionDraft.reset ();
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

bool BeginTerrainSectionEdit (Session& s, edit::Id section)
{
    if (!Ready (s))
        return false;
    const auto path = ResolveTerrainDesignPath (s);
    const auto* authored = edit::FindPath (s.edits.Doc (), path);
    if (authored)
        for (const auto& value : authored->sections)
            if (value.id == section) {
                s.terrainSectionDraft = std::make_shared<TerrainSectionDraft> (
                    TerrainSectionDraft { path, section, s.edits.Doc ().revision, value });
                s.message.clear ();
                return true;
            }
    s.message = "TerrainSectionMissing: the chosen path range no longer exists.";
    return false;
}

bool ApplyTerrainSectionEdit (Session& s)
{
    if (!Ready (s))
        return false;
    const auto draft = s.terrainSectionDraft;
    if (!draft || draft->revision != s.edits.Doc ().revision || draft->path != ResolveTerrainDesignPath (s)) {
        s.message = "TerrainSectionStale: reload section values before applying over a changed document.";
        return false;
    }
    const auto request = *draft; // The operation owns its typed values, never a mutable UI borrow.
    const bool changed = s.edits.Apply ([request] (edit::Document& doc, std::string& error) {
        return edit::EditPath (
            doc, request.path,
            [request] (edit::Path& path, std::string& error) {
                for (auto& section : path.sections)
                    if (section.id == request.section) {
                        if (request.value.id != section.id || request.value.station != section.station ||
                            (request.value.fall != edit::Fall::Left && request.value.fall != edit::Fall::Right &&
                             request.value.fall != edit::Fall::Crown)) {
                            error = "TerrainSectionIdentity: use path tools to change ranges, not dimensional fields.";
                            return false;
                        }
                        section = request.value;
                        return true;
                    }
                error = "TerrainSectionMissing: the edited range no longer exists.";
                return false;
            },
            error);
    });
    if (!changed) {
        s.message = s.edits.Message ().empty () ? "Cross-section unchanged." : s.edits.Message ();
        return false;
    }
    s.terrainSectionDraft = std::make_shared<TerrainSectionDraft> (
        TerrainSectionDraft { request.path, request.section, s.edits.Doc ().revision, request.value });
    s.publishedRevision = UINT64_MAX;
    s.message = "Cross-section applied. Terrain preview follows the document revision.";
    return true;
}

namespace {
void SectionPanel (Session& s, edit::Id path)
{
    const auto* authored = edit::FindPath (s.edits.Doc (), path);
    if (!authored || authored->sections.empty () || !ImGui::TreeNode ("Terrain cross-section"))
        return;
    const auto sections = authored->sections;
    if ((!s.terrainSectionDraft || s.terrainSectionDraft->path != path) && Idle (s))
        BeginTerrainSectionEdit (s, sections.front ().id);
    if (!s.terrainSectionDraft || s.terrainSectionDraft->path != path) {
        ImGui::TextDisabled ("Finish the current input to edit section values.");
        ImGui::TreePop ();
        return;
    }
    const auto range = "Range " + std::to_string (s.terrainSectionDraft->section);
    if (ImGui::BeginCombo ("Section range", range.c_str ())) {
        for (const auto& section : sections) {
            const auto label =
                "Range " + std::to_string (section.id) + " from " + std::to_string (section.station) + " m";
            if (ImGui::Selectable (label.c_str (), section.id == s.terrainSectionDraft->section))
                BeginTerrainSectionEdit (s, section.id);
        }
        ImGui::EndCombo ();
    }
    const auto draft = s.terrainSectionDraft;
    const bool stale = draft->revision != s.edits.Doc ().revision;
    if (stale)
        ImGui::TextWrapped (
            "Section values are stale. Reload before applying; newer document edits will not be overwritten.");
    ImGui::BeginDisabled (stale);
    auto& value = draft->value;
    // Sliders (the user, 2026-10-24); a release applies, so a drag is one undo step and the
    // preview rebuilds once. Ctrl+click types a value, outside the slider's range too.
    bool release = false;
    const auto slide = [&release] (const char* label, double& field, double lo, double hi, const char* format) {
        ImGui::SliderScalar (label, ImGuiDataType_Double, &field, &lo, &hi, format);
        release |= ImGui::IsItemDeactivatedAfterEdit ();
    };
    slide ("Left width", value.widthLeft, 0.0, 10.0, "%.2f m");
    slide ("Right width", value.widthRight, 0.0, 10.0, "%.2f m");
    slide ("Crossfall", value.crossfall, 0.0, 10.0, "%.1f %%");
    const char* fall = value.fall == edit::Fall::Left ? "Left" : value.fall == edit::Fall::Right ? "Right" : "Crown";
    if (ImGui::BeginCombo ("Falls to", fall)) {
        for (auto hand : { edit::Fall::Left, edit::Fall::Right, edit::Fall::Crown }) {
            const char* name = hand == edit::Fall::Left ? "Left" : hand == edit::Fall::Right ? "Right" : "Crown";
            if (ImGui::Selectable (name, value.fall == hand) && value.fall != hand) {
                value.fall = hand;
                release = true;
            }
        }
        ImGui::EndCombo ();
    }
    slide ("Shoulder", value.shoulder, 0.0, 5.0, "%.2f m");
    slide ("Cut slope", value.cutSlope, 5.0, 200.0, "%.0f %%");
    slide ("Fill slope", value.fillSlope, 5.0, 200.0, "%.0f %%");
    slide ("Rounding", value.rounding, 0.0, 10.0, "%.2f m");
    if (release)
        ApplyTerrainSectionEdit (s);
    ImGui::EndDisabled ();
    if (ImGui::Button ("Reload section values"))
        BeginTerrainSectionEdit (s, draft->section);
    ImGui::TextWrapped (
        "Each change applies when released, as one undo step. Profile elevations stay in the path profile.");
    ImGui::TreePop ();
}
} // namespace

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
    if (chosen)
        SectionPanel (s, chosen);
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
