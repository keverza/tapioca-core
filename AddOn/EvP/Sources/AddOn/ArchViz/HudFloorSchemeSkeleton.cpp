#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

// The skeleton on the Plan view's canvas, with the flats (user, 2026-11-15: "units and skeleton
// should be single view and workflow"): the wings the floor is planned on, its corridors' centre
// lines and how each flat is entered, drawn over the plan and edited there. The skeleton is a
// drawing the virtual editor's kernel edits (FloorSchemeSketch.hpp): the pointer picks a wing's
// side or corner, a corridor's leg or a stair's side with its pick, a drag is its edit of that part
// and snaps flush to the floor's and the skeleton's own lines with its snap, and the drawing is
// read back through the floor plan's edits -- so nothing is accepted here that the plan then leaves
// out. Everything else under the pointer -- a flat, a wall, a stair's body, a corridor's end -- is
// the canvas's own (HudFloorSchemeCanvas.cpp). The canvas only, never the locked view.
namespace geomsrv::archviz::hudfloorscheme::detail {
namespace {
namespace ge = ::geomsrv::edit;
namespace sk = fs::sketch;
const ImU32 kLine = IM_COL32 (30, 40, 45, 255), kFound = IM_COL32 (47, 93, 138, 255),
            kMine = IM_COL32 (255, 186, 0, 255), kBad = IM_COL32 (214, 48, 49, 255),
            kVeil = IM_COL32 (236, 238, 232, 150), kLane = IM_COL32 (122, 72, 160, 255),
            kEdge = IM_COL32 (122, 72, 160, 120), kNode = IM_COL32 (255, 255, 255, 235),
            kSnap = IM_COL32 (0, 150, 136, 255);
constexpr float kPickReach = 7, kSnapReach = 8; // pixels

fs::Vec Along (const fs::Wing& w)
{
    const double l = std::hypot (w.b.x - w.a.x, w.b.y - w.a.y);
    return l > 1e-9 ? fs::Vec { (w.b.x - w.a.x) / l, (w.b.y - w.a.y) / l } : fs::Vec { 1, 0 };
}
fs::Vec Q (ge::Vec3 p)
{
    return { p.x, p.y };
}
ge::Vec3 P3 (fs::Vec p)
{
    return { p.x, p.y, 0.0 };
}
bool Valid (const fs::Scheme& s, int wing)
{
    return wing >= 0 && wing < int (s.wings.size ());
}
bool HasCorridor (const fs::Wing& w)
{
    return w.access == fs::Access::Centre || w.access == fs::Access::OneSide;
}
// The width the user gave the wing's corridor; 0 when it is the generator's.
double PinnedWidth (const fs::Scheme& s, const fe::Design& d, int wing)
{
    for (const auto& pin : d.pins.corridors)
        if (fe::WingAt (s, pin.at) == wing)
            return pin.width;
    return 0;
}
// What of the skeleton is under the pointer.
struct Under {
    ge::Ref ref;
    const sk::Item* item = nullptr;
    int flat = -1, wing = -1;
    bool Any () const
    {
        return item != nullptr || flat >= 0;
    }
};
// The nearest edge of the drawing the virtual editor's pick gives: a stair's side before a
// corridor's leg before a wing's side or corner.
Under Pick (const Editor& e, const View& v, ImVec2 mouse, float scale)
{
    Under u;
    u.wing = fe::WingAt (e.scheme, v.W (mouse));
    ge::PickView view;
    view.toScreen = [&v] (ge::Vec3 w, double& x, double& y) {
        const ImVec2 p = v.P ({ w.x, w.y });
        return x = p.x, y = p.y, true;
    };
    int best = INT_MAX;
    for (const auto& hit : ge::Pick (e.sketch.doc, view, mouse.x, mouse.y,
                                     ge::kPickVertex | ge::kPickSegment | ge::kPickEntity, kPickReach * scale)) {
        const auto* item = e.sketch.Find (hit.ref.entity);
        // Edges only: inside a shape the pointer is on a flat or a stair, which the canvas holds;
        // a corridor's end is the canvas's too (it plans the floor live while it is dragged).
        if (!item || hit.inside || hit.ref.part == ge::Part::Entity ||
            (item->what == sk::What::Corridor && hit.ref.part == ge::Part::Vertex) ||
            (item->what == sk::What::Stair && hit.ref.part == ge::Part::Vertex))
            continue;
        const int rank = (hit.inside ? 10 : 0) + (item->what == sk::What::Stair      ? 0
                                                  : item->what == sk::What::Corridor ? 1
                                                                                     : 2);
        if (rank < best)
            best = rank, u.ref = hit.ref, u.item = item;
    }
    if (u.item && u.item->what == sk::What::Wing)
        u.wing = u.item->index;
    return u;
}
// The lines of `entity`'s part `ref`: a side, the sides at a corner, or every side.
std::vector<std::pair<ge::Vec3, ge::Vec3>> Edges (const ge::Entity& entity, const ge::Ref& ref)
{
    std::vector<std::pair<ge::Vec3, ge::Vec3>> out;
    for (size_t k = 0; k < ge::SegmentCount (entity); ++k) {
        size_t from = 0, to = 0;
        ge::SegmentEnds (entity, k, from, to);
        if (ref.part == ge::Part::Segment && entity.segments[k].id != ref.item)
            continue;
        if (ref.part == ge::Part::Vertex && entity.vertices[from].id != ref.item && entity.vertices[to].id != ref.item)
            continue;
        out.push_back ({ entity.vertices[from].p, entity.vertices[to].p });
    }
    return out;
}
void Outline (ImDrawList* draw, const View& v, const ge::Entity& entity, ImU32 colour, float width)
{
    for (size_t k = 0; k < ge::SegmentCount (entity); ++k) {
        size_t from = 0, to = 0;
        ge::SegmentEnds (entity, k, from, to);
        draw->AddLine (v.P (Q (entity.vertices[from].p)), v.P (Q (entity.vertices[to].p)), colour, width);
    }
}
void Square (ImDrawList* draw, ImVec2 p, float h, ImU32 fill, float line)
{
    draw->AddRectFilled ({ p.x - h, p.y - h }, { p.x + h, p.y + h }, fill);
    draw->AddRect ({ p.x - h, p.y - h }, { p.x + h, p.y + h }, kLine, 0, 0, line);
}
// The flat whose entrance door's middle is within `reach` metres of `m`; -1 when none is.
int DoorUnder (const fs::Scheme& s, fs::Vec m, double reach)
{
    int best = -1;
    double near = reach;
    for (size_t i = 0; i < s.flats.size (); ++i)
        if (const auto door = fe::DoorOf (s, int (i)))
            if (const double d = std::hypot (door->x - m.x, door->y - m.y); d <= near)
                near = d, best = int (i);
    return best;
}
void Door (ImDrawList* draw, const View& v, const fs::Opening& door, fs::Vec at, ImU32 colour, float width)
{
    const fs::Vec half { (door.b.x - door.a.x) / 2, (door.b.y - door.a.y) / 2 };
    draw->AddLine (v.P ({ at.x - half.x, at.y - half.y }), v.P ({ at.x + half.x, at.y + half.y }), colour, width);
}
const char* RoomName (fs::RoomKind kind)
{
    switch (kind) {
        case fs::RoomKind::Living:
            return "Living room";
        case fs::RoomKind::Bedroom:
            return "Bedroom";
        case fs::RoomKind::Alcove:
            return "Alcove";
        case fs::RoomKind::Hall:
            return "Hall";
        case fs::RoomKind::Bath:
            return "Bath";
        case fs::RoomKind::Wc:
            return "WC";
        default:
            return "Storage";
    }
}
const char* PartName (const Under& u)
{
    if (!u.item)
        return "";
    const bool side = u.ref.part == ge::Part::Segment, corner = u.ref.part == ge::Part::Vertex;
    switch (u.item->what) {
        case sk::What::Stair:
            return side ? "drag the side: the stair's size" : "drag the stair along its wing";
        case sk::What::Corridor:
            return corner ? "drag the end: the corridor's length" : "drag the corridor across its wing";
        default:
            return side ? "drag the side" : corner ? "drag the corner" : "drag the wing";
    }
}
} // namespace

SkeletonPull SkeletonGesture (const Editor& e, const Context& c)
{
    SkeletonPull pull;
    pull.next = e.baseDesign;
    const auto& h = e.hold;
    pull.doc = h.sketch.doc;
    const auto* item = h.sketch.Find (h.ref.entity);
    if (!item)
        return pull;
    // In steps along the wing where nothing snaps.
    const fs::Vec to = fe::Stepped (e.base, e.from, e.at);
    fs::Vec by { to.x - e.from.x, to.y - e.from.y };
    std::string error;
    const auto drag = [&] {
        pull.doc = h.sketch.doc;
        error.clear ();
        return sk::Drag (pull.doc, h.sketch, h.ref, by, error);
    };
    if (!drag ()) {
        pull.refused = true, pull.why = error;
        return pull;
    }
    // The virtual editor's snap, against the floor's own lines and the rest of the skeleton: an end
    // to a point, a side or a shape flush with a line beside it.
    const auto context = sk::Context (e.base, e.real);
    ge::SnapQuery query;
    query.cursor = P3 (e.at);
    query.tolerance = h.reach;
    query.anchor = P3 (e.from);
    query.exclude = { ge::Ref { h.ref.entity } };
    query.modes =
        ge::kSnapEndpoint | ge::kSnapMidpoint | ge::kSnapIntersection | ge::kSnapPerpendicular | ge::kSnapNearest;
    const ge::Entity* was = ge::Find (h.sketch.doc, h.ref.entity);
    const ge::Entity* now = ge::Find (pull.doc, h.ref.entity);
    ge::SnapResult snap;
    if (item->what == sk::What::Corridor && h.ref.part == ge::Part::Vertex) {
        snap = ge::Snap (h.sketch.doc, context, query);
        if (const int i = was ? ge::VertexIndex (*was, h.ref.item) : -1; snap.snapped && i >= 0)
            by = { snap.point.x - was->vertices[i].p.x, snap.point.y - was->vertices[i].p.y };
        else
            snap.snapped = false;
    }
    else if (now && item->what != sk::What::Corridor) {
        // A corridor's leg is its centre line, flush with nothing: it goes in steps.
        ge::Vec3 translate = P3 (by);
        if (ge::SnapEdges (h.sketch.doc, context, query, Edges (*now, h.ref), translate, snap))
            by = Q (translate), snap.snapped = true;
        else
            snap.snapped = false;
    }
    if (snap.snapped && !drag ()) {
        pull.refused = true, pull.why = error;
        return pull;
    }
    pull.snapped = snap.snapped;
    pull.guides = std::move (snap.guides);
    pull.next = sk::Read (e.base, e.baseDesign, h.sketch, pull.doc, &pull.why, c.options);
    pull.refused = pull.next == e.baseDesign && !pull.why.empty ();
    return pull;
}

bool SkeletonPointer (Editor& e, Context& c, const View& v, bool hovered, float scale)
{
    if (e.have && e.sketchOf != e.revision) {
        e.sketch = sk::Of (e.scheme);
        e.graph = sk::Access (e.scheme);
        e.sketchOf = e.revision;
    }
    const ImVec2 at = ImGui::GetIO ().MousePos;
    const fs::Vec mouse = v.W (at);
    const double px = v.Pixel ();
    e.over = {};
    e.overDoor = -1;
    // A gesture of its own, carried on and let go.
    if (e.drag != Drag::None && e.hold.Held ()) {
        if (e.owner != ImGui::GetCurrentContext () || e.dragOn != Surface::Canvas)
            return true;
        e.at = mouse;
        e.hold.reach = kSnapReach * px;
        e.moved = e.moved || std::hypot (e.at.x - e.from.x, e.at.y - e.from.y) > 4 * px;
        if (ImGui::IsKeyPressed (ImGuiKey_Escape))
            e.drag = Drag::None, e.hold = {};
        else if (!ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            if (e.moved && e.hold.flat >= 0) {
                // An entrance door, let go: along its wall, as near here as it opens onto circulation.
                std::string why;
                auto next = fe::MoveDoor (e.base, e.baseDesign, e.hold.flat, e.at, &why);
                Apply (e, c, e.baseDesign, std::move (next), why);
            }
            else if (e.moved) {
                auto pull = SkeletonGesture (e, c);
                Apply (e, c, e.baseDesign, std::move (pull.next), pull.why);
            }
            e.drag = Drag::None, e.hold = {};
        }
        hudshell::OwnCursor ();
        ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
        return true;
    }
    // The canvas's own gesture, or its menu: the pointer is not the skeleton's.
    if (!hovered || !e.have || e.drag != Drag::None || e.sketchOf != e.revision ||
        ImGui::IsMouseClicked (ImGuiMouseButton_Right) || ImGui::IsPopupOpen ("##planMenu"))
        return false;
    // A wall against the next building, a corridor's end and a zone's division are held first.
    const Kind unit = HitAt (e.scheme, mouse, px).kind;
    if (unit == Kind::Party || unit == Kind::End || unit == Kind::Division)
        return false;
    // A flat's entrance door, by its middle; then an edge of the skeleton.
    const int door = DoorUnder (e.scheme, mouse, kPickReach * scale * px);
    const Under under = door >= 0 ? Under {} : Pick (e, v, at, scale);
    if (door < 0 && !under.item)
        return false;
    e.over = under.ref;
    e.overDoor = door;
    if (ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
        e.from = e.at = mouse;
        e.base = e.scheme;
        e.baseDesign = e.input.design;
        e.owner = ImGui::GetCurrentContext ();
        e.moved = false;
        e.dragOn = Surface::Canvas;
        e.held = {};
        e.hold = { door >= 0 ? sk::Sketch {} : e.sketch, under.ref, door, kSnapReach * px };
        e.drag = Drag::Body;
    }
    hudshell::OwnCursor ();
    ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
    return true;
}

void SkeletonMarks (Editor& e, Context& c, const View& v, ImDrawList* draw, float scale, bool hovered)
{
    const auto& s = e.scheme;
    if (!e.have || e.sketchOf != e.revision)
        return;
    // How the floor is entered: each flat through its door to the circulation it opens onto, and
    // inside it from the door to the hall and on from room to room. A flat whose rooms are laid is
    // its rooms' nodes; one whose rooms are left to the user is one node.
    const auto laid = [&] (int flat) {
        return flat >= 0 && flat < int (s.flats.size ()) && !s.flats[flat].roomList.empty () &&
               fe::DoorOf (s, flat).has_value ();
    };
    for (const auto& edge : e.graph.edges) {
        const bool inside = edge.kind == sk::Graph::Edge::Kind::Inside;
        // From the door on, where the rooms' own edges lead up to it.
        const size_t first = edge.kind == sk::Graph::Edge::Kind::Access && edge.line.size () == 3 && laid (edge.flat);
        for (size_t i = first; i + 1 < edge.line.size (); ++i)
            draw->AddLine (v.P (edge.line[i]), v.P (edge.line[i + 1]), kEdge, (inside ? 1.0f : 1.2f) * scale);
    }
    const sk::Graph::Node* room = nullptr; // the room's node under the pointer
    for (const auto& node : e.graph.nodes) {
        const ImVec2 p = v.P (node.at);
        if (node.kind == sk::Graph::Kind::Stair)
            Square (draw, p, 3.0f * scale, kLine, 1);
        else if (node.kind == sk::Graph::Kind::Corridor)
            draw->AddCircleFilled (p, 3.0f * scale, kLane);
        else if (node.kind == sk::Graph::Kind::Room) {
            const bool hall = node.what == fs::RoomKind::Hall;
            draw->AddCircleFilled (p, (hall ? 2.6f : 2.0f) * scale, hall ? kLane : kNode);
            draw->AddCircle (p, (hall ? 2.6f : 2.0f) * scale, kLane, 0, 1.0f * scale);
            const ImVec2 m = ImGui::GetIO ().MousePos;
            if (hovered && std::hypot (m.x - p.x, m.y - p.y) <= 5 * scale)
                room = &node;
        }
        else if (!laid (node.index)) {
            draw->AddCircleFilled (p, 3.0f * scale, kNode);
            draw->AddCircle (p, 3.0f * scale, kLane, 0, 1.2f * scale);
        }
    }
    // Each entrance door, a handle on its wall: the one under the pointer as it would be held.
    for (size_t i = 0; i < s.flats.size (); ++i)
        if (const auto at = fe::DoorOf (s, int (i))) {
            const bool over = e.drag == Drag::None && e.overDoor == int (i);
            Door (draw, v, s.flats[i].door, *at, over ? kMine : kLane, (over ? 5.0f : 3.0f) * scale);
        }
    for (const auto& corridor : s.corridors)
        for (size_t i = 0; i + 1 < corridor.axis.size (); ++i)
            Dashed (draw, v.P (corridor.axis[i]), v.P (corridor.axis[i + 1]), kLane, 1.5f * scale, 6 * scale);
    // The wings: the user's in the accent, the generator's in blue.
    const ImU32 colour = s.authored ? kMine : kFound;
    for (const auto& w : s.wings)
        Ring (draw, v, w.rect, colour, 2.0f * scale);
    // The part under the pointer, as it would be held.
    if (e.drag == Drag::None && e.over.entity != ge::kNoId)
        if (const auto* entity = ge::Find (e.sketch.doc, e.over.entity)) {
            for (const auto& [a, b] : Edges (*entity, e.over))
                draw->AddLine (v.P (Q (a)), v.P (Q (b)), IM_COL32 (255, 255, 255, 230), 4 * scale);
            if (e.over.part == ge::Part::Vertex)
                if (const int i = ge::VertexIndex (*entity, e.over.item); i >= 0)
                    Square (draw, v.P (Q (entity->vertices[i].p)), 6 * scale, kMine, 1.2f * scale);
        }
    // The gesture, drawn at once: the part where it would stand, red when the floor cannot take
    // it; what it snapped to.
    std::string why;
    if (e.drag != Drag::None && e.hold.flat >= 0 && e.moved && e.hold.flat < int (e.base.flats.size ())) {
        // A held door: where it would stand on its wall, red where it has nowhere to go.
        const auto place = fe::DoorPlace (e.base, e.hold.flat, e.at);
        const auto was = fe::DoorOf (e.base, e.hold.flat);
        if (place || was)
            Door (draw, v, e.base.flats[e.hold.flat].door, place ? *place : *was, place ? kMine : kBad, 5 * scale);
        why = place ? "the hall follows the door" : "no way in from circulation there";
    }
    else if (e.drag != Drag::None && e.hold.Held () && e.moved) {
        const auto pull = SkeletonGesture (e, c);
        why = pull.refused ? pull.why : pull.snapped ? std::string ("flush") : std::string ();
        if (const auto* entity = ge::Find (pull.doc, e.hold.ref.entity))
            Outline (draw, v, *entity, pull.refused ? kBad : kMine, 3 * scale);
        for (const auto& guide : pull.guides)
            Dashed (draw, v.P (Q (guide.from)), v.P (Q (guide.to)), kSnap, 1.5f * scale, 4 * scale);
    }
    if (!hovered)
        return;
    if (!why.empty ())
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right, why);
    else if (e.drag == Drag::None && e.overDoor >= 0)
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         "Entrance | drag it along the wall: the hall follows");
    else if (e.drag == Drag::None && room && e.over.entity == ge::kNoId)
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right, RoomName (room->what));
    else if (e.drag == Drag::None && e.over.entity != ge::kNoId)
        if (const auto* item = e.sketch.Find (e.over.entity)) {
            Under under;
            under.ref = e.over, under.item = item;
            if (item->what == sk::What::Wing && Valid (s, item->index)) {
                const auto& w = s.wings[item->index];
                hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                                 hudshell::Formatted ("Wing %u | %.1f x %.1f m | %s | %s%s",
                                                      fe::WingId (s, item->index), w.length, w.depth,
                                                      AccessName (w.access), PartName (under),
                                                      s.authored ? "" : " | an edit makes the wings yours"));
            }
            else
                hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                                 hudshell::Formatted ("%s | %s", item->what == sk::What::Stair ? "Stair" : "Corridor",
                                                      PartName (under)));
        }
}

void SkeletonMenuItems (Editor& e, Context& c)
{
    const auto& s = e.scheme;
    const auto& full = e.input.design;
    std::string why;
    e.wing = fe::WingAt (s, e.menuAt);
    // The flat's entrance, when it is the user's: given back to the generator.
    if (const int flat = fe::FlatAt (s, e.menuAt); flat >= 0) {
        const bool own = std::any_of (full.pins.doors.begin (), full.pins.doors.end (),
                                      [&] (const fs::Pins::Door& p) { return fe::FlatAt (s, p.at) == flat; });
        ImGui::BeginDisabled (!own);
        if (ImGui::MenuItem ("Entrance back where the generator puts it"))
            Commit (e, c, fe::ResetDoor (s, full, flat));
        ImGui::EndDisabled ();
        ImGui::Separator ();
    }
    if (Valid (s, e.wing)) {
        const auto& w = s.wings[e.wing];
        ImGui::TextDisabled ("Wing %u | %.1f x %.1f m", fe::WingId (s, e.wing), w.length, w.depth);
        fs::Access now = fs::Access::Auto;
        for (const auto& a : full.pins.access)
            if (fe::WingAt (s, a.at) == e.wing)
                now = a.access;
        ImGui::SetNextItemWidth (11 * ImGui::GetFontSize ());
        if (ImGui::BeginCombo ("Access", AccessName (now))) {
            for (auto a :
                 { fs::Access::Auto, fs::Access::Centre, fs::Access::OneSide, fs::Access::CoreOnly, fs::Access::Rows })
                if (ImGui::Selectable (AccessName (a), a == now) && a != now)
                    Commit (e, c, fe::SetAccess (s, full, e.wing, a));
            ImGui::EndCombo ();
        }
        if (HasCorridor (w)) {
            const double pinned = PinnedWidth (s, full, e.wing);
            double width = pinned > 0 ? pinned : c.options.corridor;
            ImGui::SetNextItemWidth (7 * ImGui::GetFontSize ());
            if (ImGui::InputDouble ("Corridor width, m", &width, 0.1, 0.3, "%.2f") &&
                std::abs (width - (pinned > 0 ? pinned : c.options.corridor)) > 1e-9) {
                // Where it runs now: its pin, else the corridor as planned, else the wing's middle.
                fs::Vec through { (w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2 };
                if (const auto lane = fe::CorridorOf (s, e.wing))
                    through = *lane;
                Apply (e, c, full, fe::SetCorridor (s, full, e.wing, through, width, &why, c.options), why);
            }
            const bool moved =
                std::any_of (full.pins.corridors.begin (), full.pins.corridors.end (),
                             [&] (const fs::Pins::Corridor& p) { return fe::WingAt (s, p.at) == e.wing; });
            ImGui::BeginDisabled (!moved);
            if (ImGui::MenuItem ("Corridor back to the middle"))
                Commit (e, c, fe::ResetCorridor (s, full, e.wing));
            ImGui::EndDisabled ();
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Remove wing"))
            Apply (e, c, full, fe::RemoveWing (s, full, e.wing, &why), why);
    }
    else if (ImGui::MenuItem ("Add a wing here"))
        Apply (e, c, full, fe::AddWingAt (s, full, e.menuAt, &why, c.options), why);
    ImGui::Separator ();
    if (fe::Authored (full)) {
        if (ImGui::MenuItem ("Let the generator find the wings"))
            Commit (e, c, fe::ReleaseSkeleton (full));
        hudshell::Tip ("The wings are searched for again; stairs, walls and flats you set stay.",
                       hudshell::TipSide::Right);
    }
    else {
        if (ImGui::MenuItem ("Keep these wings as mine"))
            Commit (e, c, fe::AdoptSkeleton (s, full));
        hudshell::Tip ("The wings shown become the design's own: the floor is planned on them from now on.",
                       hudshell::TipSide::Right);
    }
}
} // namespace geomsrv::archviz::hudfloorscheme::detail
