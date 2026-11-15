#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/EditLayers.hpp"
#include "Edit/EditPrimitives.hpp"
#include <imgui.h>
#include <algorithm>
#include <iterator>
#include <set>

// #191 area C, the Inspector: the page's top, under the pinned header. It shows one context at a
// time -- what the active command, tool or selection needs -- and never the others' controls.
namespace geomsrv::archviz::edithud {
using namespace detail;
namespace lg = tapioca::geometry;

namespace {
bool BooleanKind (lg::OperationKind kind)
{
    return kind == lg::OperationKind::Union || kind == lg::OperationKind::Difference ||
           kind == lg::OperationKind::Intersection || kind == lg::OperationKind::Xor;
}
const lg::LandscapeSource* SourceOf (const edit::Document& doc, edit::Id entity)
{
    for (const auto& source : doc.landscape->sources)
        if (!source.deleted && source.entityId == entity)
            return &source;
    return nullptr;
}
// What a line is when a Boolean takes it whole: its region's footprint, or the line itself (#160).
edit::Id BooleanInputOf (const edit::Document& doc, const lg::LandscapeSource& source)
{
    for (const auto& object : doc.landscape->objects)
        if (object.boundarySource == source.id && object.footprintOperation)
            return object.footprintOperation;
    return source.id;
}
// The lines a Boolean folded in, through the earlier Booleans of the same chain (A op B op C).
void BooleanLeaves (const lg::LandscapeSession& landscape, edit::Id operation, std::set<edit::Id>& leaves,
                    int depth = 0)
{
    for (const auto& op : landscape.operations)
        if (op.id == operation && BooleanKind (op.kind) && depth < 64) {
            for (const auto input : op.inputs) {
                leaves.insert (input);
                BooleanLeaves (landscape, input, leaves, depth + 1);
            }
            return;
        }
}
} // namespace

InspectorContext ResolveContext (const Session& s)
{
    // The File menu's Open draft... lists the drafts here, with the recovery offers (#159).
    if (s.closePrompt || !s.recoveries.empty () || s.draftFilesListed)
        return InspectorContext::Prompt;
    if (s.landscapeCommand || s.booleanDraft || s.rectangularArray || s.tool == Tool::Mirror || s.tool == Tool::Trim)
        return InspectorContext::Command;
    if (s.tool != Tool::Select || !s.drawing.empty () || s.landscapeMode == LandscapeMode::DrawSource ||
        s.landscapeMode == LandscapeMode::Entrance)
        return InspectorContext::DrawTool;
    const auto& doc = s.edits.Doc ();
    std::vector<edit::Id> objects;
    bool vertices = true;
    for (const auto& ref : s.selection.Items ()) {
        if (ref.entity == edit::kNoId)
            continue;
        vertices = vertices && ref.part == edit::Part::Vertex;
        if (std::find (objects.begin (), objects.end (), ref.entity) == objects.end ())
            objects.push_back (ref.entity);
    }
    if (objects.empty ())
        return s.selection.Items ().empty () ? InspectorContext::Nothing : InspectorContext::Several;
    std::vector<const lg::LandscapeSource*> sources;
    if (doc.landscape)
        for (const auto id : objects)
            if (const auto* source = SourceOf (doc, id))
                sources.push_back (source);
    const bool allSources = sources.size () == objects.size ();
    // Vertices of terrain lines are what Fix height acts on, whatever line they belong to.
    const bool fixedHeights =
        vertices && allSources && std::none_of (sources.begin (), sources.end (), [] (const auto* source) {
            return source->role == lg::SourceRole::ReferenceLine || source->role == lg::SourceRole::PathCenterline;
        });
    if (objects.size () == 1 && edit::FindPath (doc, objects.front ()))
        return InspectorContext::Path;
    if (objects.size () == 1 && !fixedHeights && allSources) {
        const auto role = sources.front ()->role;
        const auto* entity = edit::Find (doc, objects.front ());
        if (entity && entity->closed && role != lg::SourceRole::ReferenceLine && role != lg::SourceRole::PathCenterline)
            return InspectorContext::Region;
    }
    if (objects.size () > 1 && allSources && !fixedHeights)
        for (const auto& op : doc.landscape->operations) {
            if (!BooleanKind (op.kind))
                continue;
            std::set<edit::Id> leaves;
            BooleanLeaves (*doc.landscape, op.id, leaves);
            if (std::all_of (sources.begin (), sources.end (),
                             [&] (const auto* source) { return leaves.count (BooleanInputOf (doc, *source)) > 0; }))
                return InspectorContext::BooleanResult;
        }
    if (objects.size () == 1 && allSources && sources.front ()->role == lg::SourceRole::ReferenceLine)
        return InspectorContext::ReferenceLine;
    if (objects.size () == 1 && !fixedHeights)
        return InspectorContext::Entity;
    if (fixedHeights)
        return InspectorContext::FixedHeights;
    return InspectorContext::Several;
}

const char* InspectorContextName (InspectorContext context)
{
    static const char* const names[] = { "Prompt",        "Command",         "Draw tool",       "Path",
                                         "Region",        "Boolean result",  "Reference line",  "Object",
                                         "Fixed heights", "Several objects", "Nothing selected" };
    const auto i = size_t (context);
    return i < std::size (names) ? names[i] : "";
}

namespace {
// What is selected, measured: the tracker's values (type a number over the view to set one).
void TrackerValues (Session& s)
{
    if (s.tracker.State () == edit::TrackerState::Hidden || s.tracker.Items ().empty ())
        return;
    ImGui::Separator ();
    for (size_t i = 0; i < s.tracker.Items ().size (); ++i) {
        const auto& item = s.tracker.Items ()[i];
        const bool typing = s.tracker.State () == edit::TrackerState::Editing && i == s.tracker.Focus ();
        const std::string value =
            typing ? s.tracker.Text () + "_"
                   : editlayers::FormatValue (item.quantity, item.locked ? item.lockedValue : item.value);
        ImGui::Text ("%s  %s%s", edit::QuantityName (item.quantity), value.c_str (), item.locked ? "  (locked)" : "");
        if (i + 1 < s.tracker.Items ().size ())
            ImGui::SameLine (0.0f, 18.0f);
    }
}

// Archicad's own drafting brought in as editor lines: what is selected in Archicad now.
void AddFromArchicad (Session& s)
{
    ImGui::BeginDisabled (s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s));
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
}

// The shared tail under every selection context: the layer and group row, the action row -- the right-click menu's
// table, an action with nothing to do here greyed as the menu greys it -- with an option shown
// only beside its action, then More and Expert.
void SelectionTail (Session& s)
{
    const auto& doc = Doc (s);
    ImGui::Separator ();
    LayerAndGroupRow (s);
    ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
    // #191 U2: Delete heads the row, away from Exit in the command bar; the Delete key is unchanged.
    if (ImGui::SmallButton ("Delete"))
        DeleteSelection (s);
    bool offset = false;
    // The row wraps: a button that would run past the panel's edge starts the next line.
    const float right = ImGui::GetItemRectMin ().x + ImGui::GetContentRegionAvail ().x;
    for (const edit::Action* a : edit::Applicable (Actions (s), s.selection, edit::ViewKind::Plan)) {
        const auto op = a->make (doc, s.selection.Items ());
        const float width = ImGui::CalcTextSize (a->label.c_str ()).x + 2.0f * ImGui::GetStyle ().FramePadding.x;
        if (ImGui::GetItemRectMax ().x + ImGui::GetStyle ().ItemSpacing.x + width <= right)
            ImGui::SameLine ();
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
    if (ImGui::CollapsingHeader ("More")) {
        DuplicateOffsetControls (s);
        RectangularArrayControls (s);
    }
    ImGui::EndDisabled ();
    if (ImGui::CollapsingHeader ("Expert"))
        for (const auto& ref : s.selection.Items ())
            ImGui::TextDisabled ("object %llu   part %d   item %llu", static_cast<unsigned long long> (ref.entity),
                                 int (ref.part), static_cast<unsigned long long> (ref.item));
}

// Several objects: how many of each kind.
void SelectionCounts (Session& s)
{
    const auto& doc = Doc (s);
    std::set<edit::Id> paths, lines, other;
    for (const auto& ref : s.selection.Items ())
        if (edit::FindPath (doc, ref.entity))
            paths.insert (ref.entity);
        else if (const auto* entity = edit::Find (doc, ref.entity))
            (entity->ellipse ? other : lines).insert (ref.entity);
    const auto count = [] (size_t n, const char* one, const char* many) {
        return std::to_string (n) + " " + (n == 1 ? one : many);
    };
    ImGui::Text ("%s   %s   %zu other", count (paths.size (), "path", "paths").c_str (),
                 count (lines.size (), "line", "lines").c_str (), other.size ());
}

const char* ToolName (Tool tool)
{
    static const char* const names[] = { "Select",  "Polyline", "Path",   "Curve", "Rectangle",
                                         "Polygon", "Ellipse",  "Mirror", "Trim" };
    const auto i = size_t (tool);
    return i < std::size (names) ? names[i] : "";
}

// What the running command is called, as its toolbar entry names it.
const char* CommandTitle (const Session& s)
{
    if (s.landscapeCommand)
        switch (s.landscapeCommand->kind) {
            case lg::OperationKind::Union:
                return "Union";
            case lg::OperationKind::Difference:
                return "Subtract";
            case lg::OperationKind::Intersection:
                return "Intersection";
            default:
                return "Boolean";
        }
    if (s.booleanDraft)
        return "Boolean";
    if (s.rectangularArray)
        return "Rectangular array";
    return ToolName (s.tool);
}

std::string Title (const Session& s, InspectorContext context)
{
    std::string title = InspectorContextName (context);
    const auto& items = s.selection.Items ();
    if (context >= InspectorContext::Path && context <= InspectorContext::Entity && !items.empty () &&
        context != InspectorContext::BooleanResult)
        title += " " + std::to_string (items.front ().entity);
    else if (context == InspectorContext::DrawTool && s.tool != Tool::Select)
        title += ": " + std::string (ToolName (s.tool));
    else if (context == InspectorContext::Command)
        title += ": " + std::string (CommandTitle (s));
    return title;
}
} // namespace

// #191 U4: one context at a time (ResolveContext), each drawn by the functions that drew its
// controls before; a selection context ends with the shared action row, More and Expert.
void detail::Inspector (Session& s)
{
    const auto context = ResolveContext (s);
    if (Doc (s).landscape)
        Landscape (s); // evaluated every frame the page is drawn, as the landscape block did
    ImGui::TextDisabled ("%s", Title (s, context).c_str ());
    // #191 U2: Export debug scene is in the command bar's File menu; what it wrote is said here.
    if (!s.debugExportPath.empty ())
        ImGui::TextWrapped ("Debug scene saved: %s", s.debugExportPath.c_str ());
    if (!s.debugExportError.empty ())
        ImGui::TextWrapped ("Debug export failed: %s", s.debugExportError.c_str ());
    const bool toolLock = s.profileTyping || s.landscapeCommand.has_value () || GumballRepositioning (s);
    switch (context) {
        case InspectorContext::Prompt:
            ClosePrompt (s);
            DraftControls (s);
            return;
        case InspectorContext::Command:
            if (s.tool == Tool::Mirror) {
                ImGui::BeginDisabled (toolLock);
                ImGui::Checkbox ("Keep original", &s.mirrorKeeps);
                ImGui::EndDisabled ();
            }
            // The whole prompt, which the command bar's row may cut short, and what is picked so far.
            if (s.landscapeCommand) {
                ImGui::TextWrapped ("%s", LandscapeCommandPrompt (s));
                ImGui::TextDisabled ("%zu picked. Enter applies, Esc cancels.", s.landscapeCommand->picks.size ());
            }
            // Only the draft that runs: an array's starter button is the selection's (More), not here.
            ImGui::BeginDisabled (s.drag != Drag::None || s.profileTyping);
            if (s.rectangularArray)
                RectangularArrayControls (s);
            if (s.booleanDraft)
                BooleanControls (s);
            ImGui::EndDisabled ();
            TrackerValues (s);
            return;
        case InspectorContext::DrawTool:
            if (s.tool == Tool::Polygon) {
                ImGui::BeginDisabled (toolLock);
                ImGui::SetNextItemWidth (120);
                ImGui::SliderInt ("Sides (Up/Down)", &s.polygonSides, 3, edit::kMaxPolygonSides);
                ImGui::EndDisabled ();
            }
            LandscapePanel (s);
            TrackerValues (s);
            return;
        case InspectorContext::Nothing:
            LandscapePanel (s);
            AddFromArchicad (s);
            return;
        case InspectorContext::Path:
            ProfileEditor (s);
            LandscapePanel (s);
            break;
        case InspectorContext::Entity:
            EllipseControls (s);
            LandscapePanel (s);
            break;
        case InspectorContext::Several:
            SelectionCounts (s);
            LandscapePanel (s);
            break;
        default: // Region, Boolean result, reference line, fixed heights: the landscape's own
            LandscapePanel (s);
            break;
    }
    TrackerValues (s);
    SelectionTail (s);
}
} // namespace geomsrv::archviz::edithud
