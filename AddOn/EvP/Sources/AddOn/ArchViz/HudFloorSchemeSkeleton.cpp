#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

// The Plan view's Skeleton stage (HudFloorSchemeEdit.hpp): the wings the floor is planned on, drawn
// over the plan they make, and edited there. A wing is dragged, its ends and long sides are dragged
// in or out, its corridor is dragged across it -- in the canvas's 0.4 m steps, each one undo step.
// What the floor cannot take is red while it is held and said on the notice line when let go: the
// edits and the generator share one rule (floorscheme::edit), so nothing is accepted here that the
// plan then leaves out. The canvas only: the locked view keeps the flats' gestures.
namespace geomsrv::archviz::hudfloorscheme::detail {
namespace {
const ImU32 kLine = IM_COL32 (30, 40, 45, 255), kFound = IM_COL32 (47, 93, 138, 255),
            kMine = IM_COL32 (255, 186, 0, 255), kBad = IM_COL32 (214, 48, 49, 255),
            kVeil = IM_COL32 (236, 238, 232, 150), kLane = IM_COL32 (122, 72, 160, 255);
constexpr double kGripReach = 9, kLaneReach = 7; // pixels

fs::Vec Along (const fs::Wing& w)
{
    const double l = std::hypot (w.b.x - w.a.x, w.b.y - w.a.y);
    return l > 1e-9 ? fs::Vec { (w.b.x - w.a.x) / l, (w.b.y - w.a.y) / l } : fs::Vec { 1, 0 };
}
fs::Vec Left (const fs::Wing& w)
{
    const fs::Vec u = Along (w);
    return { -u.y, u.x };
}
double Dot (fs::Vec a, fs::Vec b)
{
    return a.x * b.x + a.y * b.y;
}
// Where a wing is held to change it: its two ends, then the middles of its long sides.
std::array<fs::Vec, 4> Grips (const fs::Wing& w)
{
    const fs::Vec n = Left (w), m { (w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2 };
    const double h = w.depth / 2;
    return { w.a, w.b, fs::Vec { m.x + n.x * h, m.y + n.y * h }, fs::Vec { m.x - n.x * h, m.y - n.y * h } };
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
// What of the scheme is under `m`: a grip of the selected wing, a wing's corridor, a wing.
std::pair<int, Grip> Under (const fs::Scheme& s, int selected, fs::Vec m, double px)
{
    if (Valid (s, selected)) {
        const auto grips = Grips (s.wings[selected]);
        for (int k = 0; k < 4; ++k)
            if (std::hypot (grips[k].x - m.x, grips[k].y - m.y) < kGripReach * px)
                return { selected, Grip (int (Grip::Start) + k) };
    }
    const int wing = fe::WingAt (s, m);
    if (!Valid (s, wing))
        return { -1, Grip::None };
    const auto& w = s.wings[wing];
    if (w.access == fs::Access::Centre)
        if (const auto lane = fe::CorridorOf (s, wing))
            if (std::abs (Dot ({ m.x - lane->x, m.y - lane->y }, Left (w))) < kLaneReach * px)
                return { wing, Grip::Corridor };
    return { wing, Grip::Body };
}
} // namespace

fe::Design SkeletonGesture (const Editor& e, const Context& c, std::string* why)
{
    const auto& s = e.base;
    if (!Valid (s, e.wing) || e.grip == Grip::None)
        return e.baseDesign;
    const auto& w = s.wings[e.wing];
    const fs::Vec d { e.at.x - e.from.x, e.at.y - e.from.y }, u = Along (w), n = Left (w);
    const auto grips = Grips (w);
    switch (e.grip) {
        case Grip::Body: {
            const double along = Stepped (Dot (d, u)), across = Stepped (Dot (d, n));
            return fe::MoveWing (s, e.baseDesign, e.wing, { u.x * along + n.x * across, u.y * along + n.y * across },
                                 why, c.options);
        }
        case Grip::Start:
            return fe::ResizeWing (s, e.baseDesign, e.wing, grips[0], Stepped (-Dot (d, u)), why, c.options);
        case Grip::End:
            return fe::ResizeWing (s, e.baseDesign, e.wing, grips[1], Stepped (Dot (d, u)), why, c.options);
        case Grip::Left:
            return fe::ResizeWing (s, e.baseDesign, e.wing, grips[2], Stepped (Dot (d, n)), why, c.options);
        case Grip::Right:
            return fe::ResizeWing (s, e.baseDesign, e.wing, grips[3], Stepped (-Dot (d, n)), why, c.options);
        case Grip::Corridor: {
            const auto lane = fe::CorridorOf (s, e.wing);
            if (!lane)
                return e.baseDesign;
            const double across = Stepped (Dot (d, n));
            return fe::SetCorridor (s, e.baseDesign, e.wing, { lane->x + n.x * across, lane->y + n.y * across },
                                    PinnedWidth (s, e.baseDesign, e.wing), why, c.options);
        }
        default:
            return e.baseDesign;
    }
}

namespace {
// The pointer over the canvas in the Skeleton stage: the wing selected, a gesture begun, carried on
// and let go, Delete, the right-click menu opened, the cursor. What is under the pointer.
std::pair<int, Grip> Pointer (Editor& e, Context& c, const View& v, bool hovered)
{
    const fs::Vec mouse = v.W (ImGui::GetIO ().MousePos);
    const double px = v.Pixel ();
    const auto& s = e.scheme;
    if (!Valid (s, e.wing))
        e.wing = -1;
    const auto under = hovered && e.have ? Under (s, e.wing, mouse, px) : std::pair { -1, Grip::None };
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Left) && e.drag == Drag::None) {
        e.from = e.at = mouse;
        e.base = s;
        e.baseDesign = e.input.design;
        e.owner = ImGui::GetCurrentContext ();
        e.moved = false;
        e.dragOn = Surface::Canvas;
        e.wing = under.first, e.grip = under.second;
        e.drag = e.grip == Grip::None ? Drag::None : Drag::Body;
    }
    if (e.drag != Drag::None && e.owner == ImGui::GetCurrentContext () && e.dragOn == Surface::Canvas) {
        e.at = mouse;
        e.moved = e.moved || std::hypot (e.at.x - e.from.x, e.at.y - e.from.y) > 4 * px;
        if (!ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            if (e.moved) {
                std::string why;
                auto next = SkeletonGesture (e, c, &why);
                Apply (e, c, e.baseDesign, std::move (next), why);
            }
            e.drag = Drag::None, e.grip = Grip::None;
        }
    }
    if (hovered && ImGui::IsKeyPressed (ImGuiKey_Escape))
        e.drag = Drag::None, e.grip = Grip::None;
    if (hovered && e.have && Valid (s, e.wing) && e.drag == Drag::None &&
        (ImGui::IsKeyPressed (ImGuiKey_Delete) || ImGui::IsKeyPressed (ImGuiKey_Backspace))) {
        std::string why;
        if (Apply (e, c, e.input.design, fe::RemoveWing (s, e.input.design, e.wing, &why), why))
            e.wing = -1;
    }
    if (hovered && e.have && ImGui::IsMouseClicked (ImGuiMouseButton_Right)) {
        hudshell::ClaimRightClick ();
        e.menuAt = mouse;
        e.wing = under.first;
        ImGui::OpenPopup ("##skeletonMenu");
    }
    if (hovered) {
        hudshell::OwnCursor ();
        ImGui::SetMouseCursor (e.drag != Drag::None || (under.second != Grip::None && under.second != Grip::Body)
                                   ? ImGuiMouseCursor_ResizeAll
                               : under.second == Grip::Body ? ImGuiMouseCursor_Hand
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
    const auto under = Pointer (e, c, v, hovered);
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
    // The selected wing's grips; the one under the pointer larger.
    if (Valid (s, e.wing) && e.drag == Drag::None) {
        const auto grips = Grips (s.wings[e.wing]);
        for (int k = 0; k < 4; ++k) {
            const bool hot = under.first == e.wing && int (under.second) == int (Grip::Start) + k;
            const float h = (hot ? 6.0f : 4.5f) * scale;
            const ImVec2 p = v.P (grips[k]);
            draw->AddRectFilled ({ p.x - h, p.y - h }, { p.x + h, p.y + h }, kMine);
            draw->AddRect ({ p.x - h, p.y - h }, { p.x + h, p.y + h }, kLine, 0, 0, 1.2f * scale);
        }
    }
    // The gesture, drawn at once: where the wing or its corridor would stand, red when the floor
    // cannot take it.
    std::string why;
    if (e.drag != Drag::None && e.moved && Valid (e.base, e.wing)) {
        const auto next = SkeletonGesture (e, c, &why);
        const bool refused = next == e.baseDesign;
        const auto& w = e.base.wings[e.wing];
        if (e.grip == Grip::Corridor) {
            const fs::Vec n = Left (w);
            const double across = Stepped (Dot ({ e.at.x - e.from.x, e.at.y - e.from.y }, n));
            if (const auto lane = fe::CorridorOf (e.base, e.wing)) {
                const fs::Vec u = Along (w), m { lane->x + n.x * across, lane->y + n.y * across };
                const double h = w.length / 2;
                draw->AddLine (v.P ({ m.x - u.x * h, m.y - u.y * h }), v.P ({ m.x + u.x * h, m.y + u.y * h }),
                               refused ? kBad : kMine, 3 * scale);
            }
        }
        else if (refused)
            Ring (draw, v, w.rect, kBad, 3 * scale);
        else
            for (const auto& pin : next.pins.wings)
                if (pin.id == fe::WingId (e.base, e.wing))
                    Ring (draw, v, fe::WingRect (pin), kMine, 3 * scale);
    }
    if (c.planner.Pending (bp::FloorId (e.key, e.story), e.input.signature))
        draw->AddText ({ v.origin.x + 6 * scale, v.origin.y + 4 * scale }, IM_COL32 (90, 100, 105, 255), "planning...");
    draw->PopClipRect ();

    if (!hovered || ImGui::IsPopupOpen ("##skeletonMenu"))
        return;
    if (e.drag != Drag::None && !why.empty ())
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right, why);
    else if (e.drag == Drag::None && Valid (s, under.first)) {
        const auto& w = s.wings[under.first];
        hudshell::TipAt (ImGui::GetIO ().MousePos, hudshell::TipSide::Right,
                         hudshell::Formatted ("Wing %u | %.1f x %.1f m | %s%s", fe::WingId (s, under.first), w.length,
                                              w.depth, AccessName (w.access),
                                              under.second == Grip::Corridor ? " | drag the corridor across the wing"
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
