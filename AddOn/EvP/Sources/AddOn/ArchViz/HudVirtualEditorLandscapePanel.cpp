#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeSerialization.hpp"

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;
namespace {
void SetChoice (const char* label, edit::Id& chosen, const std::vector<lg::LandscapeOperation>& sets)
{
    const std::string preview = chosen ? "Set " + std::to_string (chosen) : "Choose a face set";
    if (!ImGui::BeginCombo (label, preview.c_str ()))
        return;
    for (const auto& set : sets) {
        const std::string name =
            "Set " + std::to_string (set.id) + " (" + std::to_string (set.faces.size ()) + " faces)";
        if (ImGui::Selectable (name.c_str (), chosen == set.id))
            chosen = set.id;
    }
    ImGui::EndCombo ();
}
} // namespace

void LandscapePanel (Session& s)
{
    if (!s.edits.Doc ().landscape)
        return;
    Landscape (s);
    ImGui::Separator ();
    ImGui::TextUnformatted ("Landscape. Sources remain editable.");
    const char* modes[] = { "Select sources", "Draw source", "Pick faces" };
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty ());
    for (int i = 0; i < 3; ++i) {
        if (i)
            ImGui::SameLine ();
        if (ImGui::RadioButton (modes[i], int (s.landscapeMode) == i))
            SetLandscapeMode (s, LandscapeMode (i));
    }
    ImGui::EndDisabled ();
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
    if (ImGui::BeginTabBar ("##landscape")) {
        if (ImGui::BeginTabItem ("Sets")) {
            if (ImGui::Button ("New face set"))
                CreateLandscapeFaceSet (s);
            // These are authored semantic RegionSelect operations, not a second
            // selection store. Copy rows because an action can replace the document.
            std::vector<lg::LandscapeOperation> sets;
            for (const auto& op : s.edits.Doc ().landscape->operations)
                if (IsLandscapeFaceSet (op))
                    sets.push_back (op);
            if (sets.empty ())
                ImGui::TextWrapped ("Create a set, pick faces in the view, then add the selected faces to it.");
            SetChoice ("Set A (active)", s.landscapeSet, sets);
            ImGui::BeginDisabled (!s.landscapeSet);
            if (ImGui::SmallButton ("Show set selection"))
                SelectLandscapeFaceSet (s);
            if (ImGui::Button ("Add selected faces to A"))
                EditLandscapeFaceSet (s);
            ImGui::SameLine ();
            if (ImGui::Button ("Remove selected faces from A"))
                EditLandscapeFaceSet (s, true);
            ImGui::EndDisabled ();
            ImGui::TextDisabled ("Click / Shift-click faces; picking never runs a Boolean.");
            ImGui::Separator ();
            SetChoice ("Set B (operand)", s.landscapeOperandSet, sets);
            const auto objects = s.edits.Doc ().landscape->objects;
            std::string resultLabel = "New result";
            for (const auto& object : objects)
                if (object.id == s.landscapeObject)
                    resultLabel = object.name + " " + std::to_string (object.id);
            if (ImGui::BeginCombo ("Output", resultLabel.c_str ())) {
                if (ImGui::Selectable ("New result", !s.landscapeObject))
                    s.landscapeObject = edit::kNoId;
                for (const auto& object : objects) {
                    const auto label = object.name + " " + std::to_string (object.id);
                    if (ImGui::Selectable (label.c_str (), object.id == s.landscapeObject))
                        s.landscapeObject = object.id;
                }
                ImGui::EndCombo ();
            }
            ImGui::BeginDisabled (!s.landscapeSet || !s.landscapeOperandSet || s.landscapeSet == s.landscapeOperandSet);
            if (ImGui::Button ("Add A + B"))
                BooleanLandscapeFaceSets (s, lg::OperationKind::Union);
            ImGui::SameLine ();
            if (ImGui::Button ("Subtract A - B"))
                BooleanLandscapeFaceSets (s, lg::OperationKind::Difference);
            ImGui::SameLine ();
            if (ImGui::Button ("Intersect A & B"))
                BooleanLandscapeFaceSets (s, lg::OperationKind::Intersection);
            ImGui::EndDisabled ();
            ImGui::TextWrapped (
                "Operations use the saved face sets, not the current picks. Sets and source curves are retained.");
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
            ImGui::TextWrapped ("Select source geometry directly in the view. Boolean operations live in Sets, not on "
                                "individual lines.");
            edit::Id sourceId = edit::kNoId;
            if (s.selection.Items ().size () == 1)
                for (const auto& source : s.edits.Doc ().landscape->sources)
                    if (!source.deleted && source.entityId && source.entityId == s.selection.Items ()[0].entity)
                        sourceId = source.id;
            ImGui::BeginDisabled (!sourceId);
            if (ImGui::Button ("Use selected source as property line")) {
                if (!s.edits.Apply ([sourceId] (edit::Document& doc, std::string& error) {
                        return lg::SetLandscapePropertyLine (doc, sourceId, error);
                    }))
                    s.message = s.edits.Message ();
            }
            ImGui::EndDisabled ();
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
        ImGui::EndTabBar ();
    }
    ImGui::EndDisabled ();
}
} // namespace geomsrv::archviz::edithud
