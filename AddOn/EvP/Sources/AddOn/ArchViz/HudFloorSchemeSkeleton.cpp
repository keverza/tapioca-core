#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

// The Plan view's Skeleton stage (HudFloorSchemeEdit.hpp): the wings the floor is planned on, its
// corridors and stairs, and how each flat is entered, drawn over the plan they make and edited
// there. The skeleton is a drawing the virtual editor's kernel edits (FloorSchemeSketch.hpp): the
// pointer picks a corner, a side or a shape with its pick, a drag is its edit of that part and
// snaps flush to the floor's and the skeleton's own lines with its snap, and the drawing is read
// back through the floor plan's edits -- so nothing is accepted here that the plan then leaves out.
// What the floor cannot take is red while it is held and said on the notice line when let go. The
// canvas only: the locked view keeps the flats' gestures.
namespace geomsrv::archviz::hudfloorscheme::detail {
namespace {
namespace ge = ::geomsrv::edit;
namespace sk = fs::sketch;
const ImU32 kLine = IM_COL32 (30, 40, 45, 255), kFound = IM_COL32 (47, 93, 138, 255),
            kMine = IM_COL32 (255, 186, 0, 255), kBad = IM_COL32 (214, 48, 49, 255),
            kVeil = IM_COL32 (236, 238, 232, 150), kLane = IM_COL32 (122, 72, 160, 255),
            kEdge = IM_COL32 (122, 72, 160, 120), kNode = IM_COL32 (255, 255, 255, 235),
            kSnap = IM_COL32 (0, 150, 136, 255);
constexpr float kPickReach = 9, kNodeReach = 7, kSnapReach = 8; // pixels

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
// A flat's node; else the nearest part of the drawing the virtual editor's pick gives -- a stair's
// before a corridor's before a wing's, an edge or a corner before the inside of a shape.
Under Pick (const Editor& e, const View& v, ImVec2 mouse, float scale)
{
    Under u;
    u.wing = fe::WingAt (e.scheme, v.W (mouse));
    for (const auto& node : e.graph.nodes)
        if (node.kind == sk::Graph::Kind::Flat) {
            const ImVec2 p = v.P (node.at);
            if (std::hypot (p.x - mouse.x, p.y - mouse.y) < kNodeReach * scale)
                return u.flat = node.index, u;
        }
    ge::PickView view;
    view.toScreen = [&v] (ge::Vec3 w, double& x, double& y) {
        const ImVec2 p = v.P ({ w.x, w.y });
        return x = p.x, y = p.y, true;
    };
    int best = INT_MAX;
    for (const auto& hit : ge::Pick (e.sketch.doc, view, mouse.x, mouse.y,
                                     ge::kPickVertex | ge::kPickSegment | ge::kPickEntity, kPickReach * scale)) {
        const auto* item = e.sketch.Find (hit.ref.entity);
        if (!item || (item->what == sk::What::Corridor && hit.ref.part == ge::Part::Entity))
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
const char* PartName (const Under& u)
{
    if (u.flat >= 0)
        return "drop it on another flat to change their places";
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

namespace {
// The pointer over the canvas in the Skeleton stage: what is under it, a gesture begun, carried on
// and let go, Delete, the right-click menu opened, the cursor.
Under Pointer (Editor& e, Context& c, const View& v, bool hovered, float scale)
{
    const ImVec2 at = ImGui::GetIO ().MousePos;
    const fs::Vec mouse = v.W (at);
    const double px = v.Pixel ();
    const auto& s = e.scheme;
    if (!Valid (s, e.wing))
        e.wing = -1;
    const Under under = hovered && e.have ? Pick (e, v, at, scale) : Under {};
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && e.drag == Drag::None) {
        e.from = e.at = mouse;
        e.base = s;
        e.baseDesign = e.input.design;
        e.owner = ImGui::GetCurrentContext ();
        e.moved = false;
        e.dragOn = Surface::Canvas;
        e.wing = under.wing;
        e.hold = { e.sketch, under.ref, under.flat, kSnapReach * px };
        if (!under.item)
            e.hold.ref = {};
        e.drag = e.hold.Held () ? Drag::Body : Drag::None;
    }
    if (e.drag != Drag::None && e.owner == ImGui::GetCurrentContext () && e.dragOn == Surface::Canvas) {
        e.at = mouse;
        e.hold.reach = kSnapReach * px;
        e.moved = e.moved || std::hypot (e.at.x - e.from.x, e.at.y - e.from.y) > 4 * px;
        if (!ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            std::string why;
            if (e.moved && e.hold.flat >= 0) {
                if (const int other = fe::FlatAt (e.base, e.at); other >= 0 && other != e.hold.flat)
                    Apply (e, c, e.baseDesign, fe::SwapFlats (e.base, e.baseDesign, e.hold.flat, other, &why), why);
                else
                    Notice (e, "Drop the flat on another flat to change their places.");
            }
            else if (e.moved) {
                auto pull = SkeletonGesture (e, c);
                Apply (e, c, e.baseDesign, std::move (pull.next), pull.why);
            }
            e.drag = Drag::None, e.hold = {};
        }
    }
    if (hovered && ImGui::IsKeyPressed (ImGuiKey_Escape))
        e.drag = Drag::None, e.hold = {};
    if (hovered && e.have && Valid (s, e.wing) && e.drag == Drag::None &&
        (ImGui::IsKeyPressed (ImGuiKey_Delete) || ImGui::IsKeyPressed (ImGuiKey_Backspace))) {
        std::string why;
        if (Apply (e, c, e.input.design, fe::RemoveWing (s, e.input.design, e.wing, &why), why))
            e.wing = -1;
    }
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Right)) {
        hudshell::ClaimRightClick ();
        e.menuAt = mouse;
        e.wing = under.wing;
        ImGui::OpenPopup ("##skeletonMenu");
    }
    if (hovered) {
        hudshell::OwnCursor ();
        const bool body = under.flat >= 0 || (under.item && under.ref.part == ge::Part::Entity);
        ImGui::SetMouseCursor (e.drag != Drag::None || (under.Any () && !body) ? ImGuiMouseCursor_ResizeAll
                               : body                                          ? ImGuiMouseCursor_Hand
                                                                               : ImGuiMouseCursor_Arrow);
    }
    return under;
}
} // namespace

void SkeletonCanvas (Editor& e, Context& c, float scale)
{
    std::vector<fs::Ring> all = e.input.owned;
    all.insert (all.end (), e.real.begin (), e.real.end ());
    const View v =
        Fit (all, ImGui::GetCursorScreenPos (), (std::max) (120.0f, ImGui::GetContentRegionAvail ().x), scale);
    ImGui::InvisibleButton ("##skeleton", v.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (e.have && e.sketchOf != e.revision) {
        e.sketch = sk::Of (e.scheme);
        e.graph = sk::Access (e.scheme);
        e.sketchOf = e.revision;
    }
    const auto under = Pointer (e, c, v, hovered, scale);
    const auto& s = e.scheme;
    auto* draw = ImGui::GetWindowDrawList ();
    const ImVec2 end (v.origin.x + v.size.x, v.origin.y + v.size.y);

    draw->AddRectFilled (v.origin, end, ImGui::GetColorU32 (ImGuiCol_FrameBg), 4 * scale);
    draw->PushClipRect (v.origin, end, true);
    // The plan the skeleton makes, faint under it: what a change will be a change to.
    for (const auto* fills : { &e.around, &e.fills })
        for (const auto& f : *fills)
            for (size_t i = 0; i + 2 < f.triangles.size (); i += 3)
                draw->AddTriangleFilled (v.P (f.triangles[i]), v.P (f.triangles[i + 1]), v.P (f.triangles[i + 2]),
                                         f.colour);
    for (const auto& f : s.flats)
        Ring (draw, v, f.shape, IM_COL32 (30, 40, 45, 90), 1);
    draw->AddRectFilled (v.origin, end, kVeil);
    for (const auto& r : e.real)
        for (size_t i = 0; i < r.size (); ++i)
            Dashed (draw, v.P (r[i]), v.P (r[(i + 1) % r.size ()]), kFound, 1.3f * scale, 5 * scale);
    // Circulation: the corridors a wing has of its own are held by their middle line.
    for (const auto& corridor : s.corridors) {
        Ring (draw, v, corridor.shape, kLine, 1.3f * scale);
        for (size_t i = 0; i + 1 < corridor.axis.size (); ++i)
            Dashed (draw, v.P (corridor.axis[i]), v.P (corridor.axis[i + 1]), kLane, 1.5f * scale, 6 * scale);
    }
    for (const auto& core : s.cores)
        Ring (draw, v, core.shape, kLine, 2 * scale);
    // Floor no wing takes, and what the typology left out.
    for (const auto& u : s.unassigned)
        if (u.reason.find ("storage") == std::string::npos)
            Ring (draw, v, u.shape, kBad, 1.5f * scale);
    for (const auto& p : s.culled)
        Ring (draw, v, p.shape, IM_COL32 (120, 120, 120, 200), 1.2f * scale);
    // How the floor is entered: each flat through its door to the circulation it opens onto.
    if (e.sketchOf == e.revision) {
        for (const auto& edge : e.graph.edges)
            for (size_t i = 0; i + 1 < edge.line.size (); ++i)
                draw->AddLine (v.P (edge.line[i]), v.P (edge.line[i + 1]), kEdge, 1.2f * scale);
        for (const auto& node : e.graph.nodes) {
            const ImVec2 p = v.P (node.at);
            const bool hot = node.kind == sk::Graph::Kind::Flat && node.index == under.flat;
            if (node.kind == sk::Graph::Kind::Stair)
                Square (draw, p, 3.5f * scale, kLine, 1);
            else if (node.kind == sk::Graph::Kind::Corridor)
                draw->AddCircleFilled (p, 3.5f * scale, kLane);
            else {
                draw->AddCircleFilled (p, (hot ? 5.5f : 4.0f) * scale, kNode);
                draw->AddCircle (p, (hot ? 5.5f : 4.0f) * scale, kLane, 0, 1.3f * scale);
            }
        }
        // The ends a corridor is held by.
        for (const auto& corridor : s.corridors)
            if (corridor.axis.size () >= 2)
                for (const auto& p : { corridor.axis.front (), corridor.axis.back () })
                    Square (draw, v.P (p), 3.5f * scale, kLane, 1);
    }
    // The wings: the user's in the accent, the generator's in blue; the selected one thick.
    const ImU32 colour = s.authored ? kMine : kFound;
    for (size_t i = 0; i < s.wings.size (); ++i) {
        const auto& w = s.wings[i];
        const bool selected = int (i) == e.wing;
        Ring (draw, v, w.rect, colour, (selected ? 3.5f : 2.0f) * scale);
        char label[48];
        std::snprintf (label, sizeof (label), "W%u  %.1f x %.1f", fe::WingId (s, int (i)), w.length, w.depth);
        const ImVec2 size = ImGui::CalcTextSize (label), mid = v.P ({ (w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2 });
        const fs::Vec u = Along (w);
        const double room = std::abs (u.x) * w.length + std::abs (u.y) * w.depth; // across the screen, m
        if (size.x + 8 * scale < room / v.Pixel ())
            draw->AddText ({ mid.x - size.x / 2, mid.y - size.y / 2 }, kLine, label);
    }
    for (const auto& d : s.diagnostics)
        if (d.level == fs::Diagnostic::Error)
            draw->AddCircleFilled (v.P (d.at), 4 * scale, kBad);
    // The selected wing's grips -- its corners and the middles of its sides -- and the part under
    // the pointer, as it would be held.
    if (e.drag == Drag::None && e.sketchOf == e.revision) {
        if (const auto* item = e.sketch.Find (sk::What::Wing, e.wing))
            if (const auto* entity = ge::Find (e.sketch.doc, item->entity))
                for (size_t k = 0; k < ge::SegmentCount (*entity); ++k) {
                    size_t from = 0, to = 0;
                    ge::SegmentEnds (*entity, k, from, to);
                    const fs::Vec a = Q (entity->vertices[from].p), b = Q (entity->vertices[to].p);
                    Square (draw, v.P (a), 3.5f * scale, kMine, 1.2f * scale);
                    Square (draw, v.P ({ (a.x + b.x) / 2, (a.y + b.y) / 2 }), 4.5f * scale, kMine, 1.2f * scale);
                }
        if (under.item)
            if (const auto* entity = ge::Find (e.sketch.doc, under.ref.entity)) {
                for (const auto& [a, b] : Edges (*entity, under.ref))
                    draw->AddLine (v.P (Q (a)), v.P (Q (b)), IM_COL32 (255, 255, 255, 220), 3.5f * scale);
                if (under.ref.part == ge::Part::Vertex)
                    if (const int i = ge::VertexIndex (*entity, under.ref.item); i >= 0)
                        Square (draw, v.P (Q (entity->vertices[i].p)), 6 * scale, kMine, 1.2f * scale);
            }
    }
    // The gesture, drawn at once: the part where it would stand, red when the floor cannot take
    // it; what it snapped to.
    std::string why;
    if (e.drag != Drag::None && e.moved && e.hold.flat >= 0 && e.hold.flat < int (e.base.flats.size ())) {
        fs::Ring moved = e.base.flats[e.hold.flat].shape;
        for (auto& p : moved)
            p.x += e.at.x - e.from.x, p.y += e.at.y - e.from.y;
        Ring (draw, v, moved, kMine, 2.5f * scale);
        if (const int other = fe::FlatAt (e.base, e.at); other >= 0 && other != e.hold.flat)
            Ring (draw, v, e.base.flats[other].shape, kMine, 3 * scale);
    }
    else if (e.drag != Drag::None && e.moved) {
        const auto pull = SkeletonGesture (e, c);
        why = pull.refused ? pull.why : std::string ();
        if (const auto* entity = ge::Find (pull.doc, e.hold.ref.entity))
            Outline (draw, v, *entity, pull.refused ? kBad : kMine, 3 * scale);
        for (const auto& guide : pull.guides)
            Dashed (draw, v.P (Q (guide.from)), v.P (Q (guide.to)), kSnap, 1.5f * scale, 4 * scale);
        if (pull.snapped && why.empty ())
            why = "flush";
    }
    if (c.planner.Pending (bp::FloorId (e.key, e.story), e.input.signature))
        draw->AddText ({ v.origin.x + 6 * scale, v.origin.y + 4 * scale }, IM_COL32 (90, 100, 105, 255), "planning...");
    draw->PopClipRect ();

    if (!hovered || ImGui::IsPopupOpen ("##skeletonMenu"))
        return;
    if (e.drag != Drag::None && !why.empty ())
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right, why);
    else if (e.drag == Drag::None && under.flat >= 0 && under.flat < int (s.flats.size ()))
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         hudshell::Formatted ("Flat | %.1f rooms | %.1f m2 | %s", s.flats[under.flat].rooms,
                                              s.flats[under.flat].net, PartName (under)));
    else if (e.drag == Drag::None && under.item && under.item->what == sk::What::Stair)
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         hudshell::Formatted ("Stair | %.1f x %.1f m | %s", s.cores[under.item->index].width,
                                              s.cores[under.item->index].depth, PartName (under)));
    else if (e.drag == Drag::None && under.item && under.item->what == sk::What::Corridor)
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         hudshell::Formatted ("Corridor | %s", PartName (under)));
    else if (e.drag == Drag::None && Valid (s, under.wing)) {
        const auto& w = s.wings[under.wing];
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         hudshell::Formatted ("Wing %u | %.1f x %.1f m | %s%s%s", fe::WingId (s, under.wing), w.length,
                                              w.depth, AccessName (w.access), under.item ? " | " : "",
                                              under.item   ? PartName (under)
                                              : s.authored ? ""
                                                           : " | the generator's: an edit makes the wings yours"));
    }
}

void SkeletonMenu (Editor& e, Context& c)
{
    if (!ImGui::BeginPopup ("##skeletonMenu"))
        return;
    const auto& s = e.scheme;
    const auto& full = e.input.design;
    std::string why;
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
        if (const int core = fe::CoreAt (s, e.menuAt); core >= 0) {
            ImGui::Separator ();
            if (ImGui::MenuItem ("Turn the stair a quarter"))
                Apply (e, c, full, fe::RotateStair (s, full, core, &why), why);
            if (ImGui::MenuItem ("Remove the stair"))
                Apply (e, c, full, fe::RemoveStair (s, full, core, &why), why);
        }
        else if (ImGui::MenuItem ("Add a stair here"))
            Apply (e, c, full, fe::AddStair (s, full, e.menuAt, &why), why);
        ImGui::Separator ();
        if (ImGui::MenuItem ("Remove wing") && Apply (e, c, full, fe::RemoveWing (s, full, e.wing, &why), why))
            e.wing = -1;
    }
    else if (ImGui::MenuItem ("Add a wing here"))
        Apply (e, c, full, fe::AddWingAt (s, full, e.menuAt, &why, c.options), why);
    ImGui::Separator ();
    if (fe::Authored (full)) {
        if (ImGui::MenuItem ("Let the generator find the wings"))
            Commit (e, c, fe::ReleaseSkeleton (full)), e.wing = -1;
        hudshell::Tip ("The wings are searched for again; stairs, walls and flats you set stay.",
                       hudshell::TipSide::Right);
    }
    else {
        if (ImGui::MenuItem ("Keep these wings as mine"))
            Commit (e, c, fe::AdoptSkeleton (s, full));
        hudshell::Tip ("The wings shown become the design's own: the floor is planned on them from now on.",
                       hudshell::TipSide::Right);
    }
    ImGui::EndPopup ();
}
} // namespace geomsrv::archviz::hudfloorscheme::detail
