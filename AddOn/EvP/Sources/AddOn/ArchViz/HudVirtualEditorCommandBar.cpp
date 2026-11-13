#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <imgui.h>
#include <algorithm>
#include <string>

// #191 area A, the command bar: pinned over the page. Row 1: the terrain build's state, Undo, Redo,
// the File menu and, apart at the right edge, Done. Row 2, always there so the panel does not jump:
// the running command, what it asks for and its Apply and Cancel; else the last message or the
// active tool's hint.
namespace geomsrv::archviz::edithud {
using namespace detail;
namespace lg = tapioca::geometry;

Chip ChipState (const TerrainDesignStatus& status, const std::string& refusal)
{
    // At 320 px row 1 leaves the chip about one word beside Undo, Redo, File and Done (#191 U0):
    // the refusal's code and sentence are row 2's while no command runs.
    if (!refusal.empty ())
        return { ChipKind::Refused, "Refused", refusal };
    if (!status.active)
        return {};
    if (status.pending || status.previewPending || status.buildRequested)
        return { ChipKind::Building, "Building..." };
    if (status.current && status.built && !status.stale && !status.previewed)
        return { ChipKind::Current, "Current r" + std::to_string (status.revision) };
    return { ChipKind::Stale, "Stale: mesh not built" };
}

void TrackBuildRefusal (Session& s, const TerrainDesignStatus& status)
{
    if (!status.error.empty ()) {
        s.buildRefusal = status.error;
        s.buildRefusalRevision = status.current ? status.revision : s.edits.Doc ().revision;
    }
    else if (!s.buildRefusal.empty () && status.current && status.built && status.revision > s.buildRefusalRevision)
        s.buildRefusal.clear (), s.buildRefusalRevision = 0;
}

std::optional<RunningCommand> detail::Running (const Session& s)
{
    if (s.landscapeCommand) {
        const auto kind = s.landscapeCommand->kind;
        return RunningCommand { kind == lg::OperationKind::Union        ? "Union"
                                : kind == lg::OperationKind::Difference ? "Subtract"
                                                                        : "Intersection",
                                LandscapeCommandPrompt (s), s.landscapeCommand->picks.size (), true };
    }
    if (s.booleanDraft)
        return RunningCommand { "Boolean",
                                s.booleanDraft->error.empty () ? "Enter applies the preview." : s.booleanDraft->error,
                                s.booleanDraft->operands.size (), s.booleanDraft->error.empty () };
    if (s.rectangularArray)
        return RunningCommand { "Array", "Set the counts and spacing below.", s.rectangularArray->refs.size (), true };
    if (s.tool == Tool::Mirror)
        return RunningCommand { "Mirror", "Click the mirror line's two points. Ctrl: the other way.", 0, false };
    if (s.tool == Tool::Trim)
        return RunningCommand { "Trim", "Hover a straight segment; click the highlighted span.", 0, false };
    return std::nullopt;
}

bool detail::ApplyRunning (Session& s)
{
    if (s.landscapeCommand)
        return AdvanceLandscapeCommand (s);
    if (s.booleanDraft)
        return ApplyBoolean (s);
    if (s.rectangularArray)
        return ApplyRectangularArray (s);
    return false;
}

bool detail::CancelRunning (Session& s)
{
    if (s.landscapeCommand)
        return CancelLandscapeCommand (s);
    if (s.booleanDraft)
        return CancelBoolean (s), true;
    if (s.rectangularArray)
        return CancelRectangularArray (s), true;
    if (s.tool == Tool::Mirror || s.tool == Tool::Trim) {
        SetTool (s, Tool::Select), s.landscapeMode = LandscapeMode::Select, s.drawing.clear ();
        return true;
    }
    return false;
}

namespace {
const char* ToolHint (Tool tool)
{
    return ShapeTool (tool) ? "Click a corner or centre, then the size. Shift: square, circle." : "";
}

// `text` on one line within `width`, cut with "..." when it does not fit; the whole on hover.
void Elided (const std::string& text, float width, bool disabled = false)
{
    std::string shown = text;
    if (ImGui::CalcTextSize (shown.c_str ()).x > width) {
        while (!shown.empty () && ImGui::CalcTextSize ((shown + "...").c_str ()).x > width)
            shown.pop_back ();
        shown += "...";
    }
    if (disabled)
        ImGui::TextDisabled ("%s", shown.c_str ());
    else
        ImGui::TextUnformatted (shown.c_str ());
    if (shown != text && ImGui::IsItemHovered ())
        hudshell::HoverTip ("%s", text.c_str ());
}

float ButtonWidth (const char* label)
{
    return ImGui::CalcTextSize (label, nullptr, true).x + 2.0f * ImGui::GetStyle ().FramePadding.x;
}

void BuildChip (Session& s)
{
    const auto status = GetTerrainDesignStatus (s);
    TrackBuildRefusal (s, status);
    const auto chip = ChipState (status, s.buildRefusal);
    if (chip.kind == ChipKind::None) {
        ImGui::TextDisabled ("No terrain");
        return;
    }
    // Refused reads red; Building amber; Stale and Current in the panel's own ink (#71).
    const bool tinted = chip.kind == ChipKind::Refused || chip.kind == ChipKind::Building;
    if (tinted)
        ImGui::PushStyleColor (ImGuiCol_Text, chip.kind == ChipKind::Refused ? IM_COL32 (220, 38, 38, 255)
                                                                             : IM_COL32 (245, 158, 11, 255));
    // What row 1 leaves it: Undo, Redo, File and Done keep their full labels.
    const float spacing = ImGui::GetStyle ().ItemSpacing.x;
    const float room = ImGui::GetContentRegionAvail ().x - ButtonWidth ("Undo") - ButtonWidth ("Redo") -
                       ButtonWidth ("File") - ButtonWidth ("Done") - 5.0f * spacing;
    std::string shown = chip.text;
    while (shown.size () > 4 && ButtonWidth ((shown + "...").c_str ()) > room)
        shown.pop_back ();
    if (shown != chip.text)
        shown += "...";
    ImGui::PushID ("build-chip");
    const bool pressed = ImGui::SmallButton (shown.c_str ());
    ImGui::PopID ();
    if (tinted)
        ImGui::PopStyleColor ();
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip ("%s%s", (chip.detail.empty () ? chip.text : chip.detail).c_str (),
                            chip.kind == ChipKind::Stale ? "\nClick to build the mesh now." : "");
    if (pressed && chip.kind == ChipKind::Stale)
        RequestTerrainBuild (s);
}

void FileMenu (Session& s)
{
    if (ImGui::SmallButton ("File"))
        ImGui::OpenPopup ("##file");
    if (!ImGui::BeginPopup ("##file"))
        return;
    DraftMenuItems (s);
    const bool landscape = s.edits.Doc ().landscape.has_value ();
    const bool free = !GizmoModeLocked (s) && s.drawing.empty ();
    ImGui::Separator ();
    // In memory only, for comparing two states within this session; Save draft is durable.
    if (ImGui::MenuItem ("Session snapshot", nullptr, false, landscape && free))
        lg::WriteLandscape (s.edits.Doc (), lg::LandscapeStorage::Virtual, s.landscapeSaved, s.message);
    if (ImGui::MenuItem ("Restore snapshot", nullptr, false, landscape && free && !s.landscapeSaved.empty ()))
        ReloadLandscape (s, s.landscapeSaved);
    ImGui::Separator ();
    const bool exportable = !(s.debugExportRequested || s.edits.InGesture () || !s.drawing.empty () ||
                              s.terrainCapture || s.draftingImport || GizmoModeLocked (s) || s.landscapeCommand);
    if (ImGui::MenuItem (s.debugExportRequested ? "Export requested..." : "Export debug scene", nullptr, false,
                         exportable))
        RequestDebugExport (s);
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        hudshell::HoverTip ("Save paths, fillet settings, captured topography and current design to a new JSON file "
                            "in %%LOCALAPPDATA%%\\Tapioca\\dumps. Export before and after a problematic edit.");
    ImGui::EndPopup ();
}

void PromptRow (Session& s)
{
    const float spacing = ImGui::GetStyle ().ItemSpacing.x;
    if (const auto command = Running (s)) {
        const char* apply = "Apply (Enter)";
        const char* cancel = "Cancel (Esc)";
        float buttons = ButtonWidth (cancel) + spacing;
        if (command->canApply)
            buttons += ButtonWidth (apply) + spacing;
        std::string text = std::string (command->name) + ": " + command->prompt;
        if (command->picked)
            text += "  " + std::to_string (command->picked) + " picked";
        Elided (text, ImGui::GetContentRegionAvail ().x - buttons);
        if (command->canApply) {
            ImGui::SameLine ();
            if (ImGui::SmallButton (apply))
                ApplyRunning (s);
        }
        ImGui::SameLine ();
        if (ImGui::SmallButton (cancel))
            CancelRunning (s);
        return;
    }
    if (!s.buildRefusal.empty ()) { // #188: the refusal stands over the last message until a newer build
        ImGui::AlignTextToFramePadding ();
        ImGui::PushStyleColor (ImGuiCol_Text, IM_COL32 (220, 38, 38, 255));
        Elided (s.buildRefusal, ImGui::GetContentRegionAvail ().x);
        ImGui::PopStyleColor ();
        return;
    }
    const std::string text = !s.message.empty () ? s.message : ToolHint (s.tool);
    if (text.empty ())
        ImGui::Dummy ({ 1.0f, ImGui::GetFrameHeight () });
    else {
        ImGui::AlignTextToFramePadding ();
        Elided (text, ImGui::GetContentRegionAvail ().x, s.message.empty ());
    }
}
} // namespace

void detail::CommandBar (Session& s)
{
    BuildChip (s);
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Undo"))
        History (s, true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Redo"))
        History (s, false);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    FileMenu (s);
    // Done stands apart at the right edge; Delete is in the Inspector's action row and on its key.
    const float done = ButtonWidth ("Done");
    ImGui::SameLine ((std::max) (ImGui::GetCursorPosX (), ImGui::GetWindowContentRegionMax ().x - done));
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    if (ImGui::SmallButton ("Done"))
        RequestClose (s);
    ImGui::EndDisabled ();
    PromptRow (s);
}
} // namespace geomsrv::archviz::edithud
