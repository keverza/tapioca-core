#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <cfloat>
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
            kReal = IM_COL32 (47, 93, 138, 255), kDivision = IM_COL32 (122, 72, 160, 230),
            kGrid = IM_COL32 (196, 74, 64, 190); // the main walls' axes

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
ImU32 Ink (uint32_t rgba)
{
    const double brightness = 0.2126 * (rgba >> 24) + 0.7152 * ((rgba >> 16) & 0xFF) + 0.0722 * ((rgba >> 8) & 0xFF);
    return brightness < 140 ? IM_COL32 (243, 247, 248, 255) : IM_COL32 (20, 25, 28, 255);
}
std::vector<Fill> Fills (const fs::Scheme& s, const floorprogramme::Programme& programme)
{
    std::vector<Fill> fills;
    fills.push_back (Triangles (s.outline, IM_COL32 (236, 238, 232, 255)));
    for (const auto& u : s.unassigned)
        fills.push_back (Triangles ({ u.shape }, u.reason.find ("storage") != std::string::npos
                                                     ? IM_COL32 (190, 190, 182, 255)
                                                     : IM_COL32 (229, 72, 77, 150)));
    for (const auto& f : s.flats) {
        ImU32 colour = hudshell::Packed (floorprogramme::Colour (programme, f.type, f.rooms));
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
Target HitAt (const fs::Scheme& s, fs::Vec m, double px)
{
    if (const int core = fe::CoreAt (s, m); core >= 0)
        return { Kind::Stair, core, {}, {} };
    if (auto end = fe::EndAt (s, m, kEndReach * px))
        return { Kind::End, end->corridor, {}, end };
    // A division before the walls: it lies on the wall between the two zones' flats.
    if (const int division = fe::DivisionAt (s, m, kDivisionReach * px); division >= 0)
        return { Kind::Division, division, {}, {} };
    if (auto wall = fe::WallAt (s, m, kWallReach * px))
        return { Kind::Wall, -1, wall, {} };
    for (size_t i = 0; i < s.party.size (); ++i)
        if (Distance (m, s.party[i][0], s.party[i][1]) < kPartyReach * px)
            return { Kind::Party, int (i), {}, {} };
    if (const int flat = fe::FlatAt (s, m); flat >= 0)
        return { Kind::Flat, flat, {}, {} };
    return {};
}
fs::Vec DivisionMid (const fs::Scheme& s, int division)
{
    const auto& l = s.divisions[division];
    return { (l[0].x + l[1].x) / 2, (l[0].y + l[1].y) / 2 };
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
        case Kind::Division:
            if (t.index >= 0 && t.index < int (s.divisions.size ())) {
                const fs::Vec mid = DivisionMid (s, t.index);
                pair (mid, WingAxis (s, mid));
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
// Where a held division goes: along its wing, in steps.
fs::Vec DivisionTo (const Editor& e)
{
    const fs::Vec mid = DivisionMid (e.base, e.held.index), u = WingAxis (e.base, mid);
    const double t = Stepped (Dot (Delta (e), u));
    return { mid.x + u.x * t, mid.y + u.y * t };
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
// The design the held gesture makes, from the scheme it started on; why it cannot, in `why`.
std::optional<fe::Design> Gesture (const Editor& e, const floorprogramme::Programme& programme,
                                   std::string* why = nullptr)
{
    const fs::Vec d = Delta (e);
    switch (e.held.kind) {
        case Kind::Stair:
            if (e.held.index < 0 || e.held.index >= int (e.base.cores.size ()))
                return std::nullopt;
            return fe::MoveStair (e.base, e.baseDesign, e.held.index, StairTo (e), why);
        case Kind::Wall:
            if (!e.held.wall)
                return std::nullopt;
            return fe::MoveWall (e.base, e.baseDesign, *e.held.wall,
                                 fe::SnapWall (e.base, *e.held.wall, e.held.wall->a,
                                               { e.held.wall->a.x + d.x, e.held.wall->a.y + d.y }, programme),
                                 why);
        case Kind::Division:
            if (e.held.index < 0 || e.held.index >= int (e.base.divisions.size ()))
                return std::nullopt;
            return fe::MoveDivision (e.base, e.baseDesign, e.held.index, DivisionTo (e), why);
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
} // namespace geomsrv::archviz::hudfloorscheme::detail
namespace geomsrv::archviz::hudfloorscheme {
void LockedPalette (const floorprogramme::Programme& programme, float scale, float width)
{
    detail::Palette (programme, scale, true, width);
}
} // namespace geomsrv::archviz::hudfloorscheme
namespace geomsrv::archviz::hudfloorscheme::detail {
void Palette (const floorprogramme::Programme& programme, float scale, bool vertical, float side)
{
    if (side <= 0)
        side = ImGui::GetFrameHeight () * (vertical ? 1.4f : 1.0f);
    for (int k = 0; k <= 5; ++k) {
        const double value = k == 5 ? kStairPayload : double (k + 1);
        if (k > 0 && !vertical)
            ImGui::SameLine (0, 4 * scale);
        else if (k > 0)
            ImGui::Dummy ({ 0, 4 * scale });
        ImGui::PushID (k);
        ImGui::InvisibleButton ("##swatch", { side, side });
        const ImVec2 a = ImGui::GetItemRectMin (), b = ImGui::GetItemRectMax ();
        auto* draw = ImGui::GetWindowDrawList ();
        const auto colour = floorprogramme::ColourForRooms (programme, value);
        draw->AddRectFilled (a, b, value < 0 ? IM_COL32 (109, 114, 118, 255) : hudshell::Packed (colour), 3 * scale);
        draw->AddRect (a, b, ImGui::IsItemHovered () ? kAccent : IM_COL32 (60, 60, 60, 160), 3 * scale, 0, scale);
        char label[8];
        std::snprintf (label, sizeof (label), value < 0 ? "S" : "%d", k + 1);
        const ImVec2 t = ImGui::CalcTextSize (label);
        draw->AddText ({ (a.x + b.x - t.x) / 2, (a.y + b.y - t.y) / 2 }, value < 0 ? IM_COL32_WHITE : Ink (colour),
                       label);
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
// The building's floor across the panel's width (user, 2026-10-10: no navigation, it fits).
View Fit (const std::vector<fs::Ring>& rings, ImVec2 origin, float width, float scale)
{
    View v;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& r : rings)
        for (const auto& p : r)
            x0 = (std::min) (x0, p.x), y0 = (std::min) (y0, p.y), x1 = (std::max) (x1, p.x), y1 = (std::max) (y1, p.y);
    if (x0 > x1)
        x0 = y0 = 0, x1 = y1 = 10;
    // Outside dimensions need screen space even when a large floor makes 1.5 m only a few pixels.
    const double margin = 1.5, pad = 36.0 * scale + ImGui::GetTextLineHeight ();
    const double dx = x1 - x0 + 2 * margin, dy = y1 - y0 + 2 * margin;
    double k = (std::max) (1e-6, (width - 2 * pad) / dx);
    const double tallest = 560.0 * scale, least = 140.0 * scale;
    if (dy * k + 2 * pad > tallest)
        k = (tallest - 2 * pad) / dy;
    v.origin = origin;
    v.size = { width, float (std::clamp (dy * k + 2 * pad, least, tallest)) };
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    v.m[0] = k, v.m[1] = 0, v.m[2] = origin.x + v.size.x / 2 - cx * k;
    v.m[3] = 0, v.m[4] = -k, v.m[5] = origin.y + v.size.y / 2 + cy * k;
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
    const ImVec2 c = v.P (a.at), tip = v.P ({ a.at.x + a.dir.x, a.at.y + a.dir.y });
    const float l = (std::max) (1e-6f, std::hypot (tip.x - c.x, tip.y - c.y));
    const ImVec2 d { (tip.x - c.x) / l, (tip.y - c.y) / l }, n { -d.y, d.x };
    draw->AddTriangleFilled ({ c.x + d.x * px, c.y + d.y * px },
                             { c.x - d.x * px * 0.4f + n.x * px * 0.8f, c.y - d.y * px * 0.4f + n.y * px * 0.8f },
                             { c.x - d.x * px * 0.4f - n.x * px * 0.8f, c.y - d.y * px * 0.4f - n.y * px * 0.8f },
                             colour);
}
void Ring (ImDrawList* draw, const View& v, const fs::Ring& r, ImU32 colour, float width)
{
    std::vector<ImVec2> points;
    for (const auto& p : r)
        points.push_back (v.P (p));
    draw->AddPolyline (points.data (), int (points.size ()), colour, ImDrawFlags_Closed, width);
}

std::pair<std::string, std::string> FlatLabel (const fs::Flat& flat)
{
    char rooms[24], area[32];
    std::snprintf (rooms, sizeof (rooms), "%gR", flat.rooms);
    std::snprintf (area, sizeof (area), "%.1f m2", flat.net);
    return { rooms, area };
}

bool LabelFits (const fs::Ring& shape, const View& view, ImVec2 middle, ImVec2 size, float padding)
{
    if (shape.size () < 3)
        return false;
    const float x = size.x / 2 + padding, y = size.y / 2 + padding;
    cp::PathD box;
    for (const ImVec2 corner : { ImVec2 { middle.x - x, middle.y - y },
                                 { middle.x + x, middle.y - y },
                                 { middle.x + x, middle.y + y },
                                 { middle.x - x, middle.y + y } }) {
        const auto world = view.W (corner);
        box.emplace_back (world.x, world.y);
    }
    // A frontage-only check lets a horizontal caption cross a rotated or concave flat.
    return cp::Difference ({ box }, { Path (shape) }, cp::FillRule::EvenOdd, 6).empty ();
}

// Shared length of two collinear boundary segments, with tolerance for the mould's rounding.
double SharedEdge (fs::Vec a, fs::Vec b, fs::Vec c, fs::Vec d)
{
    const fs::Vec u = Unit ({ b.x - a.x, b.y - a.y }), n { -u.y, u.x };
    const fs::Vec ac { c.x - a.x, c.y - a.y }, ad { d.x - a.x, d.y - a.y };
    if (std::abs (Dot (ac, n)) > 0.01 || std::abs (Dot (ad, n)) > 0.01)
        return 0;
    const double lo = (std::min) (Dot (ac, u), Dot (ad, u)), hi = (std::max) (Dot (ac, u), Dot (ad, u));
    return (std::max) (0.0, (std::min) (std::hypot (b.x - a.x, b.y - a.y), hi) - (std::max) (0.0, lo));
}

std::optional<std::array<AlignedDimension, 2>> FlatDimensions (const fs::Scheme& scheme, int flat)
{
    if (flat < 0 || flat >= int (scheme.flats.size ()))
        return std::nullopt;
    const auto& f = scheme.flats[flat];
    if (f.shape.size () < 3 || std::abs (fs::Area (f.shape)) < 1e-8)
        return std::nullopt;
    fs::Vec u = Unit (f.axis);
    if (u.x < -1e-9 || (std::abs (u.x) <= 1e-9 && u.y < 0))
        u = { -u.x, -u.y };
    const fs::Vec bandNormal { -u.y, u.x };
    fs::Vec facade = bandNormal, edge = u;
    double best = 0;
    bool alongBand = false;
    // Prefer an exposed edge along the band over a corner's gable. A courtyard is outside too;
    // a wall against another building is not a facade. Ring winding must not change the result.
    const double winding = fs::Area (f.shape) > 0 ? 1.0 : -1.0;
    for (size_t i = 0; i < f.shape.size (); ++i) {
        const fs::Vec a = f.shape[i], b = f.shape[(i + 1) % f.shape.size ()];
        const fs::Vec dir = Unit ({ b.x - a.x, b.y - a.y });
        double shared = 0, blind = 0;
        for (const auto& ring : scheme.outline)
            for (size_t j = 0; j < ring.size (); ++j)
                shared += SharedEdge (a, b, ring[j], ring[(j + 1) % ring.size ()]);
        for (const auto& wall : scheme.party)
            blind += SharedEdge (a, b, wall[0], wall[1]);
        const double exposed = (std::max) (0.0, shared - blind);
        const double alignment = std::abs (Dot (dir, u)), score = exposed * (0.1 + alignment);
        const bool aligned = alignment >= 0.5;
        const fs::Vec outside { winding * dir.y, -winding * dir.x };
        const bool better = score > best + 1e-6 ||
                            (std::abs (score - best) <= 1e-6 && Dot (outside, bandNormal) > Dot (facade, bandNormal));
        if (exposed > 1e-6 && ((aligned && !alongBand) || (aligned == alongBand && better))) {
            best = score;
            alongBand = aligned;
            edge = dir;
            facade = outside;
        }
    }
    // End caps can have their only facade across the original band.
    if (best > 0 && std::abs (Dot (edge, u)) < 0.5)
        u = edge;
    if (u.x < -1e-9 || (std::abs (u.x) <= 1e-9 && u.y < 0))
        u = { -u.x, -u.y };
    const fs::Vec n { -u.y, u.x }, origin = f.shape.front ();
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    for (const auto& p : f.shape) {
        const fs::Vec q { p.x - origin.x, p.y - origin.y };
        u0 = (std::min) (u0, Dot (q, u)), u1 = (std::max) (u1, Dot (q, u));
        v0 = (std::min) (v0, Dot (q, n)), v1 = (std::max) (v1, Dot (q, n));
    }
    if (u1 - u0 < 1e-6 || v1 - v0 < 1e-6)
        return std::nullopt;
    const auto world = [&] (double along, double across) {
        return fs::Vec { origin.x + u.x * along + n.x * across, origin.y + u.y * along + n.y * across };
    };
    const double sign = Dot (facade, n) >= 0 ? 1.0 : -1.0, side = sign > 0 ? v1 : v0;
    return std::array<AlignedDimension, 2> { AlignedDimension {
                                                 world (u0, side), world (u1, side), { sign * n.x, sign * n.y } },
                                             AlignedDimension { world (u1, v0), world (u1, v1), u } };
}

std::string DimensionText (const AlignedDimension& dimension, const char* name)
{
    char text[64];
    std::snprintf (text, sizeof (text), "%s %.2f m", name,
                   std::hypot (dimension.b.x - dimension.a.x, dimension.b.y - dimension.a.y));
    return text;
}

void Dimension (ImDrawList* draw, const View& view, const AlignedDimension& dimension, const char* name, float scale)
{
    const ImVec2 a = view.P (dimension.a), b = view.P (dimension.b);
    const float span = std::hypot (b.x - a.x, b.y - a.y);
    if (span < 1e-3f)
        return;
    const ImVec2 d { (b.x - a.x) / span, (b.y - a.y) / span };
    ImVec2 n { -d.y, d.x };
    const ImVec2 outside = view.P ({ dimension.a.x + dimension.outward.x, dimension.a.y + dimension.outward.y });
    if ((outside.x - a.x) * n.x + (outside.y - a.y) * n.y < 0)
        n = { -n.x, -n.y };
    const auto shift = [&] (ImVec2 p, float offset) { return ImVec2 { p.x + n.x * offset, p.y + n.y * offset }; };
    const ImVec2 x = shift (a, 18 * scale), y = shift (b, 18 * scale);
    const ImU32 colour = IM_COL32 (255, 219, 112, 255), halo = IM_COL32 (26, 30, 32, 235);
    const auto line = [&] (ImVec2 from, ImVec2 to) {
        draw->AddLine (from, to, halo, 3 * scale);
        draw->AddLine (from, to, colour, scale);
    };
    line (shift (a, 3 * scale), shift (a, 23 * scale));
    line (shift (b, 3 * scale), shift (b, 23 * scale));
    line (x, y);
    for (const ImVec2 p : { x, y }) {
        const ImVec2 tick { (d.x + n.x) * 3 * scale, (d.y + n.y) * 3 * scale };
        line ({ p.x - tick.x, p.y - tick.y }, { p.x + tick.x, p.y + tick.y });
    }
    const auto text = DimensionText (dimension, name);
    const ImVec2 size = ImGui::CalcTextSize (text.c_str ());
    const ImVec2 middle = shift ({ (x.x + y.x) / 2, (x.y + y.y) / 2 }, size.y / 2 + 4 * scale);
    // ImGui lays out horizontal glyphs. Rotate just these vertices, keeping text upright on
    // either facade and at every building angle; no font, input or overlay-renderer changes.
    const float sign = d.x < -1e-6f || (std::abs (d.x) <= 1e-6f && d.y > 0) ? -1.0f : 1.0f;
    const ImVec2 along { sign * d.x, sign * d.y }, across { -along.y, along.x };
    const auto rotate = [&] (float px, float py) {
        return ImVec2 { middle.x + along.x * px + across.x * py, middle.y + along.y * px + across.y * py };
    };
    const float halfWidth = size.x / 2 + 3 * scale, halfHeight = size.y / 2 + 2 * scale;
    const ImVec2 box[] = { rotate (-halfWidth, -halfHeight), rotate (halfWidth, -halfHeight),
                           rotate (halfWidth, halfHeight), rotate (-halfWidth, halfHeight) };
    draw->AddConvexPolyFilled (box, 4, halo);
    const int first = draw->VtxBuffer.Size;
    // Lay out at the clip centre: clipping a horizontal source caption at its final anchor
    // would discard glyphs that fit after rotation (especially a vertical facade at an edge).
    const ImVec2 clipLo = draw->GetClipRectMin (), clipHi = draw->GetClipRectMax ();
    const ImVec2 source { (clipLo.x + clipHi.x) / 2, (clipLo.y + clipHi.y) / 2 };
    draw->AddText ({ source.x - size.x / 2, source.y - size.y / 2 }, colour, text.c_str ());
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        auto& p = draw->VtxBuffer[i].pos;
        p = rotate (p.x - source.x, p.y - source.y);
    }
}

Pointing Interact (Editor& e, Context& c, const View& v, bool hovered, Surface on)
{
    Pointing at;
    const fs::Vec mouse = v.W (ImGui::GetIO ().MousePos);
    const double px = v.Pixel ();
    const auto& s = e.scheme;
    // Under the pointer, and the selection's arrows.
    if (e.have)
        at.arrows = Arrows (s, e.selected, 12 * px);
    for (size_t i = 0; i < at.arrows.size (); ++i)
        if (std::hypot (at.arrows[i].at.x - mouse.x, at.arrows[i].at.y - mouse.y) < 9 * px)
            at.arrow = int (i);
    if (hovered && e.have)
        at.hover = HitAt (s, mouse, px);
    const auto& hover = at.hover;

    // Gestures on the left button.
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && e.drag == Drag::None) {
        e.from = e.at = mouse;
        e.base = s;
        e.baseDesign = e.input.design;
        e.owner = ImGui::GetCurrentContext ();
        e.moved = false;
        e.liveSince = c.planner.Revision ();
        e.dragOn = on;
        if (at.arrow >= 0) {
            e.held = e.selected;
            e.axis = at.arrows[at.arrow].dir;
            e.drag = Drag::Arrow;
        }
        else {
            e.selected = e.held = hover;
            e.selectedAt = mouse;
            e.drag = hover.kind == Kind::None ? Drag::None : Drag::Body;
        }
    }
    // Carried on only on the surface it began on: over the other (the panel's canvas while it was
    // begun on the view) its transform would read the pointer elsewhere and draw a second outline.
    if (e.drag != Drag::None && e.owner == ImGui::GetCurrentContext () && e.dragOn == on) {
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
            std::string why;
            if (e.held.kind == Kind::Flat && e.drag == Drag::Arrow && !e.moved) {
                // A flat's arrow clicked: it changes places with the neighbour there.
                if (const int other = Beside (e.base, e.held.index, e.axis); other >= 0)
                    Apply (e, c, e.baseDesign, fe::SwapFlats (e.base, e.baseDesign, e.held.index, other, &why), why);
            }
            else if (e.held.kind == Kind::Flat && e.moved) {
                // Dropped on another flat: they change places; off the floor: it goes.
                const int other = fe::FlatAt (e.base, e.at);
                if (off) {
                    if (Apply (e, c, e.baseDesign,
                               fe::RemoveFlat (e.base, e.baseDesign, e.held.index, &why,
                                               fs::MaxFlat (c.programme, c.options)),
                               why))
                        e.selected = {};
                }
                else if (other >= 0 && other != e.held.index) {
                    if (Apply (e, c, e.baseDesign, fe::SwapFlats (e.base, e.baseDesign, e.held.index, other, &why),
                               why))
                        e.selectedAt = e.at;
                }
                else if (other < 0)
                    Notice (e, "Drop the flat on another flat to change places, or off the floor to remove it.");
            }
            else if (e.held.kind == Kind::Stair && e.moved && off && e.drag == Drag::Body) {
                if (Apply (e, c, e.baseDesign, fe::RemoveStair (e.base, e.baseDesign, e.held.index, &why), why))
                    e.selected = {};
            }
            else if (e.held.kind == Kind::Party && e.moved)
                PushParty (e, c, e.base.party[e.held.index], PartyDistance (e));
            else if (e.moved)
                if (auto next = Gesture (e, c.programme, &why))
                    if (Apply (e, c, e.baseDesign, std::move (*next), why) && e.held.kind == Kind::Stair)
                        e.selectedAt = StairTo (e);
            e.drag = Drag::None;
            if (e.selected.kind != Kind::Flat && e.selected.kind != Kind::Stair)
                e.selected = {}; // walls and ends move again from where they now are
        }
    }
    if (hovered && ImGui::IsKeyPressed (ImGuiKey_Escape))
        e.drag = Drag::None;
    if (hovered && e.have && (ImGui::IsKeyPressed (ImGuiKey_Delete) || ImGui::IsKeyPressed (ImGuiKey_Backspace))) {
        std::string why;
        bool removed = false;
        if (e.selected.kind == Kind::Stair)
            removed = Apply (e, c, e.input.design, fe::RemoveStair (s, e.input.design, e.selected.index, &why), why);
        else if (e.selected.kind == Kind::Flat)
            removed = Apply (
                e, c, e.input.design,
                fe::RemoveFlat (s, e.input.design, e.selected.index, &why, fs::MaxFlat (c.programme, c.options)), why);
        if (removed)
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
        if (e.drag != Drag::None || at.arrow >= 0 || kind == Kind::Wall || kind == Kind::End || kind == Kind::Party ||
            kind == Kind::Division)
            ImGui::SetMouseCursor (ImGuiMouseCursor_ResizeAll);
        else if (kind == Kind::Flat || kind == Kind::Stair)
            ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
        else
            ImGui::SetMouseCursor (ImGuiMouseCursor_Arrow);
    }
    return at;
}

// Where a dragged flat or stair was, ghostly: a faint fill and a dashed outline (user, 2026-10-14).
void Ghost (ImDrawList* draw, const View& v, const fs::Ring& ring, float scale)
{
    const Fill fill = Triangles ({ ring }, kGhostFill);
    for (size_t i = 0; i + 2 < fill.triangles.size (); i += 3)
        draw->AddTriangleFilled (v.P (fill.triangles[i]), v.P (fill.triangles[i + 1]), v.P (fill.triangles[i + 2]),
                                 kGhostFill);
    for (size_t i = 0; i < ring.size (); ++i)
        Dashed (draw, v.P (ring[i]), v.P (ring[(i + 1) % ring.size ()]), kGhostLine, 1.5f * scale, 5 * scale);
}
void Marks (const Editor& e, Context& c, const View& v, ImDrawList* draw, float scale, const Pointing& at, Surface on)
{
    const auto& s = e.scheme;
    // The hovered and the selected.
    const auto outline = [&] (const Target& t, ImU32 colour, float width) {
        if (t.kind == Kind::Flat && t.index >= 0 && t.index < int (s.flats.size ()))
            Ring (draw, v, s.flats[t.index].shape, colour, width);
        else if (t.kind == Kind::Stair && t.index >= 0 && t.index < int (s.cores.size ()))
            Ring (draw, v, s.cores[t.index].shape, colour, width);
        else if (t.kind == Kind::Wall && t.wall)
            draw->AddLine (v.P (t.wall->a), v.P (t.wall->b), colour, width);
        else if (t.kind == Kind::End && t.end)
            draw->AddCircle (v.P (t.end->at), 7 * scale, colour, 0, width);
        else if (t.kind == Kind::Party && t.index >= 0 && t.index < int (s.party.size ()))
            draw->AddLine (v.P (s.party[t.index][0]), v.P (s.party[t.index][1]), colour, width + 2 * scale);
        else if (t.kind == Kind::Division && t.index >= 0 && t.index < int (s.divisions.size ()))
            draw->AddLine (v.P (s.divisions[t.index][0]), v.P (s.divisions[t.index][1]), colour, width);
    };
    // A flat or stair being dragged leaves a ghost where it was; anything else selected stays solid.
    const bool mine = e.drag != Drag::None && e.dragOn == on; // the gesture is this surface's
    const bool dragged = mine && e.moved && (e.held.kind == Kind::Flat || e.held.kind == Kind::Stair);
    if (e.drag == Drag::None)
        outline (at.hover, IM_COL32 (255, 255, 255, 170), 2 * scale);
    if (dragged && e.held.kind == Kind::Flat && e.held.index >= 0 && e.held.index < int (e.base.flats.size ()))
        Ghost (draw, v, e.base.flats[e.held.index].shape, scale);
    else if (dragged && e.held.kind == Kind::Stair && e.held.index >= 0 && e.held.index < int (e.base.cores.size ()))
        Ghost (draw, v, e.base.cores[e.held.index].shape, scale);
    else if (on == Surface::Canvas || e.selected.kind != Kind::Flat)
        outline (e.selected, IM_COL32 (255, 255, 255, 255), 3 * scale);
    if (e.drag == Drag::None)
        for (size_t i = 0; i < at.arrows.size (); ++i)
            Pointer (draw, v, at.arrows[i], (int (i) == at.arrow ? 9.0f : 7.0f) * scale, kAccent);
    // The gesture, drawn at once while the worker catches up.
    if (!mine || !e.moved)
        return;
    const fs::Vec d = Delta (e);
    const bool off = !Inside (e.input.owned, e.at);
    if (e.held.kind == Kind::Stair && e.held.index >= 0 && e.held.index < int (e.base.cores.size ())) {
        const fs::Vec to = StairTo (e), from = e.base.cores[e.held.index].centre;
        fs::Ring moved = e.base.cores[e.held.index].shape;
        for (auto& p : moved)
            p.x += to.x - from.x, p.y += to.y - from.y;
        Ring (draw, v, moved, off ? kRed : kAccent, 2.5f * scale);
    }
    else if (e.held.kind == Kind::Flat && e.held.index >= 0 && e.held.index < int (e.base.flats.size ())) {
        fs::Ring moved = e.base.flats[e.held.index].shape;
        for (auto& p : moved)
            p.x += d.x, p.y += d.y;
        Ring (draw, v, moved, off ? kRed : kAccent, 2.5f * scale);
        if (const int other = fe::FlatAt (e.base, e.at); other >= 0 && other != e.held.index)
            Ring (draw, v, e.base.flats[other].shape, kAccent, 3 * scale);
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
        draw->AddCircleFilled (v.P ({ e.held.end->at.x + u.x * t, e.held.end->at.y + u.y * t }), 6 * scale, kAccent);
    }
    else if (e.held.kind == Kind::Division && e.held.index >= 0 && e.held.index < int (e.base.divisions.size ())) {
        const fs::Vec mid = DivisionMid (e.base, e.held.index), to = DivisionTo (e);
        const auto& l = e.base.divisions[e.held.index];
        draw->AddLine (v.P ({ l[0].x + to.x - mid.x, l[0].y + to.y - mid.y }),
                       v.P ({ l[1].x + to.x - mid.x, l[1].y + to.y - mid.y }), kAccent, 3 * scale);
    }
    else if (e.held.kind == Kind::Party && e.held.index >= 0 && e.held.index < int (e.base.party.size ())) {
        const auto& w = e.base.party[e.held.index];
        const fs::Vec n = Outward (w);
        const double t = PartyDistance (e);
        draw->AddLine (v.P ({ w[0].x + n.x * t, w[0].y + n.y * t }), v.P ({ w[1].x + n.x * t, w[1].y + n.y * t }),
                       kAccent, 5 * scale);
    }
}

void Dropped (Editor& e, Context& c, fs::Vec at, std::optional<fe::Insertion>& insertion,
              std::optional<fs::Vec>& stairDrop)
{
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload (
        kPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if (!payload)
        return;
    const auto& s = e.scheme;
    const double value = *static_cast<const double*> (payload->Data);
    std::string why;
    if (value < 0) {
        stairDrop = at;
        if (payload->IsDelivery ()) {
            if (Inside (e.input.owned, at))
                Apply (e, c, e.input.design, fe::AddStair (s, e.input.design, at, &why), why);
            else
                Notice (e, "A stair goes inside the floor: drop it on the plan.");
        }
    }
    else {
        insertion = fe::Insert (s, at, value, &why);
        if (payload->IsDelivery ()) {
            if (insertion)
                Apply (e, c, e.input.design, fe::InsertFlat (s, e.input.design, at, value, &why), why);
            else
                Notice (e, why);
        }
    }
}
void Canvas (Editor& e, Context& c, float scale)
{
    std::vector<fs::Ring> all = e.input.owned;
    all.insert (all.end (), e.real.begin (), e.real.end ());
    const View v =
        Fit (all, ImGui::GetCursorScreenPos (), (std::max) (120.0f, ImGui::GetContentRegionAvail ().x), scale);
    ImGui::InvisibleButton ("##plan", v.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const fs::Vec mouse = v.W (ImGui::GetIO ().MousePos);
    const auto& s = e.scheme;
    auto* draw = ImGui::GetWindowDrawList ();
    const ImVec2 end (v.origin.x + v.size.x, v.origin.y + v.size.y);

    // The palette dropped here: a flat put in beside the one under it, or a stair.
    std::optional<fe::Insertion> insertion;
    std::optional<fs::Vec> stairDrop;
    if (e.have && ImGui::BeginDragDropTarget ()) {
        Dropped (e, c, mouse, insertion, stairDrop);
        ImGui::EndDragDropTarget ();
    }
    const Pointing at = Interact (e, c, v, hovered);

    // ---- drawing ----
    draw->AddRectFilled (v.origin, end, ImGui::GetColorU32 (ImGuiCol_FrameBg), 4 * scale);
    draw->PushClipRect (v.origin, end, true);
    const auto fill = [&] (const std::vector<Fill>& fills) {
        for (const auto& f : fills)
            for (size_t i = 0; i + 2 < f.triangles.size (); i += 3)
                draw->AddTriangleFilled (v.P (f.triangles[i]), v.P (f.triangles[i + 1]), v.P (f.triangles[i + 2]),
                                         f.colour);
    };
    fill (e.around); // the neighbours: context only
    // A gesture planned live shows while it is held; otherwise the scheme planned.
    const fs::Scheme* shown = &s;
    if (e.drag != Drag::None && e.moved)
        if (const auto* live = c.planner.Latest (bp::FloorId (e.key, e.story) + "#live");
            live && live->revision > e.liveSince) {
            shown = &live->scheme;
            fill (Fills (live->scheme, c.programme));
        }
    // A scheme just planned: the flats flow to it -- the floor and what is unassigned under them,
    // the circulation over them -- and settle before their outlines, rooms and labels show.
    const double now = Now ();
    const bool flowing = shown == &s && Flowing (e, now);
    if (shown == &s && !flowing)
        fill (e.fills);
    else if (flowing) {
        const size_t flats0 = (std::min) (e.fills.size (), 1 + s.unassigned.size ()),
                     flats1 = (std::min) (e.fills.size (), flats0 + s.flats.size ());
        fill ({ e.fills.begin (), e.fills.begin () + static_cast<std::ptrdiff_t> (flats0) });
        const double t = (now - e.morphStart) / kMorphSeconds;
        for (const auto& m : e.morphs) {
            std::vector<ImVec2> points;
            for (const auto& p : Between (m, t))
                points.push_back (v.P (p));
            if (points.size () < 3)
                continue;
            draw->AddConcavePolyFilled (points.data (), int (points.size ()), m.colour);
            draw->AddPolyline (points.data (), int (points.size ()), kInk, ImDrawFlags_Closed, 1.3f * scale);
        }
        fill ({ e.fills.begin () + static_cast<std::ptrdiff_t> (flats1), e.fills.end () });
    }
    const double px = v.Pixel ();
    const bool rooms = px <= 1.0 / 7;
    for (const auto& f : shown->flats) {
        if (flowing)
            break;
        if (rooms)
            for (const auto& r : f.roomList)
                Ring (draw, v, r.shape, IM_COL32 (40, 50, 55, 70), 1);
        Ring (draw, v, f.shape, kInk, 1.3f * scale);
    }
    for (const auto& w : shown->party)
        draw->AddLine (v.P (w[0]), v.P (w[1]), kParty, 5 * scale);
    // The building's main walls (Scheme::grid): a thick mark outside each facade where one meets
    // it, numbered in a bubble well clear of the building -- never a line across the small plan
    // (user, 2026-10-15: "Do not show full grid lines in miniGUI ... only thicker line segments
    // outside building. Grid numbers should have larger offset from building").
    for (size_t i = 0; i < shown->grid.size (); ++i) {
        const ImVec2 a = v.P (shown->grid[i][0]), b = v.P (shown->grid[i][1]);
        const float len = std::hypot (b.x - a.x, b.y - a.y);
        if (len < 1)
            continue;
        const ImVec2 u { (b.x - a.x) / len, (b.y - a.y) / len };
        const auto out = [&] (ImVec2 p, float sign, float d) {
            return ImVec2 (p.x + sign * u.x * d * scale, p.y + sign * u.y * d * scale);
        };
        draw->AddLine (out (a, -1, 4), out (a, -1, 16), kGrid, 3 * scale);
        draw->AddLine (out (b, 1, 4), out (b, 1, 16), kGrid, 3 * scale);
        char label[8];
        std::snprintf (label, sizeof (label), "%zu", i + 1);
        const float r = 7 * scale;
        const ImVec2 at = out (b, 1, 16 + 7 + 6);
        draw->AddCircleFilled (at, r, IM_COL32_WHITE);
        draw->AddCircle (at, r, kGrid, 0, scale);
        const float size = ImGui::GetFontSize () * 0.8f;
        const ImVec2 t = ImGui::GetFont ()->CalcTextSizeA (size, FLT_MAX, 0, label);
        draw->AddText (nullptr, size, { at.x - t.x / 2, at.y - t.y / 2 }, kGrid, label);
    }
    // The typology's blocks: where a wing's stair zones meet, dashed (user, 2026-10-14: "Show
    // typology block division lines ... to manually move or edit how big the stair core zone
    // has to be"). Dragged like a wall.
    for (const auto& l : shown->divisions)
        Dashed (draw, v.P (l[0]), v.P (l[1]), kDivision, 2 * scale, 7 * scale);
    for (const auto& r : e.real)
        for (size_t i = 0; i < r.size (); ++i)
            Dashed (draw, v.P (r[i]), v.P (r[(i + 1) % r.size ()]), kReal, 1.3f * scale, 5 * scale);
    for (const auto& p : shown->culled)
        Ring (draw, v, p.shape, IM_COL32 (120, 120, 120, 200), 1.2f * scale);
    // Labels where they fit, a padlock on locked flats: once the flats have settled.
    for (size_t i = 0; i < shown->flats.size () && !flowing; ++i) {
        const auto& f = shown->flats[i];
        const ImU32 ink = Ink (floorprogramme::Colour (c.programme, f.type, f.rooms));
        const auto [roomText, areaText] = FlatLabel (f);
        const auto roomSize = ImGui::CalcTextSize (roomText.c_str ());
        const auto areaSize = ImGui::CalcTextSize (areaText.c_str ());
        const float line = ImGui::GetTextLineHeight ();
        const ImVec2 t { (std::max) (roomSize.x, areaSize.x), 2 * line };
        const ImVec2 middle = v.P (Mid (f.shape));
        if (LabelFits (f.shape, v, middle, t, 2 * scale)) {
            draw->AddText ({ middle.x - roomSize.x / 2, middle.y - line }, ink, roomText.c_str ());
            draw->AddText ({ middle.x - areaSize.x / 2, middle.y }, ink, areaText.c_str ());
        }
        if (fe::Locked (*shown, e.input.design, int (i)))
            Padlock (draw, { middle.x, middle.y - line - 6 * scale }, scale, ink);
    }
    for (const auto& d : shown->diagnostics)
        if (d.level == fs::Diagnostic::Error)
            draw->AddCircleFilled (v.P (d.at), 4 * scale, kRed);
    Marks (e, c, v, draw, scale, at);
    const Target selected = shown == &s ? e.selected : Again (*shown, e.selected, e.selectedAt);
    if (selected.kind == Kind::Flat)
        if (const auto dimensions = FlatDimensions (*shown, selected.index)) {
            Dimension (draw, v, (*dimensions)[0], "L", scale);
            Dimension (draw, v, (*dimensions)[1], "D", scale);
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
        Ring (draw, v,
              { { p.x - u.x * a - n.x * b, p.y - u.y * a - n.y * b },
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
        if (at.hover.kind == Kind::Flat) {
            const auto& f = s.flats[at.hover.index];
            ImGui::SetTooltip ("%s | net %.1f m2 | %.1f x %.1f m%s",
                               floorprogramme::Name (c.programme, f.type).c_str (), f.net, f.frontage, f.depth,
                               f.manual ? " | walls at an angle: rooms left to you" : "");
        }
        else if (at.hover.kind == Kind::Party)
            ImGui::SetTooltip (
                "Wall against the next building: no windows. Drag it to take its floor or give it yours.");
    }
}

} // namespace geomsrv::archviz::hudfloorscheme::detail
