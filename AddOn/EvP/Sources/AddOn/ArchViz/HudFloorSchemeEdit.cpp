#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::hudfloorscheme {
void EditorDeleter::operator() (Editor* editor) const
{
    delete editor;
}

namespace detail {
constexpr size_t kUndo = 200;

// ---- designs, undo ------------------------------------------------------------------------
std::vector<bp::Core> CoresOf (const std::vector<fs::Pins::Core>& pins)
{
    std::vector<bp::Core> cores;
    for (const auto& p : pins)
        cores.push_back ({ { p.centre.x, p.centre.y },
                           p.width > 0 ? p.width : bp::kStairWidth,
                           p.depth > 0 ? p.depth : bp::kStairDepth });
    return cores;
}
Snapshot Keep (const bp::Draft& draft)
{
    return { draft.designs, draft.cores };
}
// Records what `change` does to the drafts of `keys` as one undo step.
template <typename F> void Change (Editor& e, Context& c, const std::vector<std::string>& keys, F change)
{
    Step step;
    for (const auto& k : keys)
        step.before[k] = Keep (c.drafts[k]);
    change ();
    bool any = false;
    for (const auto& k : keys) {
        step.after[k] = Keep (c.drafts[k]);
        any = any || !(step.after[k] == step.before[k]);
    }
    if (!any)
        return;
    for (const auto& k : keys)
        c.drafts[k].changed = true;
    e.undo.push_back (std::move (step));
    if (e.undo.size () > kUndo)
        e.undo.erase (e.undo.begin ());
    e.redo.clear ();
}
// The shown floor's design becomes `next`: the floor's design, and the building's stairs when
// they changed (they are then the building's own, on every floor).
void Commit (Editor& e, Context& c, fe::Design next)
{
    const auto plan = c.plans.find (e.key);
    if (plan == c.plans.end ())
        return;
    Change (e, c, { e.key }, [&] {
        auto& draft = c.drafts[e.key];
        const auto* floor = bp::Displayed (plan->second, draft);
        if (!floor)
            return;
        if (!(next.pins.cores == e.input.design.pins.cores))
            draft.cores = CoresOf (next.pins.cores);
        next.pins.cores.clear ();
        draft.designs.floors[bp::DesignStory (plan->second, draft, *floor)] = std::move (next);
    });
}
void UndoRedo (Editor& e, Context& c, bool undo)
{
    auto& from = undo ? e.undo : e.redo;
    auto& to = undo ? e.redo : e.undo;
    if (from.empty ())
        return;
    auto step = std::move (from.back ());
    from.pop_back ();
    for (const auto& [k, snapshot] : undo ? step.before : step.after) {
        auto& draft = c.drafts[k];
        draft.designs = snapshot.designs;
        draft.cores = snapshot.cores;
        draft.changed = true;
    }
    to.push_back (std::move (step));
    e.selected = {};
}
// The other building whose floor at elevation `z` holds `p`.
struct Neighbour {
    std::string key;
    const bp::Floor* floor = nullptr;
};
Neighbour NeighbourAt (const Context& c, const std::string& key, double z, fs::Vec p)
{
    for (const auto& [name, plan] : c.plans) {
        if (name == key)
            continue;
        for (const auto& floor : plan.floors)
            if (std::abs (floor.z - z) <= bp::kElevationTolerance &&
                (Inside (bp::FloorRings (floor), p) ||
                 Inside (bp::InputFor (c.plans, c.drafts, name, floor, c.programme, {}, c.options).owned, p)))
                return { name, &floor };
    }
    return {};
}
fe::Design& DesignAt (Context& c, const std::string& key, const bp::Floor& floor)
{
    auto& draft = c.drafts[key];
    bp::Sync (c.plans.at (key), draft); // a neighbour's draft is known before it is edited
    return draft.designs.floors[bp::DesignStory (c.plans.at (key), draft, floor)];
}
// A party wall pushed `distance` out of the building (or pulled in): the strip changes hands.
void PushParty (Editor& e, Context& c, const std::array<fs::Vec, 2>& wall, double distance)
{
    const auto plan = c.plans.find (e.key);
    if (plan == c.plans.end () || std::abs (distance) < 0.05)
        return;
    const auto* floor = bp::Displayed (plan->second, c.drafts[e.key]);
    if (!floor)
        return;
    const fs::Vec n = Outward (wall), m { (wall[0].x + wall[1].x) / 2, (wall[0].y + wall[1].y) / 2 };
    const auto neighbour = NeighbourAt (c, e.key, floor->z, { m.x + n.x * 0.3, m.y + n.y * 0.3 });
    if (!neighbour.floor)
        return;
    const auto mine = e.input.owned;
    const auto theirs = bp::InputFor (c.plans, c.drafts, neighbour.key, *neighbour.floor, c.programme, {}, c.options);
    Change (e, c, { e.key, neighbour.key }, [&] {
        auto& here = DesignAt (c, e.key, *floor);
        auto& there = DesignAt (c, neighbour.key, *neighbour.floor);
        if (distance > 0)
            fe::Take (here, there, fe::PartyStrip (wall, distance, theirs.owned));
        else
            fe::Take (there, here, fe::PartyStrip (wall, distance, mine));
    });
}

// ---- small widgets ------------------------------------------------------------------------
void NextControl (float width)
{
    const float right = ImGui::GetCursorScreenPos ().x + ImGui::GetContentRegionAvail ().x;
    if (ImGui::GetItemRectMax ().x + ImGui::GetStyle ().ItemSpacing.x + width <= right)
        ImGui::SameLine ();
}
bool Toggle (const char* label, bool on, const char* tip)
{
    if (on)
        ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
    const bool pressed = ImGui::SmallButton (label);
    if (on)
        ImGui::PopStyleColor ();
    if (ImGui::IsItemHovered () && tip)
        ImGui::SetTooltip ("%s", tip);
    return pressed;
}
std::vector<double> RoomSteps (const floorprogramme::Programme& p)
{
    std::vector<double> out;
    for (const auto& t : p.types)
        out.push_back (t.rooms);
    std::sort (out.begin (), out.end ());
    out.erase (std::unique (out.begin (), out.end ()), out.end ());
    return out;
}
const char* AccessName (fs::Access a)
{
    switch (a) {
        case fs::Access::Centre:
            return "Centre corridor";
        case fs::Access::OneSide:
            return "Corridor on one side";
        case fs::Access::CoreOnly:
            return "Sections round stairs";
        case fs::Access::Rows:
            return "Sections in rows";
        default:
            return "Auto";
    }
}
// The editor of building `key` on `floor`: its floors asked of the planner when their inputs
// change, its input and scheme brought up to date; a new floor drops the selection and gesture.
Editor& Refresh (EditorPtr& editor, Context& c, const std::string& key, const bp::Floor& floor)
{
    if (!editor)
        editor.reset (new Editor);
    auto& e = *editor;
    const auto& plan = c.plans.at (key);
    const auto& draft = c.drafts[key];
    c.planner.Poll ();
    const auto* planned = bp::WantFloors (c.planner, c.plans, c.drafts, key, c.programme, floor.story, c.options);
    std::vector<fs::Pins::Core> stairs;
    if (const auto* lead = bp::LeadFloor (plan); draft.cores.empty () && lead)
        if (const auto* led = c.planner.Latest (bp::FloorId (key, lead->story) + "#auto"))
            for (const auto& core : led->scheme.cores)
                stairs.push_back ({ core.centre, core.width, core.depth });
    e.input = bp::InputFor (c.plans, c.drafts, key, floor, c.programme, stairs, c.options);
    if (e.key != key || e.story != floor.story) {
        e.key = key, e.story = floor.story;
        e.selected = e.menu = {};
        e.drag = Drag::None;
        e.have = false;
        e.revision = 0;
    }
    e.real = bp::FloorRings (floor);
    if (planned && planned->revision != e.revision) {
        e.scheme = planned->scheme;
        e.revision = planned->revision;
        e.have = true;
        e.fills = Fills (e.scheme);
        e.around.clear ();
        if (!e.input.party.empty ())
            e.around.push_back (Triangles (e.input.party, IM_COL32 (205, 207, 203, 255)));
        e.selected = Again (e.scheme, e.selected, e.selectedAt);
    }
    return e;
}
// The right-click menu: a flat's rooms, split, join, lock; a stair's size; the floor's own.
void Menu (Editor& e, Context& c, bp::Draft& draft, const bp::Plan& plan, const bp::Floor& floor, Asked& asked)
{
    if (!ImGui::BeginPopup ("##planMenu"))
        return;
    const auto& s = e.scheme;
    const auto& full = e.input.design;
    const int flat = e.menu.kind == Kind::Flat ? fe::FlatAt (s, e.menuAt) : -1;
    const int stair = e.menu.kind == Kind::Stair ? fe::CoreAt (s, e.menuAt) : -1;
    if (flat >= 0) {
        const auto& f = s.flats[flat];
        ImGui::TextDisabled ("%s | net %.1f m2", floorprogramme::Name (c.programme, f.type).c_str (), f.net);
        for (double r : RoomSteps (c.programme)) {
            char label[24];
            std::snprintf (label, sizeof (label), r == std::floor (r) ? "%.0f rooms" : "%.1f rooms", r);
            if (ImGui::RadioButton (label, std::abs (f.rooms - r) < 1e-6) && std::abs (f.rooms - r) > 1e-6) {
                Commit (e, c, fe::SetRooms (s, full, flat, r));
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::Separator ();
        const bool locked = fe::Locked (s, full, flat);
        ImGui::BeginDisabled (locked);
        if (ImGui::MenuItem ("Split here"))
            Commit (e, c, fe::SplitFlat (s, full, flat, e.menuAt));
        if (ImGui::MenuItem ("Join with its neighbour")) {
            Commit (e, c, fe::RemoveFlat (s, full, flat));
            e.selected = {};
        }
        ImGui::EndDisabled ();
        if (ImGui::MenuItem (locked ? "Unlock" : "Lock"))
            Commit (e, c, fe::SetLocked (s, full, flat, !locked));
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("A locked flat keeps its walls, rooms and place through every other edit.");
        std::string traits;
        if (f.corner)
            traits += "corner  ";
        if (f.through)
            traits += "dual aspect  ";
        if (f.cap)
            traits += "corridor end";
        if (!traits.empty ())
            ImGui::TextDisabled ("%s", traits.c_str ());
    }
    else if (stair >= 0) {
        ImGui::TextDisabled ("Stair %.1f x %.1f m", s.cores[stair].width, s.cores[stair].depth);
        // A size is the building's: its stairs become its own, on every floor.
        const auto resize = [&] (double width, double depth) {
            auto next = full;
            const fs::Vec at = s.cores[stair].centre;
            size_t near = next.pins.cores.size ();
            double best = 1e18;
            for (size_t i = 0; i < next.pins.cores.size (); ++i) {
                const double d = std::hypot (next.pins.cores[i].centre.x - at.x, next.pins.cores[i].centre.y - at.y);
                if (d < best)
                    best = d, near = i;
            }
            if (near == next.pins.cores.size ()) {
                next.pins.cores.push_back ({ at, 0, 0 });
                near = next.pins.cores.size () - 1;
            }
            next.pins.cores[near].width = width, next.pins.cores[near].depth = depth;
            Commit (e, c, std::move (next));
        };
        if (ImGui::MenuItem ("4.5 x 4.2 m: stair round a lift"))
            resize (bp::kStairWidth, bp::kStairDepth);
        if (ImGui::MenuItem ("9.0 x 2.5 m: straight stair"))
            resize (9.0, 2.5);
        if (ImGui::MenuItem ("Rotate"))
            resize (s.cores[stair].depth, s.cores[stair].width);
        ImGui::Separator ();
        if (ImGui::MenuItem ("Remove stair")) {
            Commit (e, c, fe::RemoveStair (s, full, stair));
            e.selected = {};
        }
    }
    else {
        int flats = int (s.flats.size ());
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Flats");
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (7 * ImGui::GetFontSize ());
        if (ImGui::InputInt ("##flats", &flats, 1, 1) && flats > 0 && flats != int (s.flats.size ()))
            Commit (e, c, fe::SetCount (s, full, flats));
        if (const int wing = fe::WingAt (s, e.menuAt); wing >= 0) {
            fs::Access now = fs::Access::Auto;
            for (const auto& a : full.pins.access)
                if (fe::WingAt (s, a.at) == wing)
                    now = a.access;
            ImGui::SetNextItemWidth (11 * ImGui::GetFontSize ());
            if (ImGui::BeginCombo ("Access", AccessName (now))) {
                for (auto a : { fs::Access::Auto, fs::Access::Centre, fs::Access::OneSide, fs::Access::CoreOnly,
                                fs::Access::Rows })
                    if (ImGui::Selectable (AccessName (a), a == now) && a != now)
                        Commit (e, c, fe::SetAccess (s, full, wing, a));
                ImGui::EndCombo ();
            }
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Draw a rectangle onto the floor"))
            e.tool = Tool::Draw;
        if (ImGui::MenuItem ("Cut a rectangle out"))
            e.tool = Tool::Cut;
        ImGui::Separator ();
        const bool unique = draft.designs.unique.contains (floor.story);
        if (ImGui::MenuItem (unique ? "Share with identical floors" : "Make this floor unique"))
            Change (e, c, { e.key }, [&] {
                if (unique) {
                    draft.designs.unique.erase (floor.story);
                    draft.designs.floors.erase (floor.story);
                    return;
                }
                const auto* shared = bp::FindDesign (plan, draft, floor);
                const auto copy = shared ? *shared : fe::Design {};
                draft.designs.unique.insert (floor.story);
                draft.designs.floors[floor.story] = copy;
            });
        if (ImGui::MenuItem ("Reset this floor"))
            Change (e, c, { e.key }, [&] { draft.designs.floors.erase (bp::DesignStory (plan, draft, floor)); });
        if (ImGui::MenuItem ("Export plan"))
            asked.exportPlan = true;
    }
    ImGui::EndPopup ();
}
} // namespace detail

using namespace detail;
Asked PlanView (EditorPtr& editor, bp::Planner& planner, const std::map<std::string, bp::Plan>& plans,
                std::map<std::string, bp::Draft>& drafts, const std::string& key,
                const floorprogramme::Programme& programme, const massingareas::Coefficients& coefficients, float scale)
{
    Asked asked;
    const auto plan = plans.find (key);
    if (plan == plans.end ())
        return asked;
    auto& draft = drafts[key];
    bp::Sync (plan->second, draft);
    const auto* floor = bp::Displayed (plan->second, draft);
    if (!floor) {
        ImGui::TextDisabled ("No complete current floor contour.");
        return asked;
    }
    fs::Options options;
    options.grossFactor = coefficients.grossFactor;
    Context c { planner, plans, drafts, programme, options };
    auto& e = Refresh (editor, c, key, *floor);

    // The tools, in one row where they fit.
    const auto& s = e.scheme;
    const bool focused = ImGui::IsWindowFocused (ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::BeginDisabled (e.undo.empty ());
    if (ImGui::SmallButton ("Undo") || (focused && ImGui::IsKeyChordPressed (ImGuiMod_Ctrl | ImGuiKey_Z)))
        UndoRedo (e, c, true);
    ImGui::EndDisabled ();
    NextControl (40 * scale);
    ImGui::BeginDisabled (e.redo.empty ());
    if (ImGui::SmallButton ("Redo") || (focused && ImGui::IsKeyChordPressed (ImGuiMod_Ctrl | ImGuiKey_Y)))
        UndoRedo (e, c, false);
    ImGui::EndDisabled ();
    NextControl (40 * scale);
    if (Toggle ("Draw", e.tool == Tool::Draw, "Drag a rectangle onto the floor, square to its wing, in 0.4 m steps."))
        e.tool = e.tool == Tool::Draw ? Tool::Select : Tool::Draw;
    NextControl (36 * scale);
    if (Toggle ("Cut", e.tool == Tool::Cut, "Drag a rectangle out of the floor."))
        e.tool = e.tool == Tool::Cut ? Tool::Select : Tool::Cut;
    NextControl (130 * scale);
    int flats = int (s.flats.size ());
    ImGui::BeginDisabled (!e.have);
    ImGui::SetNextItemWidth (90 * scale);
    if (ImGui::InputInt ("flats", &flats, 1, 1) && e.have && flats > 0 && flats != int (s.flats.size ()))
        Commit (e, c, fe::SetCount (s, e.input.design, flats));
    ImGui::EndDisabled ();
    // Save and Discard: every building the edits changed (a party wall moves two).
    std::vector<std::string> dirty;
    for (auto& [name, d] : drafts)
        if (plans.contains (name) && bp::Dirty (d))
            dirty.push_back (name);
    NextControl (100 * scale);
    ImGui::BeginDisabled (dirty.empty () || e.drag != Drag::None);
    if (ImGui::SmallButton (dirty.empty () ? "Save" : "Save *")) {
        bool refused = false;
        for (const auto& name : dirty) {
            auto edits = bp::Edits (plans.at (name), drafts[name]);
            refused = refused || edits.empty ();
            asked.edits.insert (asked.edits.end (), edits.begin (), edits.end ());
        }
        if (refused)
            ImGui::OpenPopup ("##notSaved");
        e.undo.clear (), e.redo.clear ();
    }
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip ("%s", dirty.empty () ? "Nothing to save: the plan is as the project keeps it."
                                                : "Write the stairs and floor plans to the slabs' metadata.");
    NextControl (60 * scale);
    if (ImGui::SmallButton ("Discard")) {
        for (const auto& name : dirty)
            bp::Reset (plans.at (name), drafts[name]);
        e.undo.clear (), e.redo.clear ();
        e.selected = {};
    }
    ImGui::EndDisabled ();
    if (ImGui::BeginPopup ("##notSaved")) {
        ImGui::TextUnformatted ("A building or its saved plan changed in the project; Discard reloads it.");
        ImGui::EndPopup ();
    }
    Palette (scale);
    Canvas (e, c, scale);
    Menu (e, c, draft, plan->second, *floor, asked);

    // One line on the floor; the problems in its tooltip.
    if (!e.have) {
        ImGui::TextDisabled ("Planning the floor...");
        return asked;
    }
    size_t errors = 0;
    double red = 0;
    for (const auto& d : s.diagnostics)
        errors += d.level == fs::Diagnostic::Error;
    for (const auto& u : s.unassigned)
        if (u.reason.find ("storage") == std::string::npos)
            red += std::abs (fs::Area (u.shape));
    const bool unique = draft.designs.unique.contains (floor->story);
    if (errors)
        ImGui::TextColored (ImVec4 (0.75f, 0.23f, 0.22f, 1), "Floor %d | %zu flats, %zu stairs | %zu problems",
                            floor->story, s.flats.size (), s.cores.size (), errors);
    else
        ImGui::TextDisabled ("Floor %d%s | %zu flats, %zu stairs | net %.0f of %.0f m2", floor->story,
                             unique ? " (unique)" : "", s.flats.size (), s.cores.size (), s.net, s.gross);
    if (ImGui::IsItemHovered ()) {
        ImGui::BeginTooltip ();
        ImGui::Text ("%s | net/gross %.2f | unassigned %.0f m2", s.typology.c_str (),
                     s.gross > 0 ? s.net / s.gross : 0.0, red);
        for (const auto& d : s.diagnostics)
            if (d.level == fs::Diagnostic::Error)
                ImGui::TextColored (ImVec4 (0.75f, 0.23f, 0.22f, 1), "%s", d.text.c_str ());
            else if (d.level == fs::Diagnostic::Warning && d.code.rfind ("pin.", 0) == 0)
                ImGui::TextDisabled ("%s", d.text.c_str ());
        ImGui::TextDisabled ("Drag a flat onto another to change places, off the floor to remove it. Right click "
                             "for rooms, split, join, lock, stair size, flat count and access.");
        ImGui::EndTooltip ();
    }
    return asked;
}

std::optional<Selection> Selected (const EditorPtr& editor)
{
    if (!editor || editor->selected.kind != Kind::Flat || editor->selected.index < 0 ||
        editor->selected.index >= int (editor->scheme.flats.size ()))
        return std::nullopt;
    return Selection { editor->key, editor->story, editor->scheme.flats[editor->selected.index].shape };
}
} // namespace geomsrv::archviz::hudfloorscheme
