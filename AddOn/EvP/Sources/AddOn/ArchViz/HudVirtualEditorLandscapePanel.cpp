#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/HudVirtualEditorRecipe.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <algorithm>
#include <iterator>
#include <imgui_internal.h>
#include <cfloat>
#include <string>
#include <functional>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;
namespace {
// selectedOnly: the selected reference line's style (Inspector); else every reference line.
void ReferenceLinesPanel (Session& s, bool selectedOnly)
{
    if (!selectedOnly && !ImGui::CollapsingHeader ("Reference line display"))
        return;
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty () || s.edits.InGesture ());
    const auto sources = s.edits.Doc ().landscape->sources;
    for (const auto& source : sources) {
        if (source.deleted || source.role != lg::SourceRole::ReferenceLine || source.reference)
            continue;
        if (selectedOnly && std::none_of (s.selection.Items ().begin (), s.selection.Items ().end (),
                                          [&] (const edit::Ref& ref) { return ref.entity == source.entityId; }))
            continue;
        ImGui::PushID (std::to_string (source.id).c_str ());
        ImGui::Text ("Line %llu", static_cast<unsigned long long> (source.entityId));
        auto style = source.lineStyle;
        bool changed = ImGui::Checkbox ("Visible", &style.visible);
        ImGui::SameLine ();
        changed |= ImGui::Checkbox ("Locked", &style.locked);
        changed |= ImGui::InputDouble ("Width (pixels)", &style.widthPixels, 0.5, 1, "%.1f");
        changed |= detail::SwatchPicker ("line colour", style.rgba); // one undo step per colour chosen
        ImGui::SameLine ();
        ImGui::TextUnformatted ("Line colour");
        if (changed && !s.edits.Apply ([id = source.id, style] (edit::Document& doc, std::string& error) {
                return lg::SetReferenceLineStyle (doc, id, style, error);
            }))
            s.message = s.edits.Message ();
        ImGui::PopID ();
    }
    ImGui::EndDisabled ();
}
} // namespace
// #191: the Outliner's two landscape views, as the Operations and Warnings tabs drew them.
void detail::OperationsTab (Session& s)
{
    RecipePanel (s); // #105: the compiled terrain recipe, read-only
    auto operations = s.edits.Doc ().landscape->operations;
    // The authored footprint steps in storage order. Their checkbox is a footprint bypass
    // (a disabled step passes its first input through), not the object's terrain effect.
    if (!ImGui::CollapsingHeader ("Footprint steps (authored order)"))
        operations.clear ();
    else if (operations.empty ())
        ImGui::TextDisabled ("No authored operations.");
    for (const auto& operation : operations) {
        if (IsLandscapeFaceSet (operation))
            continue;
        ImGui::PushID (std::to_string (operation.id).c_str ());
        bool enabled = operation.enabled;
        if (ImGui::Checkbox ("Footprint on", &enabled)) {
            const auto id = operation.id;
            if (!s.edits.Apply ([id, enabled] (edit::Document& doc, std::string& error) {
                    return lg::SetLandscapeOperationEnabled (doc, id, enabled, error);
                }))
                s.message = s.edits.Message ();
        }
        if (ImGui::IsItemHovered ())
            geomsrv::archviz::hudshell::HoverTip (
                "Off bypasses this step: its first input passes through. The owner's "
                "terrain effect is unchanged; set Effect to None to remove it from the "
                "terrain recipe.");
        ImGui::SameLine ();
        ImGui::Text ("%s %llu   %zu inputs", lg::RecipeOperationName (operation.kind),
                     static_cast<unsigned long long> (operation.id), operation.inputs.size ());
        ImGui::PopID ();
    }
}
void detail::WarningsTab (Session& s)
{
    Landscape (s);
    if (!s.edits.Doc ().landscape->propertyLineSourceId)
        ImGui::TextWrapped ("No property line: the captured terrain boundary limits region/path grading.");
    if (s.landscape.diagnostics.empty () && s.landscape.planarDiagnostics.empty () && s.landscape.error.empty ())
        ImGui::TextDisabled ("No evaluation warnings.");
    for (const auto& warning : s.landscape.diagnostics)
        ImGui::TextWrapped ("%s: %s", lg::LandscapeDiagnosticCodeName (warning.code), warning.message.c_str ());
    for (const auto& warning : s.landscape.planarDiagnostics)
        ImGui::TextWrapped ("%s: %s", lg::PlanarDiagnosticCodeName (warning.code), warning.message.c_str ());
    if (!s.landscape.error.empty ())
        ImGui::TextWrapped ("%s", s.landscape.error.c_str ());
}
namespace {
// #191: a role is picked from a grid of toggle buttons, three to a row across the panel's width;
// a two-word role reads on two lines, each centred. The one in force is drawn pressed. Returns the index pressed,
// or -1.
int RoleGrid (const char* id, const char* const* labels, size_t count, const std::function<bool (size_t)>& active)
{
    int pressed = -1;
    ImGui::PushID (id);
    const float height = 2.0f * ImGui::GetTextLineHeight () + 2.0f * ImGui::GetStyle ().FramePadding.y;
    if (ImGui::BeginTable ("##roles", 3, ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i = 0; i < count; ++i) {
            ImGui::TableNextColumn ();
            std::string label = labels[i];
            if (const auto space = label.find (' '); space != std::string::npos)
                label[space] = '\n';
            const bool on = active (i);
            if (on) {
                ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
                ImGui::PushStyleColor (ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
            }
            // The frame is the button's; each line of its label is centred on its own (ImGui centres a
            // multi-line label as one block, its lines left-aligned).
            ImGui::PushID (int (i));
            if (ImGui::Button ("##role", ImVec2 (-FLT_MIN, height)))
                pressed = int (i);
            const ImVec2 lo = ImGui::GetItemRectMin (), hi = ImGui::GetItemRectMax ();
            const auto lines = 1 + std::count (label.begin (), label.end (), '\n');
            const float line = ImGui::GetTextLineHeight ();
            float y = (lo.y + hi.y - line * float (lines)) * 0.5f;
            for (size_t start = 0; start <= label.size ();) {
                const auto end = std::min (label.find ('\n', start), label.size ());
                ImGui::RenderTextClipped (ImVec2 (lo.x, y), ImVec2 (hi.x, y + line), label.c_str () + start,
                                          label.c_str () + end, nullptr, ImVec2 (0.5f, 0.0f));
                y += line;
                start = end + 1;
            }
            ImGui::PopID ();
            if (on)
                ImGui::PopStyleColor (2);
        }
        ImGui::EndTable ();
    }
    ImGui::PopID ();
    return pressed;
}
// The role the next drawn line takes (Inspector context: draw tool).
void NextLineRole (Session& s)
{
    const char* roles[] = { "Reference line", "Region boundary", "Cutter", "Property line" };
    const lg::SourceRole values[] = { lg::SourceRole::ReferenceLine, lg::SourceRole::RegionBoundary,
                                      lg::SourceRole::Cutter, lg::SourceRole::PropertyLine };
    ImGui::TextUnformatted ("Next line's role:");
    const int pressed = RoleGrid ("draw-source-role", roles, std::size (roles),
                                  [&] (size_t i) { return s.landscapeRole == values[i]; });
    if (pressed >= 0)
        s.landscapeRole = values[pressed];
}
// The selected lines' role: any line can be a region's boundary, a cutter, a material
// boundary, or -- one closed line -- the property line. A path is always a centreline.
void SelectedLinesRole (Session& s)
{
    const auto shared = SelectedSourceRole (s);
    ImGui::BeginDisabled (s.selection.Items ().empty ());
    const struct {
        const char* label;
        lg::SourceRole role;
    } uses[] = { { "Reference line", lg::SourceRole::ReferenceLine },
                 { "Region boundary", lg::SourceRole::RegionBoundary },
                 { "Cutter", lg::SourceRole::Cutter },
                 { "Material boundary", lg::SourceRole::MaterialBoundary },
                 { "Property line", lg::SourceRole::PropertyLine },
                 { "Building outline", lg::SourceRole::Building } };
    // A reference is Archicad's line: shown, used, but changed only in Archicad.
    size_t references = 0;
    for (const auto& source : s.edits.Doc ().landscape->sources)
        references += !source.deleted && source.reference;
    if (references)
        ImGui::TextDisabled ("%zu line%s referenced from Archicad: locked here, edit in Archicad.", references,
                             references == 1 ? "" : "s");
    ImGui::TextUnformatted ("Use selected lines as:");
    const char* labels[std::size (uses)];
    for (size_t i = 0; i < std::size (uses); ++i)
        labels[i] = uses[i].label;
    const int pressed =
        RoleGrid ("selected-source-role", labels, std::size (uses), [&] (size_t i) { return shared == uses[i].role; });
    if (pressed >= 0)
        SetSelectedSourceRole (s, uses[pressed].role);
    ImGui::EndDisabled ();
    if (shared == lg::SourceRole::ReferenceLine)
        ImGui::TextWrapped (
            "Reference line: snappable linework only. No fill, material, quantities or terrain effect.");
}
// The landscape Boolean the selected lines feed, by name (#189 adds its construction inputs).
void BooleanOfSelection (Session& s)
{
    const auto& landscape = *s.edits.Doc ().landscape;
    for (const auto& object : landscape.objects)
        for (const auto& op : landscape.operations)
            if (op.id == object.footprintOperation &&
                (op.kind == lg::OperationKind::Union || op.kind == lg::OperationKind::Difference ||
                 op.kind == lg::OperationKind::Intersection || op.kind == lg::OperationKind::Xor)) {
                ImGui::Text ("%s %llu   %zu inputs", lg::RecipeOperationName (op.kind),
                             static_cast<unsigned long long> (op.id), op.inputs.size ());
            }
}
bool SelectionIsBuilding (const Session& s)
{
    return SelectedSourceRole (s) == lg::SourceRole::Building;
}
} // namespace

// #191 U4: what the landscape adds to the Inspector's context. Each context draws only its own:
// the next line's role for a drawing tool, the selected lines' role and the region's settings
// for a region, one reference line's style for a reference line, the terrain with nothing selected.
void LandscapePanel (Session& s)
{
    if (!s.edits.Doc ().landscape)
        return;
    Landscape (s);
    const auto context = ResolveContext (s);
    ImGui::BeginDisabled (s.landscapeCommand.has_value ());
    switch (context) {
        case InspectorContext::DrawTool:
            NextLineRole (s);
            if (s.landscapeMode == LandscapeMode::Entrance)
                EntrancesPanel (s);
            break;
        case InspectorContext::Path:
            EntrancesPanel (s);
            if (const auto* path = ProfilePath (s); path && path->id == ResolveTerrainDesignPath (s))
                TerrainSectionPanel (s, path->id);
            break;
        case InspectorContext::Region:
            SelectedLinesRole (s);
            ImGui::TextDisabled ("A region boundary must be a closed editor-owned line.");
            RegionPanel (s);
            ImGui::SeparatorText ("Material");
            RegionPanel (s, true);
            if (SelectionIsBuilding (s))
                EntrancesPanel (s);
            break;
        case InspectorContext::BooleanResult:
            BooleanOfSelection (s);
            SelectedLinesRole (s);
            break;
        case InspectorContext::ReferenceLine:
            SelectedLinesRole (s);
            ReferenceLinesPanel (s, true);
            break;
        case InspectorContext::FixedHeights:
            ImGui::SetNextItemOpen (true, ImGuiCond_Appearing);
            FixedHeightsPanel (s);
            break;
        case InspectorContext::Entity:
        case InspectorContext::Several:
            SelectedLinesRole (s);
            break;
        case InspectorContext::Nothing:
            TerrainDesignPanel (s);
            ImGui::Separator ();
            EntrancesPanel (s);
            ImGui::Text ("%zu faces   %zu regions   XY preview at %.2f m", s.landscape.faces.size (),
                         s.landscape.regions.size (), s.plane.origin.z);
            // #191 U2: Session snapshot and Restore snapshot are in the command bar's File menu.
            DraftControls (s);
            ReferenceLinesPanel (s, false);
            break;
        default:
            break;
    }
    ImGui::EndDisabled ();
}
} // namespace geomsrv::archviz::edithud
