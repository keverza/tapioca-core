// Fixed terrain heights: the panel that fixes guide vertices' heights in the terrain, and #109's
// grading of the selected paths to those control points. Split from HudVirtualEditorTerrain.cpp,
// which keeps the terrain capture and design (#224).
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditDisplayUnits.hpp"
#include "Edit/EditGradeToControls.hpp"
#include "Edit/EditLandscape.hpp"
#include <imgui.h>
#include <algorithm>
#include <set>

namespace geomsrv::archviz::edithud {
bool SetTerrainControlSelection (Session& s, bool enabled)
{
    if (!s.edits.Doc ().landscape || s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty ()) {
        s.message = "Finish the current input before changing terrain controls.";
        return false;
    }
    const auto refs = s.selection.Items ();
    if (refs.empty ()) {
        s.message = "Select guide vertices or whole open polylines for fixed terrain heights.";
        return false;
    }
    const bool changed = s.edits.Apply ([refs, enabled] (edit::Document& doc, std::string& error) {
        bool changed = false;
        std::set<std::pair<edit::Id, edit::Id>> controls;
        for (const auto& ref : refs) {
            if ((ref.part != edit::Part::Vertex && ref.part != edit::Part::Entity) || !edit::Exists (doc, ref)) {
                error = "TerrainControl: select guide vertices or whole open polylines.";
                return false;
            }
            edit::Id source = 0;
            for (const auto& candidate : doc.landscape->sources)
                if (!candidate.deleted && candidate.entityId == ref.entity) {
                    if (source) {
                        error = "TerrainControl: the selected geometry has multiple source identities.";
                        return false;
                    }
                    source = candidate.id;
                }
            if (!source) {
                error = "TerrainControl: the selection has no live landscape guide source.";
                return false;
            }
            controls.emplace (source, ref.part == edit::Part::Vertex ? ref.item : 0);
        }
        for (const auto& [source, vertex] : controls) {
            if (tapioca::geometry::SetLandscapeTerrainControl (doc, source, vertex, enabled, error))
                changed = true;
            else if (!error.empty ())
                return false;
        }
        return changed;
    });
    s.message = changed ? "Fixed terrain heights updated; guide geometry remains editable." : s.edits.Message ();
    if (changed)
        s.publishedRevision = UINT64_MAX;
    return changed;
}

bool GradeSelectedPathsToControls (Session& s)
{
    std::vector<edit::Id> paths;
    for (const auto& r : s.selection.Items ())
        if (edit::FindPath (s.edits.Doc (), r.entity) != nullptr &&
            std::find (paths.begin (), paths.end (), r.entity) == paths.end ())
            paths.push_back (r.entity);
    if (paths.empty ()) {
        s.message = "Select the paths to grade first.";
        return false;
    }
    tapioca::geometry::GradeToControlsReport report;
    const double reach = s.gradeReach;
    std::string refused;
    const bool changed = s.edits.Apply ([paths, reach, &report, &refused] (edit::Document& doc, std::string& error) {
        // Nothing in reach is no edit: false with no reason, so no undo step is recorded.
        const bool ok = tapioca::geometry::GradePathsToControls (doc, paths, reach, report, error);
        refused = ok ? std::string () : error;
        return ok && !report.graded.empty ();
    });
    s.message = refused.empty () ? report.Summary () : refused;
    if (changed)
        s.publishedRevision = UINT64_MAX;
    return changed;
}

namespace {
void TerrainControlsPanel (Session& s)
{
    if (!s.edits.Doc ().landscape || !ImGui::CollapsingHeader ("Fixed terrain heights"))
        return;
    ImGui::BeginDisabled (s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty ());
    ImGui::TextWrapped ("Select guide vertices or an open straight polyline. Its authored Z is fixed in the terrain; "
                        "this is not an object movement lock. Controls must be inside the rebuild region.");
    if (ImGui::Button ("Fix selected heights"))
        SetTerrainControlSelection (s, true);
    detail::SameLineIfRoom (detail::ButtonWidth ("Release selected heights"));
    if (ImGui::Button ("Release selected heights"))
        SetTerrainControlSelection (s, false);
    const auto controls = s.edits.Doc ().landscape->terrainControls;
    for (size_t i = 0; i < controls.size (); ++i) {
        const auto control = controls[i];
        ImGui::PushID (static_cast<int> (i));
        bool enabled = control.enabled;
        if (ImGui::Checkbox ("##enabled", &enabled)) {
            const bool changed = s.edits.Apply ([control, enabled] (edit::Document& doc, std::string& error) {
                return tapioca::geometry::SetLandscapeTerrainControl (doc, control.source, control.vertex, enabled,
                                                                      error);
            });
            s.message = s.edits.Message ();
            if (changed)
                s.publishedRevision = UINT64_MAX;
        }
        ImGui::SameLine ();
        ImGui::Text ("Source %llu / %s %llu", static_cast<unsigned long long> (control.source),
                     control.vertex ? "vertex" : "line", static_cast<unsigned long long> (control.vertex));
        ImGui::PopID ();
    }
    // #109: the selected paths take the heights of the control points within reach of them.
    ImGui::SeparatorText ("Grade paths to control points");
    {
        const auto units = edit::CurrentDisplayUnits ();
        double reach = edit::MetresToLength (units, s.gradeReach);
        const std::string format = std::string ("%.2f ") + edit::LengthSymbol (units);
        const double low = edit::MetresToLength (units, 0.05), high = edit::MetresToLength (units, 50.0);
        ImGui::SetNextItemWidth (120.0f);
        if (ImGui::DragScalar ("Reach", ImGuiDataType_Double, &reach, float (edit::MetresToLength (units, 0.05)), &low,
                               &high, format.c_str (), ImGuiSliderFlags_AlwaysClamp))
            s.gradeReach = edit::LengthToMetres (units, reach);
        if (ImGui::IsItemHovered ())
            hudshell::HoverTip ("How far from a path's centreline, in plan, a control point grades it");
        detail::SameLineIfRoom (detail::ButtonWidth ("Grade selected paths"));
        if (ImGui::Button ("Grade selected paths"))
            GradeSelectedPathsToControls (s);
        if (ImGui::IsItemHovered ())
            hudshell::HoverTip ("Each fixed height within reach sets the path's elevation where it projects onto it; "
                                "the grade runs straight between. Path ends with none in reach keep their height.");
    }
    ImGui::EndDisabled ();
}
} // namespace

void FixedHeightsPanel (Session& s)
{
    TerrainControlsPanel (s);
}
} // namespace geomsrv::archviz::edithud
