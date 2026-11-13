#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/EditLayers.hpp"
#include "Edit/EditPrimitives.hpp"
#include <imgui.h>

// #191 area C, the Inspector: the page's top, under the pinned header. Until U4 picks one context,
// it holds every control the header and the Outliner do not, in the order the panel drew them.
namespace geomsrv::archviz::edithud {
using namespace detail;

void detail::Inspector (Session& s)
{
    const auto& doc = Doc (s);
    ClosePrompt (s);
    LandscapePanel (s);
    // #191 U2: Export debug scene is in the command bar's File menu; what it wrote is said here.
    if (!s.debugExportPath.empty ())
        ImGui::TextWrapped ("Debug scene saved: %s", s.debugExportPath.c_str ());
    if (!s.debugExportError.empty ())
        ImGui::TextWrapped ("Debug export failed: %s", s.debugExportError.c_str ());
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
    if (s.tool == Tool::Mirror) {
        ImGui::Checkbox ("Keep original", &s.mirrorKeeps);
    }
    if (s.tool == Tool::Polygon) {
        ImGui::SetNextItemWidth (120);
        ImGui::SliderInt ("Sides (Up/Down)", &s.polygonSides, 3, edit::kMaxPolygonSides);
    }
    // Archicad's own drafting brought in as editor lines: what is selected in Archicad now.
    ImGui::BeginDisabled (s.draftingImport || s.edits.InGesture () || !s.drawing.empty ());
    if (ImGui::SmallButton (s.draftingImport ? "Adding Archicad lines..." : "Add selected Archicad lines and fills"))
        s.draftingImport = true, s.draftingImportAs = "lines";
    // The same selection as a landscape's reference outlines, or its doors as entrances.
    if (s.edits.Doc ().landscape) {
        const struct {
            const char *label, *as;
        } refs[] = { { "As property line", "property" },
                     { "As building outline", "building" },
                     { "Doors as entrances", "entrances" } };
        for (size_t i = 0; i < std::size (refs); ++i) {
            if (i)
                ImGui::SameLine ();
            if (ImGui::SmallButton (refs[i].label))
                s.draftingImport = true, s.draftingImportAs = refs[i].as;
        }
    }
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        geomsrv::archviz::hudshell::HoverTip (
            "Lines, polylines, arcs, circles and fill outlines selected in Archicad, at the "
            "working plane's height. One undo step.");
    ImGui::EndDisabled ();
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    // Context actions for what is selected.
    // An action with nothing to do here is shown greyed, as the menu shows it.
    // #191 U2: Delete heads the row, away from Done in the command bar; the Delete key is unchanged.
    if (ImGui::SmallButton ("Delete"))
        DeleteSelection (s);
    bool offers = true, offset = false;
    for (const edit::Action* a : edit::Applicable (Actions (s), s.selection, edit::ViewKind::Plan)) {
        const auto op = a->make (doc, s.selection.Items ());
        if (offers)
            ImGui::SameLine ();
        offers = true;
        offset = offset || a->id == "offset";
        ImGui::BeginDisabled (!op);
        if (ImGui::SmallButton (a->label.c_str ())) {
            const edit::Id since = doc.nextId;
            if (!s.edits.Apply (op))
                s.message = s.edits.Message ();
            SelectMade (s, since);
        }
        ImGui::EndDisabled ();
    }
    // Its distance comes after the rows: changing it rebuilds the table they point into.
    if (offset) {
        float distance = float (s.actionDefaults.offsetDistance);
        ImGui::SetNextItemWidth (110.0f);
        if (ImGui::DragFloat ("Offset distance", &distance, 0.01f, -50.0f, 50.0f, "%.2f m"))
            s.actionDefaults.offsetDistance = double (distance);
        hudshell::Tip ("Positive: outside a closed outline or left of a line. Negative: inside or right.",
                       hudshell::TipSide::Above);
        OffsetJoinControl (s);
        OffsetMiterLimitControl (s);
    }
    DuplicateOffsetControls (s);
    RectangularArrayControls (s);
    BooleanControls (s);
    ImGui::EndDisabled ();
    EllipseControls (s);

    // What is selected, measured: the tracker's values (type a number over the view to set one).
    if (s.tracker.State () != edit::TrackerState::Hidden && !s.tracker.Items ().empty ()) {
        ImGui::Separator ();
        for (size_t i = 0; i < s.tracker.Items ().size (); ++i) {
            const auto& item = s.tracker.Items ()[i];
            const bool typing = s.tracker.State () == edit::TrackerState::Editing && i == s.tracker.Focus ();
            const std::string value =
                typing ? s.tracker.Text () + "_"
                       : editlayers::FormatValue (item.quantity, item.locked ? item.lockedValue : item.value);
            ImGui::Text ("%s  %s%s", edit::QuantityName (item.quantity), value.c_str (),
                         item.locked ? "  (locked)" : "");
            if (i + 1 < s.tracker.Items ().size ())
                ImGui::SameLine (0.0f, 18.0f);
        }
    }
    ProfileEditor (s);
}
} // namespace geomsrv::archviz::edithud
