#include "ArchViz/FloorSchemeEditDetail.hpp"
#include <algorithm>

// Editing the skeleton itself: the wings the floor is planned on, and where a wing's corridor runs.
// The first such edit takes the wings the generator found as the design's own (AdoptSkeleton), so
// the user starts from the plan on screen and changes one thing; from then on the generator plans
// on those wings and searches for none. Every edit is checked by the generator's own rule
// (detail::WingFault): one it would leave out is refused here, with the measure that failed.
namespace geomsrv::archviz::floorscheme::edit {
namespace {
using namespace shared;
using namespace detail;

// The floor the wings must lie in: what the scheme planned and what its typology left out of it.
cp::PathsD FloorOf (const Scheme& s)
{
    cp::PathsD floor = ToPaths (s.outline);
    for (const auto& piece : s.culled)
        floor.push_back (ToPath (Counter (piece.shape)));
    return cp::Union (floor, cp::FillRule::NonZero, kPrecision);
}
// The design's wing that is the scheme's `wing`, the skeleton adopted first; -1 when there is none.
int PinOf (const Scheme& s, Design& d, int wing)
{
    if (wing < 0 || wing >= static_cast<int> (s.wings.size ()))
        return -1;
    const bool adopted = d.pins.wings.empty ();
    d = AdoptSkeleton (s, std::move (d));
    // Adopted just now: the pins are the scheme's wings in order. Else by the name it carries.
    // A scheme planned before the adoption still shows the generator's wings: they were named in order.
    const uint32_t id = adopted || !s.authored ? static_cast<uint32_t> (wing) + 1 : s.wings[wing].id;
    for (size_t i = 0; i < d.pins.wings.size (); ++i)
        if (d.pins.wings[i].id == id)
            return static_cast<int> (i);
    return -1;
}
Ring RectOf (const Pins::Wing& pin)
{
    Frame frame;
    return AuthoredWing (pin, frame).rect;
}
// Why the design's wing `at` cannot stand as it is now; empty when it can.
std::string Fault (const Scheme& s, const Design& d, size_t at, const Options& o)
{
    std::vector<Ring> others;
    for (size_t i = 0; i < d.pins.wings.size (); ++i)
        if (i != at)
            others.push_back (RectOf (d.pins.wings[i]));
    return WingFault (FloorOf (s), others, d.pins.wings[at], o);
}
// Every pin inside `rect` moved by `by`: what the user put in a wing goes with the wing.
void Carry (Design& d, const Ring& rect, Vec by)
{
    auto move = [&] (Vec& p) {
        if (In (rect, p))
            p = { p.x + by.x, p.y + by.y };
    };
    for (auto& p : d.pins.cores)
        move (p.centre);
    for (auto& p : d.pins.access)
        move (p.at);
    for (auto& p : d.pins.walls)
        move (p.at);
    for (auto& p : d.pins.rooms)
        move (p.at);
    for (auto& p : d.pins.ends)
        move (p.at);
    for (auto& p : d.pins.doors)
        move (p.at);
    for (auto& p : d.pins.counts)
        move (p.at);
    for (auto& p : d.pins.cuts)
        move (p.at);
    for (auto& p : d.pins.corridors)
        move (p.at);
    for (auto& p : d.locked)
        move (p);
    for (auto& j : d.pins.joins)
        if (In (rect, j.into))
            j.from = { j.from.x + by.x, j.from.y + by.y }, j.into = { j.into.x + by.x, j.into.y + by.y };
    for (auto& g : d.pins.gives)
        if (In (rect, g.into)) {
            g.into = { g.into.x + by.x, g.into.y + by.y };
            for (auto& q : g.region)
                q = { q.x + by.x, q.y + by.y };
        }
}
} // namespace

bool Authored (const Design& d)
{
    return !d.pins.wings.empty ();
}
Design AdoptSkeleton (const Scheme& s, Design d)
{
    if (Authored (d))
        return d;
    uint32_t id = 0;
    for (const auto& w : s.wings)
        d.pins.wings.push_back ({ ++id, w.a, w.b, w.depth, w.mirror });
    return d;
}
Design ReleaseSkeleton (Design d)
{
    d.pins.wings.clear ();
    return d;
}
Design MoveWing (const Scheme& s, Design d, int wing, Vec by, std::string* why, const Options& o)
{
    const Design before = d;
    const int at = PinOf (s, d, wing);
    if (at < 0)
        return Refuse (before, why, "There is no wing there.");
    auto& pin = d.pins.wings[at];
    const Ring was = RectOf (pin);
    pin.a = { pin.a.x + by.x, pin.a.y + by.y }, pin.b = { pin.b.x + by.x, pin.b.y + by.y };
    if (const auto fault = Fault (s, d, static_cast<size_t> (at), o); !fault.empty ())
        return Refuse (before, why, "%s", fault.c_str ());
    Carry (d, was, by);
    return d;
}
Design ResizeWing (const Scheme& s, Design d, int wing, Vec grip, double by, std::string* why, const Options& o)
{
    const Design before = d;
    const int at = PinOf (s, d, wing);
    if (at < 0)
        return Refuse (before, why, "There is no wing there.");
    auto& pin = d.pins.wings[at];
    const Vec u = Unit ({ pin.b.x - pin.a.x, pin.b.y - pin.a.y }), n { -u.y, u.x };
    const Vec mid { (pin.a.x + pin.b.x) / 2, (pin.a.y + pin.b.y) / 2 };
    const double half = pin.depth / 2;
    // The side nearest the grip: an end (the wing's length) or a long side (its depth).
    const Vec sides[4] = {
        pin.a, pin.b, { mid.x + n.x * half, mid.y + n.y * half }, { mid.x - n.x * half, mid.y - n.y * half }
    };
    int side = 0;
    for (int k = 1; k < 4; ++k)
        if (std::hypot (sides[k].x - grip.x, sides[k].y - grip.y) <
            std::hypot (sides[side].x - grip.x, sides[side].y - grip.y))
            side = k;
    if (side == 0)
        pin.a = { pin.a.x - u.x * by, pin.a.y - u.y * by };
    else if (side == 1)
        pin.b = { pin.b.x + u.x * by, pin.b.y + u.y * by };
    else {
        // A long side moves: the centre line follows it by half.
        const double shift = (side == 2 ? 1 : -1) * by / 2;
        pin.a = { pin.a.x + n.x * shift, pin.a.y + n.y * shift };
        pin.b = { pin.b.x + n.x * shift, pin.b.y + n.y * shift };
        pin.depth += by;
    }
    if (side < 2 && Dot ({ pin.b.x - pin.a.x, pin.b.y - pin.a.y }, u) < 0.1)
        return Refuse (before, why, "A wing cannot be made shorter than nothing.");
    if (const auto fault = Fault (s, d, static_cast<size_t> (at), o); !fault.empty ())
        return Refuse (before, why, "%s", fault.c_str ());
    return d;
}
Design SetWing (const Scheme& s, Design d, int wing, Vec a, Vec b, double depth, std::string* why, const Options& o)
{
    const Design before = d;
    const int at = PinOf (s, d, wing);
    if (at < 0)
        return Refuse (before, why, "There is no wing there.");
    auto& pin = d.pins.wings[at];
    // The same way round: an end is not dragged through the other.
    if (Dot ({ b.x - a.x, b.y - a.y }, Unit ({ pin.b.x - pin.a.x, pin.b.y - pin.a.y })) < 0.1 || depth < 0.1)
        return Refuse (before, why, "A wing cannot be made smaller than nothing.");
    pin.a = a, pin.b = b, pin.depth = depth;
    if (const auto fault = Fault (s, d, static_cast<size_t> (at), o); !fault.empty ())
        return Refuse (before, why, "%s", fault.c_str ());
    return d;
}
Design AddWing (const Scheme& s, Design d, Vec a, Vec b, double depth, std::string* why, const Options& o)
{
    const Design before = d;
    d = AdoptSkeleton (s, std::move (d));
    uint32_t id = 0;
    for (const auto& w : d.pins.wings)
        id = (std::max) (id, w.id);
    d.pins.wings.push_back ({ id + 1, a, b, depth, false });
    if (const auto fault = Fault (s, d, d.pins.wings.size () - 1, o); !fault.empty ())
        return Refuse (before, why, "%s", fault.c_str ());
    return d;
}
Design RemoveWing (const Scheme& s, Design d, int wing, std::string* why)
{
    const Design before = d;
    const int at = PinOf (s, d, wing);
    if (at < 0)
        return Refuse (before, why, "There is no wing there.");
    if (d.pins.wings.size () < 2)
        return Refuse (before, why, "A floor keeps one wing at least.");
    d.pins.wings.erase (d.pins.wings.begin () + at);
    return d;
}
Design DropWing (Design d, uint32_t id)
{
    std::erase_if (d.pins.wings, [&] (const Pins::Wing& w) { return w.id == id; });
    return d;
}
Design SetCorridor (const Scheme& s, Design d, int wing, Vec through, double width, std::string* why, const Options& o)
{
    if (wing < 0 || wing >= static_cast<int> (s.wings.size ()))
        return Refuse (std::move (d), why, "There is no wing there.");
    const auto& w = s.wings[wing];
    if (w.access != Access::Centre && w.access != Access::OneSide)
        return Refuse (std::move (d), why, "This wing has no corridor: its flats are entered from the stairs.");
    if (width != 0 && (width < kCorridorMin - 1e-9 || width > kCorridorMax + 1e-9))
        return Refuse (std::move (d), why, "A corridor is %.1f to %.1f m wide: %.2f m is not.", kCorridorMin,
                       kCorridorMax, width);
    const double wide = width > 0 ? width : o.corridor;
    const Vec u = Unit ({ w.b.x - w.a.x, w.b.y - w.a.y }), n { -u.y, u.x };
    const Vec mid { (w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2 };
    // How far off the wing's centre line the corridor's own centre line is asked to run.
    double off = w.access == Access::Centre ? Dot ({ through.x - w.a.x, through.y - w.a.y }, n) : 0;
    if (w.access == Access::Centre) {
        const double row = w.depth / 2 - std::abs (off) - wide / 2;
        const double centred = (w.depth - wide) / 2;
        if (centred < kRowDepthMin - 1e-9) {
            if (std::abs (off) > 0.05)
                return Refuse (std::move (d), why,
                               "This wing is %.1f m deep: its corridor stays in the middle, a row of flats "
                               "either side being at least %.1f m deep.",
                               w.depth, kRowDepthMin);
            off = 0;
        }
        else if (row < kRowDepthMin - 1e-9)
            return Refuse (std::move (d), why,
                           "A corridor there leaves a row of flats %.1f m deep: a row is at least %.1f m deep.", row,
                           kRowDepthMin);
    }
    std::erase_if (d.pins.corridors, [&] (const Pins::Corridor& c) { return In (w.rect, c.at); });
    d.pins.corridors.push_back ({ { mid.x + n.x * off, mid.y + n.y * off }, width });
    return d;
}
Design AddWingAt (const Scheme& s, Design d, Vec at, std::string* why, const Options& o)
{
    const Piece* free = nullptr;
    for (const auto& piece : s.unassigned)
        if (In (piece.shape, at))
            free = &piece;
    if (!free)
        return Refuse (std::move (d), why, "The floor here already belongs to a wing.");
    // Along the nearest wing, so the new one stands square to the skeleton.
    double theta = 0, near = 1e18;
    for (const auto& w : s.wings) {
        const Vec m = Centroid (w.rect);
        if (const double dd = std::hypot (m.x - at.x, m.y - at.y); dd < near)
            near = dd, theta = std::atan2 (w.b.y - w.a.y, w.b.x - w.a.x);
    }
    Frame frame;
    Box box;
    if (!Inscribed ({ ToPath (Counter (free->shape)) }, theta, o.minWingDepth, o.minWingLength, frame, box))
        return Refuse (std::move (d), why,
                       "No wing fits the free floor there: a wing is at least %.1f m deep and %.1f m long.",
                       o.minWingDepth, o.minWingLength);
    // Its longer side is its length; no deeper than a wing may be.
    const bool alongU = box.W () >= box.H ();
    const double depth = (std::min) (alongU ? box.H () : box.W (), o.maxWingDepth);
    const Vec a = alongU ? frame.World (box.u0, box.v0 + depth / 2) : frame.World (box.u0 + depth / 2, box.v0);
    const Vec b = alongU ? frame.World (box.u1, box.v0 + depth / 2) : frame.World (box.u0 + depth / 2, box.v1);
    return AddWing (s, std::move (d), a, b, depth, why, o);
}
uint32_t WingId (const Scheme& s, int wing)
{
    if (wing < 0 || wing >= static_cast<int> (s.wings.size ()))
        return 0;
    return s.authored ? s.wings[wing].id : static_cast<uint32_t> (wing) + 1;
}
Ring WingRect (const Pins::Wing& wing)
{
    return RectOf (wing);
}
std::optional<Vec> CorridorOf (const Scheme& s, int wing)
{
    if (wing < 0 || wing >= static_cast<int> (s.wings.size ()))
        return std::nullopt;
    // A straight corridor in the wing, or the leg of one that turns a corner which runs along it.
    const auto& w = s.wings[wing];
    const Vec u = Unit ({ w.b.x - w.a.x, w.b.y - w.a.y });
    for (const auto& c : s.corridors)
        for (size_t i = 0; i + 1 < c.axis.size (); ++i) {
            const Vec a = c.axis[i], b = c.axis[i + 1], mid { (a.x + b.x) / 2, (a.y + b.y) / 2 };
            if (std::hypot (b.x - a.x, b.y - a.y) > 0.5 && In (w.rect, mid) &&
                std::abs (Dot (Unit ({ b.x - a.x, b.y - a.y }), u)) > 0.9)
                return mid;
        }
    return std::nullopt;
}
Design ResetCorridor (const Scheme& s, Design d, int wing)
{
    if (wing >= 0 && wing < static_cast<int> (s.wings.size ()))
        std::erase_if (d.pins.corridors, [&] (const Pins::Corridor& c) { return In (s.wings[wing].rect, c.at); });
    return d;
}
} // namespace geomsrv::archviz::floorscheme::edit
