#include "ArchViz/HudFloorSchemeEdit.hpp"
#include "ArchViz/FloorSchemeEdit.hpp"
#include "ArchViz/HudShell.hpp"
#include "imgui.h"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <optional>

namespace geomsrv::archviz::hudfloorscheme {
namespace fs = floorscheme;
namespace fe = floorscheme::edit;
namespace cp = Clipper2Lib;

enum class Tool : uint8_t { Select, AddStair, Split, Join, Draw, Cut, Access };
enum class Drag : uint8_t { None, Stair, Wall, End, Rect, Pan };

// World triangles of one filled shape, built once per scheme.
struct Fill {
    std::vector<fs::Vec> triangles;
    ImU32 colour = 0;
};
struct Editor {
    double z = 0;
    std::string owner, floorKey;
    fe::Session session;
    fs::Scheme scheme; // what the canvas shows: the latest result
    bool have = false;
    std::vector<Fill> fills;
    // The worker: one generation at a time; the newest design waiting replaces an older one.
    std::future<std::pair<uint64_t, fs::Scheme>> job;
    std::optional<fe::Design> waiting;
    fe::Design asked;
    uint64_t sent = 0, shown = 0;
    std::chrono::steady_clock::time_point started;
    double took = 0; // ms, the last generation
    // The view: world centre, pixels per metre.
    bool fitted = false;
    double cx = 0, cy = 0, k = 4;
    float height = 520;
    Tool tool = Tool::Select;
    int flat = -1, core = -1;
    // A gesture: the scheme it started on (indices stay valid), where, and what it holds.
    Drag drag = Drag::None;
    ImGuiContext* dragOwner = nullptr;
    fs::Scheme base;
    fs::Vec from, at;
    int dragCore = -1;
    std::optional<fe::Wall> wall;
    std::optional<fe::End> end;
};
void EditorDeleter::operator() (Editor* editor) const
{
    delete editor;
}

namespace {
constexpr double kFloorTolerance = 0.05; // m: floors at one elevation

std::vector<fs::Ring> FloorAt (const std::map<std::string, buildingplan::Plan>& plans, double z, std::string& key)
{
    std::vector<fs::Ring> rings;
    double area = 0;
    for (const auto& [name, plan] : plans)
        for (const auto& floor : plan.floors) {
            if (std::abs (floor.z - z) > kFloorTolerance)
                continue;
            for (const auto& chain : floor.outline) {
                fs::Ring ring;
                for (size_t i = 0; i < chain.Count (); ++i)
                    ring.push_back ({ chain.xy[i * 2], chain.xy[i * 2 + 1] });
                if (ring.size () >= 3) {
                    area += fs::Area (ring);
                    rings.push_back (std::move (ring));
                }
            }
        }
    char text[64];
    std::snprintf (text, sizeof (text), "%.2f:%zu:%.3f", z, rings.size (), area);
    key = text;
    return rings;
}
cp::PathD Path (const fs::Ring& ring)
{
    cp::PathD path;
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    return path;
}
Fill Triangles (const std::vector<fs::Ring>& rings, ImU32 colour)
{
    Fill fill;
    fill.colour = colour;
    cp::PathsD paths;
    for (const auto& r : rings)
        paths.push_back (Path (r));
    cp::PathsD tris;
    if (cp::Triangulate (paths, 4, tris, false) == cp::TriangulateResult::success)
        for (const auto& t : tris)
            for (const auto& p : t)
                fill.triangles.push_back ({ p.x, p.y });
    else
        for (const auto& r : rings) // a fan: right for the convex shapes that fail
            for (size_t i = 1; i + 1 < r.size (); ++i)
                fill.triangles.insert (fill.triangles.end (), { r[0], r[i], r[i + 1] });
    return fill;
}
void Rebuild (Editor& e)
{
    e.fills.clear ();
    const auto& s = e.scheme;
    e.fills.push_back (Triangles (s.outline, IM_COL32 (236, 238, 232, 255)));
    for (const auto& u : s.unassigned)
        e.fills.push_back (Triangles ({ u.shape }, u.reason.find ("storage") != std::string::npos
                                                       ? IM_COL32 (190, 190, 182, 255)
                                                       : IM_COL32 (229, 72, 77, 150)));
    for (const auto& f : s.flats) {
        ImU32 colour = hudshell::Packed (floorprogramme::Colour (f.rooms));
        if (f.manual)
            colour = (colour & 0x00FFFFFF) | 0x99000000;
        e.fills.push_back (Triangles ({ f.shape }, colour));
    }
    for (const auto& c : s.corridors)
        e.fills.push_back (Triangles ({ c.shape }, IM_COL32 (214, 196, 154, 255)));
    for (const auto& l : s.lobbies)
        e.fills.push_back (Triangles ({ l }, IM_COL32 (230, 218, 185, 255)));
    for (const auto& c : s.cores)
        e.fills.push_back (Triangles ({ c.shape }, IM_COL32 (109, 114, 118, 255)));
}
void Pump (Editor& e, const floorprogramme::Programme& programme)
{
    if (e.job.valid ()) {
        if (e.job.wait_for (std::chrono::seconds (0)) != std::future_status::ready)
            return;
        auto [id, scheme] = e.job.get ();
        e.took = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - e.started).count ();
        if (id > e.shown) {
            e.scheme = std::move (scheme);
            e.shown = id;
            e.have = true;
            Rebuild (e);
            // The typology the generator chose stays while editing: an edit never flips it.
            if (e.session.design.shallow == fs::Access::Auto)
                e.session.design.shallow = fe::Chosen (e.scheme, e.session.options);
        }
    }
    if (!e.waiting)
        return;
    const uint64_t id = ++e.sent;
    e.asked = *e.waiting;
    e.started = std::chrono::steady_clock::now ();
    e.job = std::async (std::launch::async, [floor = e.session.floor, programme, design = std::move (*e.waiting),
                                             options = e.session.options, id] {
        return std::pair { id, fe::Run (floor, programme, design, options) };
    });
    e.waiting.reset ();
}
void Ask (Editor& e, fe::Design design)
{
    if (e.waiting ? *e.waiting == design : e.asked == design && (e.job.valid () || e.have))
        return; // already asked
    e.waiting = std::move (design);
}
void Commit (Editor& e, fe::Design design)
{
    if (e.session.Commit (std::move (design)))
        Ask (e, e.session.design);
}
void Fit (Editor& e, float width)
{
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& ring : e.session.floor)
        for (const auto& p : ring)
            x0 = (std::min) (x0, p.x), y0 = (std::min) (y0, p.y), x1 = (std::max) (x1, p.x), y1 = (std::max) (y1, p.y);
    if (x0 > x1)
        return;
    e.cx = (x0 + x1) / 2, e.cy = (y0 + y1) / 2;
    e.k = (std::min) ((width - 40) / (std::max) (1.0, x1 - x0), (e.height - 40) / (std::max) (1.0, y1 - y0));
    e.fitted = true;
}
// Room counts the programme offers, in order: the steps of the room buttons.
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
            return "centre corridor";
        case fs::Access::OneSide:
            return "corridor on one side";
        case fs::Access::CoreOnly:
            return "sections round stairs";
        case fs::Access::Rows:
            return "sections in rows";
        default:
            return "auto";
    }
}
fs::Access NextAccess (fs::Access a)
{
    switch (a) {
        case fs::Access::Auto:
            return fs::Access::Centre;
        case fs::Access::Centre:
            return fs::Access::OneSide;
        case fs::Access::OneSide:
            return fs::Access::CoreOnly;
        case fs::Access::CoreOnly:
            return fs::Access::Rows;
        default:
            return fs::Access::Auto;
    }
}
bool ToolButton (const char* label, Tool tool, Tool& current, const char* tip)
{
    const bool on = current == tool;
    if (on)
        ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
    const bool pressed = ImGui::SmallButton (label);
    if (on)
        ImGui::PopStyleColor ();
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("%s", tip);
    if (pressed)
        current = tool;
    return pressed;
}
void SameLineIfRoom (float width)
{
    const float right = ImGui::GetCursorScreenPos ().x + ImGui::GetContentRegionAvail ().x;
    if (ImGui::GetItemRectMax ().x + ImGui::GetStyle ().ItemSpacing.x + width <= right)
        ImGui::SameLine ();
}

// The design a gesture in progress would make, from the scheme it started on.
std::optional<fe::Design> Gesture (const Editor& e)
{
    const auto& d = e.session.design;
    switch (e.drag) {
        case Drag::Stair:
            return fe::MoveStair (e.base, d, e.dragCore, e.at);
        case Drag::Wall:
            return e.wall ? std::optional (fe::MoveWall (e.base, d, *e.wall, e.at)) : std::nullopt;
        case Drag::End:
            return e.end ? std::optional (fe::MoveEnd (d, *e.end, e.at)) : std::nullopt;
        case Drag::Rect:
            return e.tool == Tool::Cut ? fe::CutRect (d, fe::Rectangle (e.base, e.from, e.at))
                                       : fe::AddRect (d, fe::Rectangle (e.base, e.from, e.at));
        default:
            return std::nullopt;
    }
}

void Canvas (Editor& e, const floorprogramme::Programme& programme, float scale)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos ();
    const ImVec2 size ((std::max) (120.0f, ImGui::GetContentRegionAvail ().x), e.height * scale);
    if (!e.fitted)
        Fit (e, size.x);
    ImGui::InvisibleButton ("##floorScheme", size,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner (ImGuiKey_MouseWheelY); // the wheel zooms the plan, not the panel
    const bool hovered = ImGui::IsItemHovered ();
    auto& io = ImGui::GetIO ();
    const auto P = [&] (fs::Vec p) {
        return ImVec2 (origin.x + size.x / 2 + float ((p.x - e.cx) * e.k),
                       origin.y + size.y / 2 - float ((p.y - e.cy) * e.k));
    };
    const auto W = [&] (ImVec2 q) {
        return fs::Vec { e.cx + (q.x - origin.x - size.x / 2) / e.k, e.cy - (q.y - origin.y - size.y / 2) / e.k };
    };
    const fs::Vec mouse = W (io.MousePos);
    const auto owner = ImGui::GetCurrentContext ();

    // View: wheel zooms about the pointer; right or middle drag pans.
    if (hovered && io.MouseWheel != 0) {
        const double k = std::clamp (e.k * std::pow (1.2, io.MouseWheel), 0.5, 200.0);
        e.cx = mouse.x - (mouse.x - e.cx) * e.k / k, e.cy = mouse.y - (mouse.y - e.cy) * e.k / k;
        e.k = k;
    }
    if (ImGui::IsItemActive () &&
        (ImGui::IsMouseDragging (ImGuiMouseButton_Right) || ImGui::IsMouseDragging (ImGuiMouseButton_Middle))) {
        e.cx -= io.MouseDelta.x / e.k, e.cy += io.MouseDelta.y / e.k;
    }

    // Gestures on the left button.
    const auto& s = e.scheme;
    const double grab = 8.0 / e.k; // 8 px in metres
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && e.drag == Drag::None) {
        e.from = e.at = mouse;
        e.base = s;
        e.dragOwner = owner;
        switch (e.tool) {
            case Tool::Select:
                if (const int core = fe::CoreAt (s, mouse); core >= 0)
                    e.drag = Drag::Stair, e.dragCore = core, e.core = core, e.flat = -1;
                else if (auto end = fe::EndAt (s, mouse, grab))
                    e.drag = Drag::End, e.end = end;
                else if (auto wall = fe::WallAt (s, mouse, grab * 0.75))
                    e.drag = Drag::Wall, e.wall = wall;
                else
                    e.flat = fe::FlatAt (s, mouse), e.core = -1;
                break;
            case Tool::AddStair:
                Commit (e, fe::AddStair (s, e.session.design, mouse));
                break;
            case Tool::Split:
                if (const int flat = fe::FlatAt (s, mouse); flat >= 0)
                    Commit (e, fe::SplitFlat (s, e.session.design, flat, mouse));
                break;
            case Tool::Join:
                if (const int flat = fe::FlatAt (s, mouse); flat >= 0)
                    Commit (e, fe::RemoveFlat (s, e.session.design, flat));
                break;
            case Tool::Draw:
            case Tool::Cut:
                e.drag = Drag::Rect;
                break;
            case Tool::Access:
                if (const int wing = fe::WingAt (s, mouse); wing >= 0) {
                    fs::Access now = fs::Access::Auto;
                    for (const auto& a : e.session.design.pins.access)
                        if (fe::WingAt (s, a.at) == wing)
                            now = a.access;
                    Commit (e, fe::SetAccess (s, e.session.design, wing, NextAccess (now)));
                }
                break;
        }
    }
    if (e.drag != Drag::None && e.dragOwner == owner) {
        e.at = mouse;
        if (ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            // Live: the newest gesture goes to the worker whenever it is free.
            if (e.drag != Drag::Rect)
                if (auto next = Gesture (e))
                    Ask (e, std::move (*next));
        }
        else {
            if (std::hypot (e.at.x - e.from.x, e.at.y - e.from.y) > 0.05)
                if (auto next = Gesture (e))
                    Commit (e, std::move (*next));
            if (!e.job.valid () && !e.waiting)
                Ask (e, e.session.design);
            e.drag = Drag::None, e.wall.reset (), e.end.reset ();
        }
    }
    if (hovered && ImGui::IsKeyPressed (ImGuiKey_Escape) && e.drag != Drag::None) {
        e.drag = Drag::None;
        Ask (e, e.session.design);
    }
    if (hovered && (ImGui::IsKeyPressed (ImGuiKey_Delete) || ImGui::IsKeyPressed (ImGuiKey_Backspace))) {
        if (e.core >= 0)
            Commit (e, fe::RemoveStair (s, e.session.design, e.core)), e.core = -1;
        else if (e.flat >= 0)
            Commit (e, fe::RemoveFlat (s, e.session.design, e.flat)), e.flat = -1;
    }

    // Drawing.
    auto* draw = ImGui::GetWindowDrawList ();
    const ImVec2 end (origin.x + size.x, origin.y + size.y);
    draw->AddRectFilled (origin, end, ImGui::GetColorU32 (ImGuiCol_FrameBg), 4 * scale);
    draw->PushClipRect (origin, end, true);
    for (const auto& fill : e.fills)
        for (size_t i = 0; i + 2 < fill.triangles.size (); i += 3)
            draw->AddTriangleFilled (P (fill.triangles[i]), P (fill.triangles[i + 1]), P (fill.triangles[i + 2]),
                                     fill.colour);
    const auto ring = [&] (const fs::Ring& r, ImU32 colour, float width) {
        std::vector<ImVec2> points;
        points.reserve (r.size ());
        for (const auto& p : r)
            points.push_back (P (p));
        draw->AddPolyline (points.data (), int (points.size ()), colour, ImDrawFlags_Closed, width);
    };
    const bool rooms = e.k >= 6, detail = e.k >= 9;
    for (size_t i = 0; i < s.flats.size (); ++i) {
        const auto& f = s.flats[i];
        if (rooms)
            for (const auto& r : f.roomList)
                ring (r.shape, IM_COL32 (40, 50, 55, 90), 1);
        if (detail) {
            for (const auto& w : f.windows)
                draw->AddLine (P (w.a), P (w.b), IM_COL32 (31, 127, 196, 255), 3 * scale);
            draw->AddLine (P (f.door.a), P (f.door.b), IM_COL32 (176, 58, 168, 255), 3 * scale);
        }
        ring (f.shape, IM_COL32 (30, 40, 45, 255), 1.4f * scale);
    }
    for (const auto& c : s.culled)
        ring (c.shape, IM_COL32 (120, 120, 120, 200), 1.5f * scale);
    for (const auto& r : s.outline)
        ring (r, IM_COL32 (47, 93, 138, 255), 1.8f * scale);
    // Labels where they fit.
    for (const auto& f : s.flats) {
        fs::Vec m;
        for (const auto& p : f.shape)
            m.x += p.x / f.shape.size (), m.y += p.y / f.shape.size ();
        char label[48];
        std::snprintf (label, sizeof (label), "%s%s %.0f", f.manual ? "R " : "",
                       floorprogramme::Name (programme, f.type).c_str (), f.net);
        const auto t = ImGui::CalcTextSize (label);
        if (t.x < f.frontage * e.k * 0.95) {
            const auto c = P (m);
            draw->AddText ({ c.x - t.x / 2, c.y - t.y / 2 }, IM_COL32 (20, 25, 28, 255), label);
        }
    }
    for (const auto& d : s.diagnostics)
        if (d.level == fs::Diagnostic::Error)
            draw->AddCircleFilled (P (d.at), 5 * scale, IM_COL32 (192, 58, 55, 255));
    // Selection and hover.
    if (e.flat >= 0 && e.flat < int (s.flats.size ()))
        ring (s.flats[e.flat].shape, IM_COL32 (255, 255, 255, 255), 3 * scale);
    if (e.core >= 0 && e.core < int (s.cores.size ()))
        ring (s.cores[e.core].shape, IM_COL32 (255, 186, 0, 255), 3 * scale);
    if (hovered && e.drag == Drag::None && e.have && e.tool == Tool::Select) {
        if (auto end = fe::EndAt (s, mouse, grab)) {
            draw->AddCircle (P (end->at), 7 * scale, IM_COL32 (255, 186, 0, 255), 0, 2 * scale);
            ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
        }
        else if (auto wall = fe::WallAt (s, mouse, grab * 0.75)) {
            draw->AddLine (P (wall->a), P (wall->b), IM_COL32 (255, 186, 0, 255), 3 * scale);
            ImGui::SetMouseCursor (std::abs (wall->axis.x) > std::abs (wall->axis.y) ? ImGuiMouseCursor_ResizeEW
                                                                                     : ImGuiMouseCursor_ResizeNS);
        }
        else if (fe::CoreAt (s, mouse) >= 0)
            ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
    }
    // The gesture's ghost, drawn at once while the worker catches up.
    if (e.drag == Drag::Stair && e.dragCore >= 0 && e.dragCore < int (e.base.cores.size ())) {
        fs::Ring moved = e.base.cores[e.dragCore].shape;
        for (auto& p : moved)
            p.x += e.at.x - e.from.x, p.y += e.at.y - e.from.y;
        ring (moved, IM_COL32 (255, 186, 0, 255), 2.5f * scale);
    }
    if (e.drag == Drag::Wall && e.wall) {
        const double t = (e.at.x - e.from.x) * e.wall->axis.x + (e.at.y - e.from.y) * e.wall->axis.y;
        const fs::Vec a { e.wall->a.x + e.wall->axis.x * t, e.wall->a.y + e.wall->axis.y * t };
        const fs::Vec b { e.wall->b.x + e.wall->axis.x * t, e.wall->b.y + e.wall->axis.y * t };
        draw->AddLine (P (a), P (b), IM_COL32 (255, 186, 0, 255), 3 * scale);
    }
    if (e.drag == Drag::End)
        draw->AddCircleFilled (P (e.at), 6 * scale, IM_COL32 (255, 186, 0, 255));
    if (e.drag == Drag::Rect)
        ring (fe::Rectangle (e.base, e.from, e.at),
              e.tool == Tool::Cut ? IM_COL32 (192, 58, 55, 255) : IM_COL32 (44, 122, 82, 255), 2.5f * scale);
    if (e.job.valid ())
        draw->AddText ({ origin.x + 8 * scale, origin.y + 6 * scale }, IM_COL32 (90, 100, 105, 255), "planning...");
    draw->PopClipRect ();
}
} // namespace

void Section (EditorPtr& editor, const std::map<std::string, buildingplan::Plan>& plans,
              const buildingplan::Floor& shown, const std::string& owner, const floorprogramme::Programme& programme,
              float scale)
{
    const bool here = editor && std::abs (editor->z - shown.z) <= kFloorTolerance;
    if (!here || editor->owner != owner) {
        if (here)
            ImGui::TextDisabled ("This floor is open in another building's panel.");
        if (ImGui::Button (here ? "Edit here" : "Edit massing floor")) {
            if (!here) {
                editor.reset (new Editor);
                editor->z = shown.z;
                editor->session.floor = FloorAt (plans, shown.z, editor->floorKey);
                editor->session.programme = programme;
                Ask (*editor, editor->session.design);
            }
            editor->owner = owner;
            editor->fitted = false;
        }
        if (ImGui::IsItemHovered ())
            ImGui::SetTooltip ("Plan every building at this elevation as one floor: drag stairs, walls and corridor "
                               "ends, split or join flats, draw onto or out of the silhouette.");
        return;
    }
    auto& e = *editor;
    // The massing or the programme changed under the editor: plan again with the same edits.
    std::string key;
    auto floor = FloorAt (plans, e.z, key);
    if (key != e.floorKey && !floor.empty ()) {
        e.floorKey = key;
        e.session.floor = std::move (floor);
        Ask (e, e.session.design);
    }
    if (!(programme == e.session.programme)) {
        e.session.programme = programme;
        Ask (e, e.session.design);
    }
    Pump (e, e.session.programme);

    ImGui::SeparatorText ("Massing floor");
    ToolButton ("Select", Tool::Select, e.tool,
                "Click a flat or stair; drag a stair, a party wall or a corridor end. Delete removes the "
                "selected stair or flat. Wheel zooms, right drag pans.");
    SameLineIfRoom (60 * scale);
    ToolButton ("+ Stair", Tool::AddStair, e.tool,
                "Click in a wing: one more stair there; the wing's stairs re-space.");
    SameLineIfRoom (60 * scale);
    ToolButton ("Split", Tool::Split, e.tool, "Click a flat: a party wall where you click (both parts stay flats).");
    SameLineIfRoom (60 * scale);
    ToolButton ("Join", Tool::Join, e.tool, "Click a flat: it joins its narrower neighbour.");
    SameLineIfRoom (60 * scale);
    ToolButton ("Draw", Tool::Draw, e.tool,
                "Drag a rectangle onto the silhouette, square to the wing you start in; corners snap to facades "
                "and a 0.3 m grid.");
    SameLineIfRoom (60 * scale);
    ToolButton ("Cut", Tool::Cut, e.tool, "Drag a rectangle out of the silhouette.");
    SameLineIfRoom (70 * scale);
    ToolButton ("Access", Tool::Access, e.tool,
                "Click a wing: auto, centre corridor, corridor on one side, sections round stairs, sections in rows.");
    ImGui::BeginDisabled (e.session.undo.empty ());
    if (ImGui::SmallButton ("Undo") ||
        (ImGui::IsWindowFocused () && ImGui::IsKeyChordPressed (ImGuiMod_Ctrl | ImGuiKey_Z)))
        if (e.session.Undo ())
            Ask (e, e.session.design);
    ImGui::EndDisabled ();
    SameLineIfRoom (50 * scale);
    ImGui::BeginDisabled (e.session.redo.empty ());
    if (ImGui::SmallButton ("Redo") ||
        (ImGui::IsWindowFocused () && ImGui::IsKeyChordPressed (ImGuiMod_Ctrl | ImGuiKey_Y)))
        if (e.session.Redo ())
            Ask (e, e.session.design);
    ImGui::EndDisabled ();
    SameLineIfRoom (50 * scale);
    if (ImGui::SmallButton ("Reset")) {
        e.session.Reset ();
        Ask (e, e.session.design);
    }
    if (ImGui::IsItemHovered ())
        ImGui::SetTooltip ("Drop every edit and plan the floor afresh (Undo brings them back).");
    SameLineIfRoom (40 * scale);
    if (ImGui::SmallButton ("Fit"))
        e.fitted = false;
    SameLineIfRoom (50 * scale);
    if (ImGui::SmallButton ("Close")) {
        editor.reset ();
        return;
    }
    SameLineIfRoom (110 * scale);
    ImGui::SetNextItemWidth (110 * scale);
    ImGui::DragFloat ("##height", &e.height, 2.0f, 240.0f, 1600.0f, "height %.0f");

    Canvas (e, e.session.programme, scale);
    Pump (e, e.session.programme); // a gesture's design starts this frame

    const auto& s = e.scheme;
    if (!e.have) {
        ImGui::TextDisabled ("Planning the floor...");
        return;
    }
    double red = 0;
    for (const auto& u : s.unassigned)
        if (u.reason.find ("storage") == std::string::npos)
            red += std::abs (fs::Area (u.shape));
    size_t errors = 0;
    for (const auto& d : s.diagnostics)
        errors += d.level == fs::Diagnostic::Error;
    ImGui::TextWrapped ("%s | %zu flats, %zu stairs | net %.0f / gross %.0f m2 (%.2f) | unassigned %.0f m2 | %.0f ms",
                        s.typology.c_str (), s.flats.size (), s.cores.size (), s.net, s.gross,
                        s.gross > 0 ? s.net / s.gross : 0.0, red, e.took);
    if (e.flat >= 0 && e.flat < int (s.flats.size ())) {
        const auto& f = s.flats[e.flat];
        ImGui::TextWrapped ("%s%s | net %.1f m2 | %.1f m wide, %.1f m deep%s", f.manual ? "R flat, " : "",
                            floorprogramme::Name (e.session.programme, f.type).c_str (), f.net, f.frontage, f.depth,
                            f.manual ? " | walls at an angle: rooms left to you" : "");
        const auto steps = RoomSteps (e.session.programme);
        const auto now = std::find (steps.begin (), steps.end (), f.rooms);
        ImGui::BeginDisabled (now == steps.begin () || now == steps.end ());
        if (ImGui::SmallButton ("Fewer rooms") && now != steps.begin ())
            Commit (e, fe::SetRooms (s, e.session.design, e.flat, *(now - 1)));
        ImGui::EndDisabled ();
        SameLineIfRoom (90 * scale);
        ImGui::BeginDisabled (now == steps.end () || now + 1 == steps.end ());
        if (ImGui::SmallButton ("More rooms") && now != steps.end () && now + 1 != steps.end ())
            Commit (e, fe::SetRooms (s, e.session.design, e.flat, *(now + 1)));
        ImGui::EndDisabled ();
        SameLineIfRoom (70 * scale);
        if (ImGui::SmallButton ("Remove flat"))
            Commit (e, fe::RemoveFlat (s, e.session.design, e.flat)), e.flat = -1;
    }
    if (e.core >= 0 && e.core < int (s.cores.size ())) {
        ImGui::TextWrapped ("Stair %.1f x %.1f m", s.cores[e.core].width, s.cores[e.core].depth);
        SameLineIfRoom (90 * scale);
        if (ImGui::SmallButton ("Remove stair"))
            Commit (e, fe::RemoveStair (s, e.session.design, e.core)), e.core = -1;
    }
    for (size_t w = 0; w < s.wings.size () && e.tool == Tool::Access; ++w)
        ImGui::TextDisabled ("Wing %zu: %.0f x %.0f m, %s", w + 1, s.wings[w].length, s.wings[w].depth,
                             AccessName (s.wings[w].access));
    if (errors == 0)
        ImGui::TextColored (ImVec4 (0.17f, 0.48f, 0.32f, 1), "Every hard rule holds.");
    for (const auto& d : s.diagnostics)
        if (d.level == fs::Diagnostic::Error)
            ImGui::TextColored (ImVec4 (0.75f, 0.23f, 0.22f, 1), "%s", d.text.c_str ());
        else if (d.level == fs::Diagnostic::Warning && d.code.rfind ("pin.", 0) == 0)
            ImGui::TextDisabled ("%s", d.text.c_str ());
    if (!s.culled.empty ())
        ImGui::TextDisabled ("Grey outlines: cut from the massing by the typology (a suggestion).");
}
} // namespace geomsrv::archviz::hudfloorscheme
