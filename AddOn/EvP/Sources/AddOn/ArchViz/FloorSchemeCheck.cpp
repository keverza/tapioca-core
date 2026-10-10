#include "ArchViz/FloorSchemeDetail.hpp"
#include <algorithm>

// The hard rules on a finished scheme: corridors, stairs, flats, rooms and the floor's
// accounting. Each failure is an Error diagnostic at the place it happens.
namespace geomsrv::archviz::floorscheme {
using namespace detail;

size_t Check (Scheme& s, const Options& o)
{
    const auto outline = ToPaths (s.outline);
    const auto blind = ToPaths (o.party);
    size_t errors = 0;
    auto fail = [&] (const char* code, const std::string& text, Vec at) {
        Note (s, Diagnostic::Error, code, text, at);
        ++errors;
    };
    // Corridors: straight or one L, never crossing each other or a stair.
    for (size_t i = 0; i < s.corridors.size (); ++i) {
        const auto& c = s.corridors[i];
        if (c.axis.size () > 3)
            fail ("corridor.turns", "A corridor turns more than once.", c.axis.front ());
        for (size_t j = i + 1; j < s.corridors.size (); ++j)
            if (Overlap (c.shape, s.corridors[j].shape) > 0.01 || Touch (c.shape, s.corridors[j].shape) > 0.3)
                fail ("corridor.branch", "Two corridors meet: corridors must not branch or cross.", Centroid (c.shape));
        for (const auto& core : s.cores)
            if (Overlap (c.shape, core.shape) > 0.01)
                fail ("corridor.over_stair", "A corridor runs over a stair.", core.centre);
        // One flat (or the stair) owns each corridor end: no wall in the middle of it.
        for (int end = 0; end < 2 && c.axis.size () >= 2; ++end) {
            const Vec p = end ? c.axis.back () : c.axis.front ();
            const Vec q = end ? c.axis[c.axis.size () - 2] : c.axis[1];
            const Vec d = Unit ({ p.x - q.x, p.y - q.y }), n { -d.y, d.x };
            std::vector<int> owners;
            bool open = false;
            for (int k = -2; k <= 2; ++k) {
                const double t = k * (o.corridor / 2 - 0.1) / 2;
                const Vec at { p.x + d.x * 0.1 + n.x * t, p.y + d.y * 0.1 + n.y * t };
                int owner = -1;
                for (size_t f = 0; f < s.flats.size () && owner < 0; ++f)
                    if (Inside ({ ToPath (s.flats[f].shape) }, at))
                        owner = static_cast<int> (f);
                for (size_t k2 = 0; k2 < s.cores.size () && owner < 0; ++k2)
                    if (Inside ({ ToPath (s.cores[k2].shape) }, at))
                        owner = -2 - static_cast<int> (k2);
                for (size_t k2 = 0; k2 < s.unassigned.size () && owner == -1; ++k2)
                    if (s.unassigned[k2].reason.find ("storage") != std::string::npos &&
                        Inside ({ ToPath (s.unassigned[k2].shape) }, at))
                        owner = -1000 - static_cast<int> (k2);
                if (owner == -1)
                    open = open || Inside (outline, at);
                else if (std::find (owners.begin (), owners.end (), owner) == owners.end ())
                    owners.push_back (owner);
            }
            if (owners.size () > 1)
                fail ("corridor.end_wall", "A wall meets the middle of a corridor end.", p);
            if (open)
                fail ("corridor.open_end", "A corridor ends in floor no flat or stair owns.", p);
        }
    }
    // Stairs need a window: in the stair itself or in the corridor right beside it.
    for (auto& core : s.cores) {
        core.window = FacadeOf (outline, core.shape, &blind);
        if (core.window >= o.stairWindow - 0.05)
            continue;
        double beside = 0;
        for (const auto& c : s.corridors)
            if (Touch (core.shape, c.shape) > 0.5)
                beside = (std::max) (beside, FacadeOf (outline, c.shape, &blind));
        for (const auto& l : s.lobbies)
            if (Touch (core.shape, l) > 0.5)
                beside = (std::max) (beside, FacadeOf (outline, l, &blind));
        if (beside < o.stairWindow - 0.05)
            fail ("stair.no_facade", "A stair has no window, nor does the corridor beside it.", core.centre);
    }
    // Flats: a facade, a door, no corridor inside, rooms no narrower than the minimum.
    for (const auto& f : s.flats) {
        const Vec at = Centroid (f.shape);
        if (FacadeOf (outline, f.shape, &blind) < kBedroomMin)
            fail ("flat.no_facade", "A flat has no facade wide enough for a room.", at);
        double door = 0;
        for (const auto& c : s.corridors)
            door += Touch (f.shape, c.shape);
        for (const auto& c : s.cores)
            door += Touch (f.shape, c.shape);
        for (const auto& l : s.lobbies)
            door += Touch (f.shape, l);
        if (door < 0.9)
            fail ("flat.no_door", "A flat does not reach a corridor or a stair landing.", at);
        // One stair's circulation per flat: between two stairs at least two flats, never
        // stair, flat, stair (user, 2026-10-10). A lobby belongs to the stair it touches; a
        // contact shorter than a door (a corner brushing a stair) does not count.
        {
            std::vector<int> reached;
            auto reach = [&] (const Ring& ring, int section) {
                if (section >= 0 && Near (f.shape, ring, 0.1) && Touch (f.shape, ring) >= 1.5 &&
                    std::find (reached.begin (), reached.end (), section) == reached.end ())
                    reached.push_back (section);
            };
            for (const auto& c : s.corridors)
                reach (c.shape, c.section);
            for (const auto& c : s.cores)
                reach (c.shape, c.section);
            for (const auto& l : s.lobbies) {
                int section = -1;
                for (const auto& c : s.cores)
                    if (section < 0 && Near (l, c.shape, 0.1) && Touch (l, c.shape) > 0.5)
                        section = c.section;
                for (const auto& c : s.corridors)
                    if (section < 0 && Near (l, c.shape, 0.1) && Touch (l, c.shape) > 0.5)
                        section = c.section;
                reach (l, section);
            }
            if (reached.size () > 1)
                fail ("flat.two_stairs", "A flat touches the circulation of two stairs.", at);
        }
        for (const auto& c : s.corridors)
            if (Overlap (f.shape, c.shape) > 0.01)
                fail ("flat.over_corridor", "A flat crosses a corridor.", at);
        // The hall is the entrance: it meets the corridor, landing or lobby.
        bool hallDoor = false, hasHall = false;
        for (const auto& r : f.roomList) {
            if (r.kind != RoomKind::Hall)
                continue;
            hasHall = true;
            double reach = 0;
            // Rooms are drawn inside their walls: the circulation grows by a wall's thickness.
            auto wall = [] (const Ring& ring) {
                const auto grown = cp::InflatePaths ({ ToPath (ring) }, kPartyWall / 2 + 0.01, cp::JoinType::Miter,
                                                     cp::EndType::Polygon, 2.0, kPrecision);
                return grown.empty () ? ring : FromPath (grown.front ());
            };
            for (const auto& c : s.corridors)
                reach += Touch (r.shape, wall (c.shape));
            for (const auto& c : s.cores)
                reach += Touch (r.shape, wall (c.shape));
            for (const auto& l : s.lobbies)
                reach += Touch (r.shape, wall (l));
            hallDoor = hallDoor || reach >= 0.9;
        }
        if (hasHall && !hallDoor)
            fail ("hall.no_door", "A flat's hall does not meet the corridor or landing.", at);
        // A 1.5R needs 1.6 m of wall between its living-room and alcove windows.
        bool alcove = false;
        for (const auto& r : f.roomList)
            alcove = alcove || r.kind == RoomKind::Alcove;
        if (alcove)
            for (size_t i = 0; i < f.windows.size (); ++i)
                for (size_t j = 0; j < f.windows.size (); ++j) {
                    const auto& a = f.windows[i];
                    const auto& b = f.windows[j];
                    if (a.kind != RoomKind::Living || b.kind != RoomKind::Alcove)
                        continue;
                    double gap = 1e18;
                    for (const Vec p : { a.a, a.b })
                        for (const Vec q : { b.a, b.b })
                            gap = (std::min) (gap, std::hypot (p.x - q.x, p.y - q.y));
                    if (gap < o.windowGap - 1e-6)
                        fail ("window.gap", "A 1.5R's two windows are closer than 1.6 m.", at);
                }
        for (const auto& r : f.roomList)
            if (!Inside ({ ToPath (f.shape) }, Centroid (r.shape)))
                fail ("room.outside", "A room lies outside its flat.", Centroid (r.shape));
        for (const auto& r : f.roomList) {
            const double width = (std::min) (r.width, r.depth);
            const double min = r.kind == RoomKind::Living    ? kLivingMin
                               : r.kind == RoomKind::Bedroom ? kBedroomMin
                               : r.kind == RoomKind::Wc      ? kWcMin
                               : r.kind == RoomKind::Bath    ? 1.5
                               : r.kind == RoomKind::Hall    ? kHallMin
                               : r.kind == RoomKind::Storage ? kStorageMin
                                                             : 0.0;
            if (width < min - 1e-6)
                fail ("room.narrow", "A room is narrower than its minimum width.", Centroid (r.shape));
        }
    }
    for (const auto& u : s.unassigned)
        if (std::abs (Area (u.shape)) >= 2.0)
            Note (s, Diagnostic::Warning, "floor.unassigned", u.reason, Centroid (u.shape));
    // Every square metre is a flat, circulation or reported unassigned, and only one of them.
    {
        cp::PathsD all;
        std::vector<std::pair<const Ring*, const char*>> parts;
        double sum = 0;
        auto add = [&] (const Ring& r, const char* what) {
            all.push_back (ToPath (r));
            parts.push_back ({ &r, what });
            sum += std::abs (Area (r));
        };
        for (const auto& f : s.flats)
            add (f.shape, "flat");
        for (const auto& c : s.corridors)
            add (c.shape, "corridor");
        for (const auto& c : s.cores)
            add (c.shape, "stair");
        for (const auto& l : s.lobbies)
            add (l, "lobby");
        for (const auto& u : s.unassigned)
            add (u.shape, "unassigned piece");
        const auto joined = cp::Union (all, cp::FillRule::NonZero, kPrecision);
        const double lost =
            std::abs (detail::Area (cp::Difference (outline, joined, cp::FillRule::NonZero, kPrecision)));
        const double twice = sum - std::abs (detail::Area (joined));
        if (lost > 1.0)
            for (const auto& path : cp::Difference (outline, joined, cp::FillRule::NonZero, kPrecision))
                if (cp::Area (path) > 0.5)
                    fail ("floor.lost", "Floor no flat, circulation or report accounts for.",
                          Centroid (FromPath (path)));
        if (twice > 1.0)
            for (size_t i = 0; i < parts.size (); ++i)
                for (size_t k = i + 1; k < parts.size (); ++k)
                    if (Near (*parts[i].first, *parts[k].first, 0) && Overlap (*parts[i].first, *parts[k].first) > 0.5)
                        fail ("floor.twice",
                              std::string ("A ") + parts[i].second + " overlaps a " + parts[k].second + ".",
                              Centroid (*parts[k].first));
    }
    s.ok = true;
    for (const auto& d : s.diagnostics)
        s.ok = s.ok && d.level != Diagnostic::Error;
    return errors;
}

namespace {
// A stack stair's footprint turned to the planned stair `core`: its width along the core's first
// side, or across it when that side is the stair's depth.
Ring Footprint (const Pins::Core& pin, const Core& core, const Options& o)
{
    const double w = pin.width > 0 ? pin.width : o.coreWidth, d = pin.depth > 0 ? pin.depth : o.coreDepth;
    Vec u { 1, 0 };
    double side = w;
    if (core.shape.size () >= 2) {
        const Vec a = core.shape[0], b = core.shape[1];
        u = Unit ({ b.x - a.x, b.y - a.y });
        side = std::hypot (b.x - a.x, b.y - a.y);
    }
    const bool across = std::abs (side - d) < std::abs (side - w);
    const Vec v { -u.y, u.x };
    const double hu = (across ? d : w) / 2, hv = (across ? w : d) / 2;
    const Vec c = pin.centre;
    auto at = [&] (double s, double t) { return Vec { c.x + u.x * s + v.x * t, c.y + u.y * s + v.y * t }; };
    return Counter ({ at (-hu, -hv), at (hu, -hv), at (hu, hv), at (-hu, hv) });
}
} // namespace

size_t CheckStack (Scheme& s, const std::vector<Pins::Core>& stack, const Options& o)
{
    // The stair shaft runs from the ground to the top floor: a stack stair's footprint must lie
    // within one stair of every floor (a deeper stair hall round it is the same shaft).
    constexpr double kCovered = 0.95;
    const auto outline = ToPaths (s.outline);
    size_t errors = 0;
    auto fail = [&] (const char* code, const std::string& text, Vec at) {
        Note (s, Diagnostic::Error, code, text, at);
        ++errors;
    };
    std::vector<bool> stacked (s.cores.size (), false);
    for (const auto& pin : stack) {
        bool found = false;
        for (size_t i = 0; i < s.cores.size () && !found; ++i) {
            const Ring foot = Footprint (pin, s.cores[i], o);
            if (Near (foot, s.cores[i].shape, 0) &&
                Overlap (foot, s.cores[i].shape) >= kCovered * std::abs (Area (foot)))
                found = stacked[i] = true;
        }
        if (found)
            continue;
        if (!Inside (outline, pin.centre))
            fail ("core.outside_floor",
                  "A stair of the building is outside this floor: stairs run from the ground to the top floor.",
                  pin.centre);
        else
            fail ("core.stack_moved", "A stair of the building is not where it stands on the other floors.",
                  pin.centre);
    }
    for (size_t i = 0; i < s.cores.size (); ++i)
        if (!stacked[i])
            fail ("core.not_in_stack",
                  "A stair only this floor has: add it to the building so it runs from the ground to the top floor.",
                  s.cores[i].centre);
    for (const auto& d : s.diagnostics)
        s.ok = s.ok && d.level != Diagnostic::Error;
    return errors;
}
} // namespace geomsrv::archviz::floorscheme
