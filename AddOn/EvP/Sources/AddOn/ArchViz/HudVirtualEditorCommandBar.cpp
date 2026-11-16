#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorBake.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/TapiocaUI/Foundation/Icons.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <imgui.h>
#include <algorithm>
#include <initializer_list>
#include <cctype>
#include <string>

// #191 area A, the command bar: pinned over the page. Row 1: the terrain build's state, Undo, Redo,
// the File menu and, apart at the right edge, Exit. Row 2, always there so the panel does not jump:
// the running command, what it asks for and its Apply and Cancel; else the last message or the
// active tool's hint.
namespace geomsrv::archviz::edithud {
using namespace detail;
namespace lg = tapioca::geometry;
namespace ui = geomsrv::archviz::ui;

Chip ChipState (const TerrainDesignStatus& status, const std::string& refusal)
{
    // At 320 px row 1 leaves the chip about one word beside Undo, Redo, File and Exit (#191 U0):
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
        ImGui::AlignTextToFramePadding ();
        ImGui::TextDisabled ("No terrain");
        return;
    }
    // Refused reads red; Building amber; Stale and Current in the panel's own ink (#71).
    const bool tinted = chip.kind == ChipKind::Refused || chip.kind == ChipKind::Building;
    if (tinted)
        ImGui::PushStyleColor (ImGuiCol_Text, chip.kind == ChipKind::Refused ? IM_COL32 (220, 38, 38, 255)
                                                                             : IM_COL32 (245, 158, 11, 255));
    // What row 1 leaves it: Undo and Redo (glyphs a frame high), File and Exit keep theirs.
    const float spacing = ImGui::GetStyle ().ItemSpacing.x;
    const float room = ImGui::GetContentRegionAvail ().x - 2.0f * ImGui::GetFrameHeight () - ButtonWidth ("File") -
                       ButtonWidth ("Exit") - 5.0f * spacing;
    // #191 U5: the Issues view's count rides on the chip; a click on a refusal or a count opens it.
    const auto issues = IssueCount (s);
    std::string shown = issues ? chip.text + " !" + std::to_string (issues) : chip.text;
    const std::string full = shown;
    while (shown.size () > 4 && ButtonWidth ((shown + "...").c_str ()) > room)
        shown.pop_back ();
    if (shown != full)
        shown += "...";
    ImGui::PushID ("build-chip");
    const bool pressed = ImGui::Button (shown.c_str ());
    ImGui::PopID ();
    if (tinted)
        ImGui::PopStyleColor ();
    // #157: a refusal in words first; its code after, for a report.
    const std::string said = chip.kind == ChipKind::Refused && TerrainRefusalText (chip.detail) != chip.detail
                                 ? TerrainRefusalText (chip.detail) + "\n" + chip.detail
                                 : (chip.detail.empty () ? chip.text : chip.detail);
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip ("%s%s%s", said.c_str (),
                            chip.kind == ChipKind::Stale ? "\nClick to build the mesh now." : "",
                            issues ? "\nIssues are listed in the Outliner." : "");
    if (pressed && chip.kind == ChipKind::Stale)
        RequestTerrainBuild (s);
    else if (pressed && (chip.kind == ChipKind::Refused || issues))
        s.outlinerShow = 2;
}

void FileMenu (Session& s)
{
    if (ImGui::Button ("File"))
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
    // tapioca-dev #177: the terrain panel's Archicad Mesh section, wherever the Inspector is now.
    if (ImGui::MenuItem ("Create Archicad Mesh...", nullptr, false, landscape))
        RevealTerrainBake (s);
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        hudshell::HoverTip ("Opens the Archicad Mesh section of the terrain panel: choose the lines, see them on "
                            "the terrain, then create the Mesh.");
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
        // #157: in words, beside the one action that answers it. A stale capture is answered by
        // recapturing the bound terrain (which clears the refusal); any other by its Issues list.
        const bool stale = s.buildRefusal.rfind ("TerrainCaptureStale", 0) == 0;
        const char* action = stale ? "Recapture" : "Issues";
        ImGui::AlignTextToFramePadding ();
        ImGui::PushStyleColor (ImGuiCol_Text, IM_COL32 (220, 38, 38, 255));
        Elided (TerrainRefusalText (s.buildRefusal),
                ImGui::GetContentRegionAvail ().x - ButtonWidth (action) - spacing);
        ImGui::PopStyleColor ();
        if (ImGui::IsItemHovered ())
            hudshell::HoverTip ("%s", s.buildRefusal.c_str ());
        ImGui::SameLine ();
        ImGui::BeginDisabled (stale && TerrainGuid (s).empty ());
        if (ImGui::SmallButton (action)) {
            if (stale)
                RequestTerrainCapture (s, false);
            else
                s.outlinerShow = 2;
        }
        ImGui::EndDisabled ();
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
    if (ui::IconButton ("undo", ui::IconId::Undo))
        History (s, true);
    hudshell::Tip ("Undo (Ctrl+Z)", hudshell::TipSide::Above);
    ImGui::SameLine ();
    if (ui::IconButton ("redo", ui::IconId::Redo))
        History (s, false);
    hudshell::Tip ("Redo (Ctrl+Y)", hudshell::TipSide::Above);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    FileMenu (s);
    // Exit stands apart at the right edge; Delete is in the Inspector's action row and on its key.
    const float exitWidth = ButtonWidth ("Exit");
    ImGui::SameLine ((std::max) (ImGui::GetCursorPosX (), ImGui::GetWindowContentRegionMax ().x - exitWidth));
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    if (ImGui::Button ("Exit"))
        RequestClose (s);
    ImGui::EndDisabled ();
    PromptRow (s);
}

// #191: what the host's command line runs on the editor's page. Names are the toolbar's and the
// menu's words, with the short forms a CAD user types; each runs exactly what its button does.
bool RunCommandLine (Session& s, const std::string& typed, std::string& message)
{
    message.clear ();
    std::string text;
    for (const char c : typed)
        text += char (std::tolower (static_cast<unsigned char> (c)));
    const auto first = text.find_first_not_of (" \t"), last = text.find_last_not_of (" \t");
    if (first == std::string::npos)
        return false;
    text = text.substr (first, last - first + 1);
    const auto is = [&] (std::initializer_list<const char*> names) {
        return std::any_of (names.begin (), names.end (), [&] (const char* n) { return text == n; });
    };
    const bool locked = s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s);
    const bool editing = s.drag != Drag::None || s.profileTyping;
    const struct {
        Tool tool;
        std::initializer_list<const char*> names;
    } tools[] = { { Tool::Select, { "select", "sel" } },
                  { Tool::Polyline, { "polyline", "line", "pl" } },
                  { Tool::Path, { "path" } },
                  { Tool::Curve, { "curve", "spline" } },
                  { Tool::Rectangle, { "rectangle", "rect" } },
                  { Tool::Polygon, { "polygon", "poly" } },
                  { Tool::Ellipse, { "ellipse", "circle" } },
                  { Tool::Mirror, { "mirror" } },
                  { Tool::Trim, { "trim" } } };
    for (const auto& t : tools)
        if (is (t.names)) {
            if (locked)
                message = "Finish or cancel the running command first (Enter or Esc).";
            else
                SelectTool (s, t.tool);
            return true;
        }
    const struct {
        lg::OperationKind kind;
        std::initializer_list<const char*> names;
    } booleans[] = { { lg::OperationKind::Union, { "union", "merge" } },
                     { lg::OperationKind::Difference, { "subtract", "difference", "minus" } },
                     { lg::OperationKind::Intersection, { "intersection", "intersect" } } };
    for (const auto& b : booleans)
        if (is (b.names)) {
            if (!s.edits.Doc ().landscape)
                message = "Booleans of regions need a landscape.";
            else if (!BeginLandscapeCommand (s, b.kind))
                message = s.message.empty () ? "The Boolean could not start now." : s.message;
            return true;
        }
    if (is ({ "undo", "u" }) || is ({ "redo" })) {
        if (editing)
            message = "Finish the drag or the typed value first.";
        else
            History (s, text != "redo");
        return true;
    }
    if (is ({ "delete", "del", "erase" })) {
        if (editing || s.selection.Items ().empty ())
            message = s.selection.Items ().empty () ? "Select what to delete first." : "Finish the drag first.";
        else
            DeleteSelection (s);
        return true;
    }
    if (is ({ "build", "build mesh" })) {
        if (!RequestTerrainBuild (s))
            message = s.message.empty () ? "The terrain cannot be built now." : s.message;
        return true;
    }
    if (is ({ "save", "save draft" })) {
        if (!RequestDraftSave (s))
            message = s.message.empty () ? "The draft cannot be saved now." : s.message;
        return true;
    }
    if (is ({ "exit", "close", "done" })) {
        RequestClose (s);
        return true;
    }
    // An action the selection offers, by its id or its menu label: what its Inspector button does.
    for (const edit::Action* a : edit::Applicable (Actions (s), s.selection, edit::ViewKind::Plan)) {
        std::string label;
        for (const char c : a->label)
            label += char (std::tolower (static_cast<unsigned char> (c)));
        if (text != a->id && text != label)
            continue;
        const auto& doc = s.edits.Doc ();
        const auto op = a->make (doc, s.selection.Items ());
        if (editing)
            message = "Finish the drag first.";
        else if (!op)
            message = a->label + " has nothing to do with this selection.";
        else {
            const edit::Id since = doc.nextId;
            if (!s.edits.Apply (op))
                message = s.edits.Message ();
            SelectMade (s, since);
        }
        return true;
    }
    return false;
}
} // namespace geomsrv::archviz::edithud
