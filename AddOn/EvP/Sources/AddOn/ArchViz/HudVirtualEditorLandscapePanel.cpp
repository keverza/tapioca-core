#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudShellTabs.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <iterator>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;
namespace {
void ReferenceLinesPanel (Session& s)
{
    if (!ImGui::CollapsingHeader ("Reference line display"))
        return;
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty () || s.edits.InGesture ());
    const auto sources = s.edits.Doc ().landscape->sources;
    for (const auto& source : sources) {
        if (source.deleted || source.role != lg::SourceRole::ReferenceLine || source.reference)
            continue;
        ImGui::PushID (std::to_string (source.id).c_str ());
        ImGui::Text ("Line %llu", static_cast<unsigned long long> (source.entityId));
        auto style = source.lineStyle;
        bool changed = ImGui::Checkbox ("Visible", &style.visible);
        ImGui::SameLine ();
        changed |= ImGui::Checkbox ("Locked", &style.locked);
        changed |= ImGui::InputDouble ("Width (pixels)", &style.widthPixels, 0.5, 1, "%.1f");
        float colour[4];
        for (int i = 0; i < 4; ++i)
            colour[i] = float ((style.rgba >> (24 - i * 8)) & 255) / 255;
        if (ImGui::ColorEdit4 ("Line colour", colour)) {
            style.rgba = 0;
            for (int i = 0; i < 4; ++i)
                style.rgba |= uint32_t (colour[i] * 255 + 0.5f) << (24 - i * 8);
            changed = true;
        }
        if (changed && !s.edits.Apply ([id = source.id, style] (edit::Document& doc, std::string& error) {
                return lg::SetReferenceLineStyle (doc, id, style, error);
            }))
            s.message = s.edits.Message ();
        ImGui::PopID ();
    }
    ImGui::EndDisabled ();
}
} // namespace
void LandscapePanel (Session& s)
{
    if (!s.edits.Doc ().landscape)
        return;
    Landscape (s);
    TerrainDesignPanel (s);
    ImGui::Separator ();
    ImGui::TextUnformatted ("Landscape. Sources remain editable.");
    const char* modes[] = { "Select sources", "Draw source" };
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    for (int i = 0; i < 2; ++i) {
        if (i)
            ImGui::SameLine ();
        if (ImGui::RadioButton (modes[i], int (s.landscapeMode) == i))
            SetLandscapeMode (s, LandscapeMode (i));
    }
    ImGui::EndDisabled ();
    EntrancesPanel (s);
    ImGui::Text ("%zu faces   %zu regions   XY preview at %.2f m", s.landscape.faces.size (),
                 s.landscape.regions.size (), s.plane.origin.z);
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    if (ImGui::SmallButton ("Save virtual definition"))
        lg::WriteLandscape (s.edits.Doc (), lg::LandscapeStorage::Virtual, s.landscapeSaved, s.message);
    ImGui::SameLine ();
    ImGui::BeginDisabled (s.landscapeSaved.empty ());
    if (ImGui::SmallButton ("Reload"))
        ReloadLandscape (s, s.landscapeSaved);
    ImGui::EndDisabled ();
    ImGui::EndDisabled ();
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    if (ImGui::Button ("Union"))
        BeginLandscapeCommand (s, lg::OperationKind::Union);
    ImGui::SameLine ();
    if (ImGui::Button ("Subtract"))
        BeginLandscapeCommand (s, lg::OperationKind::Difference);
    ImGui::SameLine ();
    if (ImGui::Button ("Intersection"))
        BeginLandscapeCommand (s, lg::OperationKind::Intersection);
    ImGui::EndDisabled ();
    if (s.landscapeCommand) {
        const auto kind = s.landscapeCommand->kind;
        ImGui::TextUnformatted (kind == lg::OperationKind::Union        ? "Union"
                                : kind == lg::OperationKind::Difference ? "Subtract"
                                                                        : "Intersection");
    }
    ImGui::TextWrapped ("%s", LandscapeCommandPrompt (s));
    if (s.landscapeCommand) {
        ImGui::Text ("%zu faces selected", s.selection.Items ().size ());
        if (ImGui::SmallButton ("Cancel (Esc)"))
            CancelLandscapeCommand (s);
    }
    ImGui::BeginDisabled (s.landscapeCommand.has_value ());
    if (hudshell::BeginSubTabs ("##landscape")) {
        if (ImGui::BeginTabItem ("Sources")) {
            const char* roles[] = { "Reference line", "Region boundary", "Cutter", "Property line" };
            const lg::SourceRole values[] = { lg::SourceRole::ReferenceLine, lg::SourceRole::RegionBoundary,
                                              lg::SourceRole::Cutter, lg::SourceRole::PropertyLine };
            ImGui::PushID ("draw-source-role");
            for (int i = 0; i < 4; ++i) {
                if (i % 2)
                    ImGui::SameLine ();
                if (ImGui::RadioButton (roles[i], s.landscapeRole == values[i]))
                    s.landscapeRole = values[i];
            }
            ImGui::PopID ();
            ImGui::TextWrapped (
                "Select source geometry directly in the view. Start a command above to pick its faces.");
            // The selected lines' role: any line can be a region's boundary, a cutter, a material
            // boundary, or -- one closed line -- the property line. A path is always a centreline.
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
            ImGui::PushID ("selected-source-role");
            for (size_t i = 0; i < std::size (uses); ++i) {
                if (i % 2)
                    ImGui::SameLine ();
                if (ImGui::RadioButton (uses[i].label, shared == uses[i].role))
                    SetSelectedSourceRole (s, uses[i].role);
            }
            ImGui::PopID ();
            ImGui::EndDisabled ();
            if (shared == lg::SourceRole::ReferenceLine)
                ImGui::TextWrapped (
                    "Reference line: snappable linework only. No fill, material, quantities or terrain effect. "
                    "Use Reference line display below to change line style, visibility and lock.");
            else if (shared == lg::SourceRole::RegionBoundary)
                ImGui::TextWrapped ("Region boundary: closed footprint and hardscape object. "
                                    "Surface metadata and terrain grading are not enabled yet.");
            ImGui::TextDisabled ("A region boundary must be a closed editor-owned line.");
            ReferenceLinesPanel (s);
            ImGui::EndTabItem ();
        }
        if (ImGui::BeginTabItem ("Operations")) {
            const auto operations = s.edits.Doc ().landscape->operations;
            if (operations.empty ())
                ImGui::TextDisabled ("No authored operations.");
            for (const auto& operation : operations) {
                if (IsLandscapeFaceSet (operation))
                    continue;
                ImGui::PushID (std::to_string (operation.id).c_str ());
                bool enabled = operation.enabled;
                if (ImGui::Checkbox ("Enabled", &enabled)) {
                    const auto id = operation.id;
                    if (!s.edits.Apply ([id, enabled] (edit::Document& doc, std::string& error) {
                            return lg::SetLandscapeOperationEnabled (doc, id, enabled, error);
                        }))
                        s.message = s.edits.Message ();
                }
                ImGui::SameLine ();
                ImGui::Text ("%llu   kind %u   %zu inputs", static_cast<unsigned long long> (operation.id),
                             unsigned (operation.kind), operation.inputs.size ());
                ImGui::PopID ();
            }
            ImGui::EndTabItem ();
        }
        if (ImGui::BeginTabItem ("Materials")) {
            ImGui::TextDisabled ("Material authoring follows in H3/H13. No terrain or native output yet.");
            ImGui::EndTabItem ();
        }
        if (ImGui::BeginTabItem ("Warnings")) {
            Landscape (s);
            if (!s.edits.Doc ().landscape->propertyLineSourceId)
                ImGui::TextWrapped ("No property line. Terrain-affecting operations require one.");
            if (s.landscape.diagnostics.empty () && s.landscape.planarDiagnostics.empty () &&
                s.landscape.error.empty ())
                ImGui::TextDisabled ("No evaluation warnings.");
            for (const auto& warning : s.landscape.diagnostics)
                ImGui::TextWrapped ("%s: %s", lg::LandscapeDiagnosticCodeName (warning.code), warning.message.c_str ());
            for (const auto& warning : s.landscape.planarDiagnostics)
                ImGui::TextWrapped ("%s: %s", lg::PlanarDiagnosticCodeName (warning.code), warning.message.c_str ());
            if (!s.landscape.error.empty ())
                ImGui::TextWrapped ("%s", s.landscape.error.c_str ());
            ImGui::EndTabItem ();
        }
        hudshell::EndSubTabs ();
    }
    ImGui::EndDisabled ();
}
} // namespace geomsrv::archviz::edithud
