#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeSerialization.hpp"

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

void LandscapePanel (Session& s)
{
    if (!s.edits.Doc ().landscape)
        return;
    Landscape (s);
    ImGui::Separator ();
    ImGui::TextUnformatted ("Landscape. Sources remain editable.");
    const char* modes[] = { "Select sources", "Draw source", "Face", "Boolean" };
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    for (int i = 0; i < 4; ++i) {
        if (i)
            ImGui::SameLine ();
        if (ImGui::RadioButton (modes[i], int (s.landscapeMode) == i))
            SetLandscapeMode (s, LandscapeMode (i));
    }
    ImGui::EndDisabled ();
    ImGui::Text ("%zu faces   %zu regions   XY preview at %.2f m", s.landscape.faces.size (),
                 s.landscape.regions.size (), s.plane.origin.z);
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    if (ImGui::SmallButton ("Keep / Add faces"))
        KeepLandscapeFaces (s);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Remove faces"))
        KeepLandscapeFaces (s, true);
    if (ImGui::SmallButton ("Save virtual definition"))
        lg::WriteLandscape (s.edits.Doc (), lg::LandscapeStorage::Virtual, s.landscapeSaved, s.message);
    ImGui::SameLine ();
    ImGui::BeginDisabled (s.landscapeSaved.empty ());
    if (ImGui::SmallButton ("Reload"))
        ReloadLandscape (s, s.landscapeSaved);
    ImGui::EndDisabled ();
    if (ImGui::BeginTabBar ("##landscape")) {
        if (ImGui::BeginTabItem ("Object")) {
            // Copy rows: a button may publish a new document value.
            const auto objects = s.edits.Doc ().landscape->objects;
            if (objects.empty ())
                ImGui::TextDisabled ("Select a face, then Keep / Add faces.");
            for (const auto& object : objects) {
                ImGui::PushID (std::to_string (object.id).c_str ());
                if (ImGui::Selectable (object.name.c_str (), s.landscapeObject == object.id))
                    s.landscapeObject = object.id;
                ImGui::PopID ();
            }
            ImGui::EndTabItem ();
        }
        if (ImGui::BeginTabItem ("Sources")) {
            const char* roles[] = { "Guide", "Cutter", "Property line" };
            const lg::SourceRole values[] = { lg::SourceRole::Guide, lg::SourceRole::Cutter,
                                              lg::SourceRole::PropertyLine };
            for (int i = 0; i < 3; ++i) {
                if (i)
                    ImGui::SameLine ();
                if (ImGui::RadioButton (roles[i], s.landscapeRole == values[i]))
                    s.landscapeRole = values[i];
            }
            const auto sources = s.edits.Doc ().landscape->sources;
            for (const auto& source : sources) {
                if (source.deleted)
                    continue;
                ImGui::PushID (std::to_string (source.id).c_str ());
                const edit::Ref ref { source.entityId ? source.entityId : source.pathId, edit::Part::Entity, 0 };
                const std::string label =
                    (source.id == s.edits.Doc ().landscape->propertyLineSourceId ? "Property line " : "Source ") +
                    std::to_string (source.id);
                if (ImGui::Selectable (label.c_str (), s.selection.Contains (ref), 0, { 160, 0 })) {
                    SetLandscapeMode (s, LandscapeMode::Select);
                    s.selection.Set ({ ref });
                }
                ImGui::SameLine ();
                if (ImGui::SmallButton ("Mark property")) {
                    const auto id = source.id;
                    if (!s.edits.Apply ([id] (edit::Document& doc, std::string& error) {
                            return lg::SetLandscapePropertyLine (doc, id, error);
                        }))
                        s.message = s.edits.Message ();
                }
                for (const auto& action : { std::pair { "Add", lg::OperationKind::Union },
                                            std::pair { "Subtract", lg::OperationKind::Difference },
                                            std::pair { "Intersect", lg::OperationKind::Intersection } }) {
                    if (action.second != lg::OperationKind::Union)
                        ImGui::SameLine ();
                    if (ImGui::SmallButton (action.first))
                        BooleanLandscape (s, action.second, source.id);
                }
                ImGui::PopID ();
            }
            ImGui::EndTabItem ();
        }
        if (ImGui::BeginTabItem ("Operations")) {
            const auto operations = s.edits.Doc ().landscape->operations;
            if (operations.empty ())
                ImGui::TextDisabled ("No authored operations.");
            for (const auto& operation : operations) {
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
        ImGui::EndTabBar ();
    }
    ImGui::EndDisabled ();
}
} // namespace geomsrv::archviz::edithud
