#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// The Plan view's canvas (HudFloorSchemeEdit.hpp): the building's floor across the panel's width,
// what is under the pointer, the selection's arrows, gestures in 0.4 m steps, the palette.
namespace geomsrv::archviz::hudfloorscheme::detail {
namespace cp = Clipper2Lib;
constexpr char kPayload[] = "tapioca.plan.palette";
constexpr double kStairPayload = -1;
const ImU32 kInk = IM_COL32 (30, 40, 45, 255), kAccent = IM_COL32 (255, 186, 0, 255),
            kRed = IM_COL32 (214, 48, 49, 255), kParty = IM_COL32 (40, 44, 48, 255),
            kReal = IM_COL32 (47, 93, 138, 255);

// ---- geometry -----------------------------------------------------------------------------
cp::PathD Path (const fs::Ring& ring)
{
    cp::PathD path;
    for (const auto& p : ring)
        path.emplace_back (p.x, p.y);
    return path;
}
bool Inside (const std::vector<fs::Ring>& rings, fs::Vec p)
{
    int winding = 0;
    for (const auto& r : rings)
        if (r.size () >= 3 &&
            cp::PointInPolygon (cp::PointD (p.x, p.y), Path (r)) != cp::PointInPolygonResult::IsOutside)
            winding += fs::Area (r) > 0 ? 1 : -1;
    return winding > 0;
}
fs::Vec Unit (fs::Vec v)
{
    const double l = std::hypot (v.x, v.y);
    return l > 1e-9 ? fs::Vec { v.x / l, v.y / l } : fs::Vec { 1, 0 };
}
double Dot (fs::Vec a, fs::Vec b)
{
    return a.x * b.x + a.y * b.y;
}
fs::Vec Mid (const fs::Ring& r)
{
    fs::Vec c;
    for (const auto& p : r)
        c.x += p.x / double (r.size ()), c.y += p.y / double (r.size ());
    return c;
}
double Distance (fs::Vec p, fs::Vec a, fs::Vec b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? std::clamp (((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0;
    return std::hypot (a.x + t * dx - p.x, a.y + t * dy - p.y);
}
fs::Vec Outward (const std::array<fs::Vec, 2>& wall)
{
    const fs::Vec d = Unit ({ wall[1].x - wall[0].x, wall[1].y - wall[0].y });
    return { d.y, -d.x };
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
ImU32 RoomsColour (double rooms)
{
    return hudshell::Packed (floorprogramme::Colour (rooms));
}
std::vector<Fill> Fills (const fs::Scheme& s)
{
    std::vector<Fill> fills;
    fills.push_back (Triangles (s.outline, IM_COL32 (236, 238, 232, 255)));
    for (const auto& u : s.unassigned)
        fills.push_back (Triangles ({ u.shape }, u.reason.find ("storage") != std::string::npos
                                                     ? IM_COL32 (190, 190, 182, 255)
                                                     : IM_COL32 (229, 72, 77, 150)));
    for (const auto& f : s.flats) {
        ImU32 colour = RoomsColour (f.rooms);
        if (f.manual)
            colour = (colour & 0x00FFFFFF) | 0x99000000;
        fills.push_back (Triangles ({ f.shape }, colour));
    }
    for (const auto& c : s.corridors)
        fills.push_back (Triangles ({ c.shape }, IM_COL32 (214, 196, 154, 255)));
    for (const auto& l : s.lobbies)
        fills.push_back (Triangles ({ l }, IM_COL32 (230, 218, 185, 255)));
    for (const auto& c : s.cores)
        fills.push_back (Triangles ({ c.shape }, IM_COL32 (109, 114, 118, 255)));
    return fills;
}

// ---- what is where ------------------------------------------------------------------------
Target HitAt (const fs::Scheme& s, fs::Vec m, double tolerance)
{
    if (const int core = fe::CoreAt (s, m); core >= 0)
        return { Kind::Stair, core, {}, {} };
    if (auto end = fe::EndAt (s, m, tolerance))
        return { Kind::End, end->corridor, {}, end };
    if (auto wall = fe::WallAt (s, m, tolerance * 0.8))
        return { Kind::Wall, -1, wall, {} };
    for (size_t i = 0; i < s.party.size (); ++i)
        if (Distance (m, s.party[i][0], s.party[i][1]) < tolerance)
            return { Kind::Party, int (i), {}, {} };
    if (const int flat = fe::FlatAt (s, m); flat >= 0)
        return { Kind::Flat, flat, {}, {} };
    return {};
}
// The wing frame at a point: along its axis.
fs::Vec WingAxis (const fs::Scheme& s, fs::Vec p)
{
    const int wing = fe::WingAt (s, p);
    return wing >= 0 ? Unit ({ s.wings[wing].b.x - s.wings[wing].a.x, s.wings[wing].b.y - s.wings[wing].a.y })
                     : fs::Vec { 1, 0 };
}
// Half extents of a ring along u and across it, from its middle.
std::pair<double, double> Extent (const fs::Ring& r, fs::Vec u)
{
    const fs::Vec c = Mid (r), v { -u.y, u.x };
    double a = 0, b = 0;
    for (const auto& p : r) {
        a = (std::max) (a, std::abs (Dot ({ p.x - c.x, p.y - c.y }, u)));
        b = (std::max) (b, std::abs (Dot ({ p.x - c.x, p.y - c.y }, v)));
    }
    return { a, b };
}
// The flat beyond `flat`'s side in `dir` along its band, or -1.
int Beside (const fs::Scheme& s, int flat, fs::Vec dir)
{
    const auto& f = s.flats[flat];
    const double reach = Extent (f.shape, Unit (f.axis)).first + 0.6;
    const fs::Vec c = Mid (f.shape);
    const int other = fe::FlatAt (s, { c.x + dir.x * reach, c.y + dir.y * reach });
    return other == flat ? -1 : other;
}
// The arrows on the selection, `gap` metres off it.
std::vector<Arrow> Arrows (const fs::Scheme& s, const Target& t, double gap)
{
    std::vector<Arrow> out;
    const auto pair = [&] (fs::Vec at, fs::Vec u) {
        for (double sign : { 1.0, -1.0 })
            out.push_back ({ { at.x + u.x * gap * sign, at.y + u.y * gap * sign }, { u.x * sign, u.y * sign } });
    };
    switch (t.kind) {
        case Kind::Stair: {
            if (t.index < 0 || t.index >= int (s.cores.size ()))
                break;
            const auto& core = s.cores[t.index];
            const fs::Vec u = WingAxis (s, core.centre), v { -u.y, u.x };
            const auto [a, b] = Extent (core.shape, u);
            const std::pair<fs::Vec, double> sides[] = {
                { u, a }, { { -u.x, -u.y }, a }, { v, b }, { { -v.x, -v.y }, b }
            };
            for (const auto& [dir, reach] : sides)
                out.push_back (
                    { { core.centre.x + dir.x * (reach + gap), core.centre.y + dir.y * (reach + gap) }, dir });
            break;
        }
        case Kind::Wall:
            if (t.wall)
                pair ({ (t.wall->a.x + t.wall->b.x) / 2, (t.wall->a.y + t.wall->b.y) / 2 }, Unit (t.wall->axis));
            break;
        case Kind::End: {
            if (!t.end || t.end->corridor < 0 || t.end->corridor >= int (s.corridors.size ()))
                break;
            const auto& axis = s.corridors[t.end->corridor].axis;
            if (axis.size () < 2)
                break;
            const fs::Vec a = t.end->last ? axis[axis.size () - 2] : axis[1], b = t.end->at;
            pair (b, Unit ({ b.x - a.x, b.y - a.y }));
            break;
        }
        case Kind::Party:
            if (t.index >= 0 && t.index < int (s.party.size ())) {
                const auto& w = s.party[t.index];
                pair ({ (w[0].x + w[1].x) / 2, (w[0].y + w[1].y) / 2 }, Outward (w));
            }
            break;
        case Kind::Flat: {
            // Arrows to change places with the neighbour on either side along the band.
            if (t.index < 0 || t.index >= int (s.flats.size ()))
                break;
            const auto& f = s.flats[t.index];
            const fs::Vec c = Mid (f.shape), u = Unit (f.axis);
            const double a = Extent (f.shape, u).first;
            for (double sign : { 1.0, -1.0 }) {
                const fs::Vec dir { u.x * sign, u.y * sign };
                if (Beside (s, t.index, dir) >= 0)
                    out.push_back ({ { c.x + dir.x * (a - gap), c.y + dir.y * (a - gap) }, dir });
            }
            break;
        }
        default:
            break;
    }
    return out;
}
// The selection found again in a new scheme, where it was picked.
Target Again (const fs::Scheme& s, const Target& t, fs::Vec at)
{
    if (t.kind == Kind::Flat)
        if (const int flat = fe::FlatAt (s, at); flat >= 0)
            return { Kind::Flat, flat, {}, {} };
    if (t.kind == Kind::Stair)
        if (const int core = fe::CoreAt (s, at); core >= 0)
            return { Kind::Stair, core, {}, {} };
    return {};
}

// ---- the gesture --------------------------------------------------------------------------
fs::Vec Delta (const Editor& e)
{
    fs::Vec d { e.at.x - e.from.x, e.at.y - e.from.y };
    if (e.drag == Drag::Arrow)
        d = { e.axis.x * Dot (d, e.axis), e.axis.y * Dot (d, e.axis) };
    return d;
}
double Stepped (double t)
{
    return std::round (t / fe::kStep) * fe::kStep;
}
fs::Vec StairTo (const Editor& e)
{
    const fs::Vec c = e.base.cores[e.held.index].centre, d = Delta (e);
    return fe::Stepped (e.base, c, { c.x + d.x, c.y + d.y });
}
fs::Vec EndAxis (const Editor& e)
{
    const auto arrows = Arrows (e.base, e.held, 0);
    return arrows.empty () ? fs::Vec { 1, 0 } : arrows.front ().dir;
}
// The design the held gesture makes, from the scheme it started on.
std::optional<fe::Design> Gesture (const Editor& e, const floorprogramme::Programme& programme)
{
    const fs::Vec d = Delta (e);
    switch (e.held.kind) {
        case Kind::Stair:
            if (e.held.index < 0 || e.held.index >= int (e.base.cores.size ()))
                return std::nullopt;
            return fe::MoveStair (e.base, e.baseDesign, e.held.index, StairTo (e));
        case Kind::Wall:
            if (!e.held.wall)
                return std::nullopt;
            return fe::MoveWall (e.base, e.baseDesign, *e.held.wall,
                                 fe::SnapWall (e.base, *e.held.wall, e.held.wall->a,
                                               { e.held.wall->a.x + d.x, e.held.wall->a.y + d.y }, programme));
        case Kind::End: {
            if (!e.held.end)
                return std::nullopt;
            const fs::Vec u = EndAxis (e);
            const double t = Stepped (Dot (d, u));
            return fe::MoveEnd (e.baseDesign, *e.held.end, { e.held.end->at.x + u.x * t, e.held.end->at.y + u.y * t });
        }
        default:
            return std::nullopt;
    }
}
double PartyDistance (const Editor& e)
{
    if (e.held.kind != Kind::Party || e.held.index < 0 || e.held.index >= int (e.base.party.size ()))
        return 0;
    return Stepped (Dot (Delta (e), Outward (e.base.party[e.held.index])));
}

// The palette above the plan: flats of 1 to 5 rooms and a stair, each a square to drag onto it.
void Palette (float scale)
{
    const float side = ImGui::GetFrameHeight ();
    for (int k = 0; k <= 5; ++k) {
        const double value = k == 5 ? kStairPayload : double (k + 1);
        if (k > 0)
            ImGui::SameLine (0, 4 * scale);
        ImGui::PushID (k);
        ImGui::InvisibleButton ("##swatch", { side, side });
        const ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
        auto* draw = ImGui::GetWindowDrawList ();
        draw->AddRectFilled (a, b, value < 0 ? IM_COL32 (109, 114, 118, 255) : RoomsColour (value), 3 * scale);
        draw->AddRect (a, b, ImGui::IsItemHovered () ? kAccent : IM_COL32 (60, 60, 60, 160), 3 * scale, 0, scale);
        char label[8];
        std::snprintf (label, sizeof (label), value < 0 ? "S" : "%d", k + 1);
        const ImVec2 t = ImGui::CalcTextSize (label);
        draw->AddText ({ (a.x + b.x - t.x) / 2, (a.y + b.y - t.y) / 2 }, value < 0 ? IM_COL32_WHITE : kInk, label);
        if (ImGui::IsItemHovered () && !ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            if (value < 0)
                ImGui::SetTooltip ("Drag onto the plan: a stair there.");
            else
                ImGui::SetTooltip ("Drag onto a flat: a %d-room flat put in beside it.", k + 1);
        }
        if (ImGui::BeginDragDropSource ()) {
            ImGui::SetDragDropPayload (kPayload, &value, sizeof (value));
            if (value < 0)
                ImGui::TextUnformatted ("Stair");
            else
                ImGui::Text ("%d-room flat", k + 1);
            ImGui::EndDragDropSource ();
        }
        ImGui::PopID ();
    }
}

// ---- the canvas -----------------------------------------------------------------------------
struct View {
    ImVec2 origin, size;
    double cx = 0, cy = 0, k = 1;
    ImVec2 P (fs::Vec p) const
    {
        return { origin.x + size.x / 2 + float ((p.x - cx) * k), origin.y + size.y / 2 - float ((p.y - cy) * k) };
    }
    fs::Vec W (ImVec2 q) const
    {
        return { cx + (q.x - origin.x - size.x / 2) / k, cy - (q.y - origin.y - size.y / 2) / k };
    }
};
// The building's floor across the panel's width (user, 2026-10-10: no navigation, it fits).
View Fit (const std::vector<fs::Ring>& rings, float width, float scale)
{
    View v;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& r : rings)
        for (const auto& p : r)
            x0 = (std::min) (x0, p.x), y0 = (std::min) (y0, p.y), x1 = (std::max) (x1, p.x), y1 = (std::max) (y1, p.y);
    if (x0 > x1)
        x0 = y0 = 0, x1 = y1 = 10;
    const double margin = 1.5, pad = 8.0 * scale;
    const double dx = x1 - x0 + 2 * margin, dy = y1 - y0 + 2 * margin;
    v.k = (std::max) (0.5, (width - 2 * pad) / dx);
    const double tallest = 560.0 * scale, least = 140.0 * scale;
    if (dy * v.k + 2 * pad > tallest)
        v.k = (tallest - 2 * pad) / dy;
    v.size = { width, float (std::clamp (dy * v.k + 2 * pad, least, tallest)) };
    v.cx = (x0 + x1) / 2, v.cy = (y0 + y1) / 2;
    return v;
}
void Dashed (ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 colour, float width, float dash)
{
    const float length = std::hypot (b.x - a.x, b.y - a.y);
    if (length < 1)
        return;
    const ImVec2 d { (b.x - a.x) / length, (b.y - a.y) / length };
    for (float t = 0; t < length; t += 2 * dash) {
        const float u = (std::min) (t + dash, length);
        draw->AddLine ({ a.x + d.x * t, a.y + d.y * t }, { a.x + d.x * u, a.y + d.y * u }, colour, width);
    }
}
void Padlock (ImDrawList* draw, ImVec2 c, float s, ImU32 colour)
{
    draw->AddRectFilled ({ c.x - 4 * s, c.y - 1 * s }, { c.x + 4 * s, c.y + 5 * s }, colour, s);
    draw->PathArcTo ({ c.x, c.y - 1 * s }, 3 * s, 3.14159f, 6.28318f);
    draw->PathStroke (colour, 0, 1.5f * s);
}
void Pointer (ImDrawList* draw, const View& v, const Arrow& a, float px, ImU32 colour)
{
    const ImVec2 c = v.P (a.at);
    const ImVec2 d { float (a.dir.x), float (-a.dir.y) }, n { -d.y, d.x };
    draw->AddTriangleFilled ({ c.x + d.x * px, c.y + d.y * px },
                             { c.x - d.x * px * 0.4f + n.x * px * 0.8f, c.y - d.y * px * 0.4f + n.y * px * 0.8f },
                             { c.x - d.x * px * 0.4f - n.x * px * 0.8f, c.y - d.y * px * 0.4f - n.y * px * 0.8f },
                             colour);
}

void Canvas (Editor& e, Context& c, float scale)
{
    std::vector<fs::Ring> all = e.input.owned;
    all.insert (all.end (), e.real.begin (), e.real.end ());
    View v = Fit (all, (std::max) (120.0f, ImGui::GetContentRegionAvail ().x), scale);
    v.origin = ImGui::GetCursorScreenPos ();
    ImGui::InvisibleButton ("##plan", v.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const fs::Vec mouse = v.W (ImGui::GetIO ().MousePos);
    const double px = 1.0 / v.k; // a pixel in metres
    const auto& s = e.scheme;
    auto* draw = ImGui::GetWindowDrawList ();
    const ImVec2 end (v.origin.x + v.size.x, v.origin.y + v.size.y);

    // The palette dropped here: a flat put in beside the one under it, or a stair.
    std::optional<fe::Insertion> insertion;
    std::optional<fs::Vec> stairDrop;
    if (e.have && ImGui::BeginDragDropTarget ()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload (
                kPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const double value = *static_cast<const double*> (payload->Data);
            if (value < 0) {
                stairDrop = mouse;
                if (payload->IsDelivery () && Inside (e.input.owned, mouse))
                    Commit (e, c, fe::AddStair (s, e.input.design, mouse));
            }
            else {
                insertion = fe::Insert (s, mouse, value);
                if (payload->IsDelivery () && insertion)
                    Commit (e, c, fe::InsertFlat (s, e.input.design, mouse, value));
            }
        }
        ImGui::EndDragDropTarget ();
    }

    // Under the pointer, and the selection's arrows.
    const auto arrows = e.have ? Arrows (s, e.selected, 12 * px) : std::vector<Arrow> {};
    int arrowAt = -1;
    for (size_t i = 0; i < arrows.size (); ++i)
        if (std::hypot (arrows[i].at.x - mouse.x, arrows[i].at.y - mouse.y) < 9 * px)
            arrowAt = int (i);
    const Target hover = hovered && e.have ? HitAt (s, mouse, 6 * px) : Target {};

    // Gestures on the left button.
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && e.drag == Drag::None) {
        e.from = e.at = mouse;
        e.base = s;
        e.baseDesign = e.input.design;
        e.owner = ImGui::GetCurrentContext ();
        e.moved = false;
        e.liveSince = c.planner.Revision ();
        if (e.tool != Tool::Select)
            e.drag = Drag::Rect;
        else if (arrowAt >= 0) {
            e.held = e.selected;
            e.axis = arrows[arrowAt].dir;
            e.drag = Drag::Arrow;
        }
        else {
            e.selected = e.held = hover;
            e.selectedAt = mouse;
            e.drag = hover.kind == Kind::None ? Drag::None : Drag::Body;
        }
    }
    if (e.drag != Drag::None && e.owner == ImGui::GetCurrentContext ()) {
        e.at = mouse;
        e.moved = e.moved || std::hypot (e.at.x - e.from.x, e.at.y - e.from.y) > 4 * px;
        const bool down = ImGui::IsMouseDown (ImGuiMouseButton_Left);
        if (down && e.moved)
            if (auto next = Gesture (e, c.programme)) {
                // Live: the newest gesture to a worker, under an id of its own.
                auto in = e.input;
                in.design = std::move (*next);
                bp::Sign (in);
                c.planner.Want (bp::FloorId (e.key, e.story) + "#live", std::move (in), true);
            }
        if (!down) {
            const bool off = !Inside (e.input.owned, e.at);
            if (e.drag == Drag::Rect) {
                if (e.moved) {
                    const fs::Ring rect = fe::Rectangle (e.base, e.from, e.at, fe::kStep);
                    Commit (e, c,
                            e.tool == Tool::Cut ? fe::CutRect (e.baseDesign, rect) : fe::AddRect (e.baseDesign, rect));
                }
            }
            else if (e.held.kind == Kind::Flat && e.drag == Drag::Arrow && !e.moved) {
                // A flat's arrow clicked: it changes places with the neighbour there.
                if (const int other = Beside (e.base, e.held.index, e.axis); other >= 0)
                    Commit (e, c, fe::SwapFlats (e.base, e.baseDesign, e.held.index, other));
            }
            else if (e.held.kind == Kind::Flat && e.moved) {
                // Dropped on another flat: they change places; off the floor: it goes.
                const int other = fe::FlatAt (e.base, e.at);
                if (off) {
                    Commit (e, c, fe::RemoveFlat (e.base, e.baseDesign, e.held.index));
                    e.selected = {};
                }
                else if (other >= 0 && other != e.held.index) {
                    Commit (e, c, fe::SwapFlats (e.base, e.baseDesign, e.held.index, other));
                    e.selectedAt = e.at;
                }
            }
            else if (e.held.kind == Kind::Stair && e.moved && off && e.drag == Drag::Body) {
                Commit (e, c, fe::RemoveStair (e.base, e.baseDesign, e.held.index));
                e.selected = {};
            }
            else if (e.held.kind == Kind::Party && e.moved)
                PushParty (e, c, e.base.party[e.held.index], PartyDistance (e));
            else if (e.moved)
                if (auto next = Gesture (e, c.programme)) {
                    Commit (e, c, std::move (*next));
                    if (e.held.kind == Kind::Stair)
                        e.selectedAt = StairTo (e);
                }
            e.drag = Drag::None;
            if (e.selected.kind != Kind::Flat && e.selected.kind != Kind::Stair)
                e.selected = {}; // walls and ends move again from where they now are
        }
    }
    if (hovered && ImGui::IsKeyPressed (ImGuiKey_Escape)) {
        e.drag = Drag::None;
        e.tool = Tool::Select;
    }
    if (hovered && e.have && (ImGui::IsKeyPressed (ImGuiKey_Delete) || ImGui::IsKeyPressed (ImGuiKey_Backspace))) {
        if (e.selected.kind == Kind::Stair)
            Commit (e, c, fe::RemoveStair (s, e.input.design, e.selected.index));
        else if (e.selected.kind == Kind::Flat)
            Commit (e, c, fe::RemoveFlat (s, e.input.design, e.selected.index));
        e.selected = {};
    }
    // The right button: what can be done there.
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Right)) {
        hudshell::ClaimRightClick ();
        e.menu = hover;
        e.menuAt = mouse;
        if (hover.kind == Kind::Flat || hover.kind == Kind::Stair)
            e.selected = hover, e.selectedAt = mouse;
        ImGui::OpenPopup ("##planMenu");
    }
    // The pointer: a hand on what moves, a cross on what drags (OverlayHud: the canvas's own).
    if (hovered) {
        hudshell::OwnCursor ();
        const Kind kind = hover.kind;
        if (e.drag != Drag::None || arrowAt >= 0 || kind == Kind::Wall || kind == Kind::End || kind == Kind::Party)
            ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
        else if (kind == Kind::Flat || kind == Kind::Stair)
            ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
        else
            ImGui::SetMouseCursor (ImGuiMouseCursor_Arrow);
    }

    // ---- drawing ----
    draw->AddRectFilled (v.origin, end, ImGui::GetColorU32 (ImGuiCol_FrameBg), 4 * scale);
    draw->PushClipRect (v.origin, end, true);
    const auto fill = [&] (const std::vector<Fill>& fills) {
        for (const auto& f : fills)
            for (size_t i = 0; i + 2 < f.triangles.size (); i += 3)
                draw->AddTriangleFilled (v.P (f.triangles[i]), v.P (f.triangles[i + 1]), v.P (f.triangles[i + 2]),
                                         f.colour);
    };
    const auto ring = [&] (const fs::Ring& r, ImU32 colour, float width) {
        std::vector<ImVec2> points;
        for (const auto& p : r)
            points.push_back (v.P (p));
        draw->AddPolyline (points.data (), int (points.size ()), colour, ImDrawFlags_Closed, width);
    };
    fill (e.around); // the neighbours: context only
    // A gesture planned live shows while it is held; otherwise the scheme planned.
    const fs::Scheme* shown = &s;
    if (e.drag != Drag::None && e.moved)
        if (const auto* live = c.planner.Latest (bp::FloorId (e.key, e.story) + "#live");
            live && live->revision > e.liveSince) {
            shown = &live->scheme;
            fill (Fills (live->scheme));
        }
    if (shown == &s)
        fill (e.fills);
    const bool rooms = v.k >= 7;
    for (const auto& f : shown->flats) {
        if (rooms)
            for (const auto& r : f.roomList)
                ring (r.shape, IM_COL32 (40, 50, 55, 70), 1);
        ring (f.shape, kInk, 1.3f * scale);
    }
    for (const auto& w : shown->party)
        draw->AddLine (v.P (w[0]), v.P (w[1]), kParty, 5 * scale);
    for (const auto& r : e.real)
        for (size_t i = 0; i < r.size (); ++i)
            Dashed (draw, v.P (r[i]), v.P (r[(i + 1) % r.size ()]), kReal, 1.3f * scale, 5 * scale);
    for (const auto& p : shown->culled)
        ring (p.shape, IM_COL32 (120, 120, 120, 200), 1.2f * scale);
    // Labels where they fit, a padlock on locked flats.
    for (size_t i = 0; i < shown->flats.size (); ++i) {
        const auto& f = shown->flats[i];
        char label[48];
        std::snprintf (label, sizeof (label), "%s%s %.0f", f.manual ? "R " : "",
                       floorprogramme::Name (c.programme, f.type).c_str (), f.net);
        const auto t = ImGui::CalcTextSize (label);
        const ImVec2 at = v.P (Mid (f.shape));
        if (t.x < f.frontage * v.k * 0.95)
            draw->AddText ({ at.x - t.x / 2, at.y - t.y / 2 }, IM_COL32 (20, 25, 28, 255), label);
        if (fe::Locked (*shown, e.input.design, int (i)))
            Padlock (draw, { at.x, at.y - t.y }, scale, kInk);
    }
    for (const auto& d : shown->diagnostics)
        if (d.level == fs::Diagnostic::Error)
            draw->AddCircleFilled (v.P (d.at), 4 * scale, kRed);
    // The hovered and the selected.
    const auto outline = [&] (const Target& t, ImU32 colour, float width) {
        if (t.kind == Kind::Flat && t.index >= 0 && t.index < int (s.flats.size ()))
            ring (s.flats[t.index].shape, colour, width);
        else if (t.kind == Kind::Stair && t.index >= 0 && t.index < int (s.cores.size ()))
            ring (s.cores[t.index].shape, colour, width);
        else if (t.kind == Kind::Wall && t.wall)
            draw->AddLine (v.P (t.wall->a), v.P (t.wall->b), colour, width);
        else if (t.kind == Kind::End && t.end)
            draw->AddCircle (v.P (t.end->at), 7 * scale, colour, 0, width);
        else if (t.kind == Kind::Party && t.index >= 0 && t.index < int (s.party.size ()))
            draw->AddLine (v.P (s.party[t.index][0]), v.P (s.party[t.index][1]), colour, width + 2 * scale);
    };
    if (e.drag == Drag::None)
        outline (hover, IM_COL32 (255, 255, 255, 170), 2 * scale);
    outline (e.selected, IM_COL32 (255, 255, 255, 255), 3 * scale);
    if (e.drag == Drag::None)
        for (size_t i = 0; i < arrows.size (); ++i)
            Pointer (draw, v, arrows[i], (int (i) == arrowAt ? 9.0f : 7.0f) * scale, kAccent);
    // The gesture, drawn at once while the worker catches up.
    if (e.drag != Drag::None && e.moved) {
        const fs::Vec d = Delta (e);
        const bool off = !Inside (e.input.owned, e.at);
        if (e.drag == Drag::Rect)
            ring (fe::Rectangle (e.base, e.from, e.at, fe::kStep),
                  e.tool == Tool::Cut ? kRed : IM_COL32 (44, 122, 82, 255), 2.5f * scale);
        else if (e.held.kind == Kind::Stair && e.held.index >= 0 && e.held.index < int (e.base.cores.size ())) {
            const fs::Vec to = StairTo (e), from = e.base.cores[e.held.index].centre;
            fs::Ring moved = e.base.cores[e.held.index].shape;
            for (auto& p : moved)
                p.x += to.x - from.x, p.y += to.y - from.y;
            ring (moved, off ? kRed : kAccent, 2.5f * scale);
        }
        else if (e.held.kind == Kind::Flat && e.held.index >= 0 && e.held.index < int (e.base.flats.size ())) {
            fs::Ring moved = e.base.flats[e.held.index].shape;
            for (auto& p : moved)
                p.x += d.x, p.y += d.y;
            ring (moved, off ? kRed : kAccent, 2.5f * scale);
            if (const int other = fe::FlatAt (e.base, e.at); other >= 0 && other != e.held.index)
                ring (e.base.flats[other].shape, kAccent, 3 * scale);
        }
        else if (e.held.kind == Kind::Wall && e.held.wall) {
            const fs::Vec to = fe::SnapWall (e.base, *e.held.wall, e.held.wall->a,
                                             { e.held.wall->a.x + d.x, e.held.wall->a.y + d.y }, c.programme);
            const fs::Vec t { to.x - e.held.wall->a.x, to.y - e.held.wall->a.y };
            draw->AddLine (v.P ({ e.held.wall->a.x + t.x, e.held.wall->a.y + t.y }),
                           v.P ({ e.held.wall->b.x + t.x, e.held.wall->b.y + t.y }), kAccent, 3 * scale);
        }
        else if (e.held.kind == Kind::End && e.held.end) {
            const fs::Vec u = EndAxis (e);
            const double t = Stepped (Dot (d, u));
            draw->AddCircleFilled (v.P ({ e.held.end->at.x + u.x * t, e.held.end->at.y + u.y * t }), 6 * scale,
                                   kAccent);
        }
        else if (e.held.kind == Kind::Party && e.held.index >= 0 && e.held.index < int (e.base.party.size ())) {
            const auto& w = e.base.party[e.held.index];
            const fs::Vec n = Outward (w);
            const double t = PartyDistance (e);
            draw->AddLine (v.P ({ w[0].x + n.x * t, w[0].y + n.y * t }), v.P ({ w[1].x + n.x * t, w[1].y + n.y * t }),
                           kAccent, 5 * scale);
        }
    }
    // The palette over the plan: the new flat's wall in red across its band, the flat it makes.
    if (insertion) {
        std::vector<ImVec2> points;
        for (const auto& p : insertion->region)
            points.push_back (v.P (p));
        draw->AddConvexPolyFilled (points.data (), int (points.size ()), IM_COL32 (214, 48, 49, 50));
        draw->AddLine (v.P (insertion->a), v.P (insertion->b), kRed, 3 * scale);
    }
    if (stairDrop) {
        const fs::Vec u = WingAxis (s, *stairDrop), n { -u.y, u.x }, p = *stairDrop;
        const double a = bp::kStairWidth / 2, b = bp::kStairDepth / 2;
        ring ({ { p.x - u.x * a - n.x * b, p.y - u.y * a - n.y * b },
                { p.x + u.x * a - n.x * b, p.y + u.y * a - n.y * b },
                { p.x + u.x * a + n.x * b, p.y + u.y * a + n.y * b },
                { p.x - u.x * a + n.x * b, p.y - u.y * a + n.y * b } },
              Inside (e.input.owned, p) ? kAccent : kRed, 2.5f * scale);
    }
    if (c.planner.Pending (bp::FloorId (e.key, e.story), e.input.signature))
        draw->AddText ({ v.origin.x + 6 * scale, v.origin.y + 4 * scale }, IM_COL32 (90, 100, 105, 255), "planning...");
    draw->PopClipRect ();
    // A short note on what the pointer is over.
    if (hovered && e.drag == Drag::None && !ImGui::IsPopupOpen ("##planMenu")) {
        if (hover.kind == Kind::Flat) {
            const auto& f = s.flats[hover.index];
            ImGui::SetTooltip ("%s | net %.1f m2 | %.1f x %.1f m%s",
                               floorprogramme::Name (c.programme, f.type).c_str (), f.net, f.frontage, f.depth,
                               f.manual ? " | walls at an angle: rooms left to you" : "");
        }
        else if (hover.kind == Kind::Party)
            ImGui::SetTooltip (
                "Wall against the next building: no windows. Drag it to take its floor or give it yours.");
    }
}

} // namespace geomsrv::archviz::hudfloorscheme::detail
