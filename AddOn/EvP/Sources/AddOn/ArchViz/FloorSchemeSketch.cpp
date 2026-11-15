#include "ArchViz/FloorSchemeSketch.hpp"
#include "ArchViz/FloorSchemeDetail.hpp"
#include "Edit/EditPolyline.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::floorscheme::sketch {
namespace {
constexpr double kSame = 1e-6;

ge::Vec3 P (Vec p)
{
    return { p.x, p.y, 0.0 };
}
Vec Q (ge::Vec3 p)
{
    return { p.x, p.y };
}
double Dot (Vec a, Vec b)
{
    return a.x * b.x + a.y * b.y;
}
Vec Unit (Vec v)
{
    const double l = std::hypot (v.x, v.y);
    return l > 1e-12 ? Vec { v.x / l, v.y / l } : Vec { 1, 0 };
}
Vec Sub (Vec a, Vec b)
{
    return { a.x - b.x, a.y - b.y };
}
Vec Mid (const std::vector<Vec>& points)
{
    Vec m;
    for (const auto& p : points)
        m.x += p.x / points.size (), m.y += p.y / points.size ();
    return m;
}
std::vector<Vec> Points (const ge::Entity& e)
{
    std::vector<Vec> out;
    for (const auto& v : e.vertices)
        out.push_back (Q (v.p));
    return out;
}
bool Same (const std::vector<Vec>& a, const std::vector<Vec>& b)
{
    if (a.size () != b.size ())
        return false;
    for (size_t i = 0; i < a.size (); ++i)
        if (std::hypot (a[i].x - b[i].x, a[i].y - b[i].y) > kSame)
            return false;
    return true;
}
// The least and most of `points` along `u` from `origin`.
std::pair<double, double> Extent (const std::vector<Vec>& points, Vec origin, Vec u)
{
    double lo = 1e300, hi = -1e300;
    for (const auto& p : points) {
        const double t = Dot (Sub (p, origin), u);
        lo = (std::min) (lo, t), hi = (std::max) (hi, t);
    }
    return { lo, hi };
}
// Whether `points` are the four corners of a rectangle with sides along `u` and across it.
bool Square (const std::vector<Vec>& points, Vec origin, Vec u)
{
    if (points.size () != 4)
        return false;
    const Vec n { -u.y, u.x };
    const auto [s0, s1] = Extent (points, origin, u);
    const auto [t0, t1] = Extent (points, origin, n);
    for (const auto& p : points) {
        const double s = Dot (Sub (p, origin), u), t = Dot (Sub (p, origin), n);
        if ((std::min) (std::abs (s - s0), std::abs (s - s1)) > 1e-4 ||
            (std::min) (std::abs (t - t0), std::abs (t - t1)) > 1e-4)
            return false;
    }
    return true;
}
void Shift (ge::Entity& e, Vec by)
{
    for (auto& v : e.vertices)
        v.p.x += by.x, v.p.y += by.y;
}
// The closest point of segment a-b to p.
Vec Closest (Vec a, Vec b, Vec p)
{
    const Vec d = Sub (b, a);
    const double l2 = Dot (d, d), t = l2 > 1e-18 ? std::clamp (Dot (Sub (p, a), d) / l2, 0.0, 1.0) : 0.0;
    return { a.x + d.x * t, a.y + d.y * t };
}
bool Inside (const Ring& ring, Vec p)
{
    bool in = false;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++)
        if ((ring[i].y > p.y) != (ring[j].y > p.y) &&
            p.x < (ring[j].x - ring[i].x) * (p.y - ring[i].y) / (ring[j].y - ring[i].y) + ring[i].x)
            in = !in;
    return in;
}
edit::Design Refused (edit::Design d, std::string* why, const std::string& text)
{
    if (why && why->empty ())
        *why = text;
    return d;
}
} // namespace

const Item* Sketch::Find (ge::Id entity) const
{
    for (const auto& item : items)
        if (item.entity == entity)
            return &item;
    return nullptr;
}
const Item* Sketch::Find (What what, int index) const
{
    for (const auto& item : items)
        if (item.what == what && item.index == index)
            return &item;
    return nullptr;
}

Sketch Of (const Scheme& s)
{
    Sketch out;
    const auto add = [&] (What what, int index, const std::vector<Vec>& points, bool closed, const char* role) {
        std::vector<ge::Vec3> line;
        for (const auto& p : points)
            if (line.empty () || ge::DistanceXY (line.back (), P (p)) > 1e-4)
                line.push_back (P (p));
        if (closed && line.size () > 1 && ge::DistanceXY (line.back (), line.front ()) <= 1e-4)
            line.pop_back ();
        if (line.size () < (closed ? 3u : 2u))
            return;
        out.items.push_back ({ what, index, ge::AddPolyline (out.doc, line, closed, role) });
    };
    for (size_t i = 0; i < s.wings.size (); ++i)
        add (What::Wing, int (i), s.wings[i].rect, true, "wing");
    for (size_t i = 0; i < s.corridors.size (); ++i)
        add (What::Corridor, int (i), s.corridors[i].axis, false, "corridor");
    for (size_t i = 0; i < s.cores.size (); ++i)
        add (What::Stair, int (i), s.cores[i].shape, true, "stair");
    return out;
}

ge::SnapContext Context (const Scheme& s, const std::vector<Ring>& contour)
{
    ge::SnapContext context;
    const auto add = [&] (const std::vector<Ring>& rings, const char* source) {
        for (const auto& ring : rings)
            for (size_t i = 0; i < ring.size (); ++i) {
                context.points.push_back ({ P (ring[i]), source });
                context.segments.push_back ({ P (ring[i]), P (ring[(i + 1) % ring.size ()]), source });
            }
    };
    add (s.outline, "floor");
    add (contour, "slab");
    return context;
}

bool Drag (ge::Document& doc, const Sketch& base, const ge::Ref& ref, Vec by, std::string& error)
{
    const Item* item = base.Find (ref.entity);
    ge::Entity* e = ge::Find (doc, ref.entity);
    if (!item || !e)
        return error = "there is nothing there to move", false;
    // A side moved parallel to itself, as far as `by` takes it across.
    const auto side = [&] (size_t k) {
        size_t from = 0, to = 0;
        ge::SegmentEnds (*e, k, from, to);
        const Vec dir = Unit (Sub (Q (e->vertices[to].p), Q (e->vertices[from].p)));
        const double across = Dot (by, { -dir.y, dir.x });
        return std::abs (across) < 1e-9 || ge::OffsetSegment (doc, e->id, e->segments[k].id, across, error);
    };
    if (item->what == What::Corridor) {
        if (ref.part == ge::Part::Vertex) {
            const int i = ge::VertexIndex (*e, ref.item);
            if (i < 0 || !ge::OpenEnd (*e, size_t (i)))
                return error = "a corridor's turn follows its wings: move a leg or an end", false;
            // An end runs along its own leg: the corridor's length.
            const size_t next = i == 0 ? 1 : size_t (i) - 1;
            const Vec at = Q (e->vertices[i].p), dir = Unit (Sub (at, Q (e->vertices[next].p)));
            const double along = Dot (by, dir);
            return ge::MoveVertex (doc, e->id, ref.item, P ({ at.x + dir.x * along, at.y + dir.y * along }), error);
        }
        if (ref.part != ge::Part::Segment)
            return error = "hold a corridor by a leg or an end", false;
        // A leg moves across its wing; the leg it turns into runs on to meet it.
        const int k = ge::SegmentIndex (*e, ref.item);
        if (k < 0)
            return error = "there is no such leg", false;
        const Vec a = Q (e->vertices[k].p), b = Q (e->vertices[k + 1].p), dir = Unit (Sub (b, a));
        const Vec n { -dir.y, dir.x };
        const double across = Dot (by, n);
        for (const size_t i : { size_t (k), size_t (k) + 1 })
            e->vertices[i].p.x += n.x * across, e->vertices[i].p.y += n.y * across;
        return true;
    }
    if (ref.part == ge::Part::Segment) {
        const int k = ge::SegmentIndex (*e, ref.item);
        return k >= 0 ? side (size_t (k)) : (error = "there is no such side", false);
    }
    if (ref.part == ge::Part::Vertex && item->what == What::Wing) {
        // A corner takes the two sides it joins.
        const int i = ge::VertexIndex (*e, ref.item);
        if (i < 0)
            return error = "there is no such corner", false;
        const ge::Id before = e->segments[ge::PrevVertex (*e, size_t (i))].id, after = e->segments[i].id;
        for (const ge::Id id : { before, after })
            if (const int k = ge::SegmentIndex (*ge::Find (doc, ref.entity), id); k < 0 || !side (size_t (k)))
                return false;
        return true;
    }
    Shift (*e, by);
    return true;
}

edit::Design Read (const Scheme& s, edit::Design d, const Sketch& before, const ge::Document& after, std::string* why,
                   const Options& o)
{
    const edit::Design start = d;
    std::string said;
    for (const auto& item : before.items) {
        const ge::Entity *was = ge::Find (before.doc, item.entity), *now = ge::Find (after, item.entity);
        if (!was || !now)
            continue;
        const auto p0 = Points (*was), p1 = Points (*now);
        if (Same (p0, p1))
            continue;
        const edit::Design from = d;
        if (item.what == What::Wing) {
            const auto& w = s.wings[item.index];
            const Vec u = Unit (Sub (w.b, w.a)), n { -u.y, u.x };
            if (!Square (p1, w.a, u))
                return Refused (start, why, "A wing stays a rectangle along its own direction.");
            const auto [s0, s1] = Extent (p0, w.a, u);
            const auto [t0, t1] = Extent (p0, w.a, n);
            const auto [S0, S1] = Extent (p1, w.a, u);
            const auto [T0, T1] = Extent (p1, w.a, n);
            const double d0 = S0 - s0, d1 = S1 - s1, e0 = T0 - t0, e1 = T1 - t1;
            if (std::abs (d0 - d1) < kSame && std::abs (e0 - e1) < kSame)
                d = edit::MoveWing (s, std::move (d), item.index, { u.x * d0 + n.x * e0, u.y * d0 + n.y * e0 }, &said,
                                    o);
            else {
                const double across = (e0 + e1) / 2;
                d = edit::SetWing (
                    s, std::move (d), item.index, { w.a.x + u.x * d0 + n.x * across, w.a.y + u.y * d0 + n.y * across },
                    { w.b.x + u.x * d1 + n.x * across, w.b.y + u.y * d1 + n.y * across }, w.depth + e1 - e0, &said, o);
            }
        }
        else if (item.what == What::Stair) {
            const auto& core = s.cores[item.index];
            const int wing = edit::WingAt (s, core.centre);
            const Vec u = wing >= 0 ? Unit (Sub (s.wings[wing].b, s.wings[wing].a)) : Vec { 1, 0 }, n { -u.y, u.x };
            const auto size = [&] (const std::vector<Vec>& p, Vec axis) {
                const auto [lo, hi] = Extent (p, core.centre, axis);
                return hi - lo;
            };
            const double du = size (p1, u) - size (p0, u), dn = size (p1, n) - size (p0, n);
            if (std::abs (du) > kSame || std::abs (dn) > kSame) {
                // Its width is along the corridor: the way the scheme has it standing.
                const bool along = std::abs (size (p0, u) - core.width) <= std::abs (size (p0, u) - core.depth);
                d = edit::ResizeStair (s, std::move (d), item.index, core.width + (along ? du : dn),
                                       core.depth + (along ? dn : du), &said);
                if (d == from)
                    return Refused (start, why, said.empty () ? "The stair cannot be that size." : said);
            }
            const Vec by = Sub (Mid (p1), Mid (p0));
            if (std::hypot (by.x, by.y) > kSame) {
                const Vec to { core.centre.x + by.x, core.centre.y + by.y };
                bool on = false;
                for (const auto& ring : s.outline)
                    on = on != Inside (ring, to);
                if (!on)
                    return Refused (start, why, "A stair stands on the floor.");
                d = edit::MoveStair (s, std::move (d), item.index, to, &said);
            }
        }
        else {
            // Each leg that moved across its wing; then each end that ran along its leg.
            for (size_t i = 0; i + 1 < p0.size () && i + 1 < p1.size (); ++i) {
                const Vec dir = Unit (Sub (p0[i + 1], p0[i])), n { -dir.y, dir.x };
                const Vec m0 { (p0[i].x + p0[i + 1].x) / 2, (p0[i].y + p0[i + 1].y) / 2 };
                const Vec m1 { (p1[i].x + p1[i + 1].x) / 2, (p1[i].y + p1[i + 1].y) / 2 };
                const double across = Dot (Sub (m1, m0), n);
                if (std::abs (across) < kSame)
                    continue;
                int wing = -1;
                for (size_t k = 0; k < s.wings.size () && wing < 0; ++k)
                    if (Inside (s.wings[k].rect, m0) &&
                        std::abs (Dot (Unit (Sub (s.wings[k].b, s.wings[k].a)), dir)) > 0.9)
                        wing = int (k);
                if (wing < 0)
                    return Refused (start, why, "This leg of the corridor belongs to no wing of its own.");
                double width = 0;
                for (const auto& pin : d.pins.corridors)
                    if (Inside (s.wings[wing].rect, pin.at))
                        width = pin.width;
                const edit::Design leg = d;
                d = edit::SetCorridor (s, std::move (d), wing, { m0.x + n.x * across, m0.y + n.y * across }, width,
                                       &said, o);
                if (d == leg)
                    return Refused (start, why, said.empty () ? "The corridor cannot run there." : said);
            }
            for (const bool last : { false, true }) {
                if (p0.size () < 2 || p1.size () != p0.size ())
                    break;
                const size_t i = last ? p0.size () - 1 : 0, next = last ? i - 1 : 1;
                const Vec dir = Unit (Sub (p0[i], p0[next]));
                const double along = Dot (Sub (p1[i], p0[i]), dir);
                if (std::abs (along) < kSame)
                    continue;
                // No shorter than a stair's width: what is left must still be a corridor.
                if (Dot (Sub (p1[i], p0[next]), dir) < o.coreWidth)
                    return Refused (start, why, "A corridor is not made shorter than a stair is wide.");
                d = edit::MoveEnd (std::move (d), { item.index, last, p0[i] },
                                   { p0[i].x + dir.x * along, p0[i].y + dir.y * along });
            }
        }
        if (d == from)
            return Refused (start, why, said.empty () ? "The floor cannot take that." : said);
    }
    return d;
}

Graph Access (const Scheme& s)
{
    Graph g;
    struct Leg {
        Vec a, b;
    };
    std::vector<Leg> legs;
    for (size_t i = 0; i < s.corridors.size (); ++i)
        for (size_t k = 0; k + 1 < s.corridors[i].axis.size (); ++k) {
            const Vec a = s.corridors[i].axis[k], b = s.corridors[i].axis[k + 1];
            legs.push_back ({ a, b });
            g.nodes.push_back ({ Graph::Kind::Corridor, int (i), { (a.x + b.x) / 2, (a.y + b.y) / 2 } });
            g.edges.push_back ({ { a, b } });
        }
    // The circulation nearest `p`: a point of a corridor's centre line, a lobby's or a stair's middle.
    const auto circulation = [&] (Vec p, bool stairs) {
        Vec best = p;
        double near = 1e300;
        const auto take = [&] (Vec q) {
            if (const double dd = std::hypot (q.x - p.x, q.y - p.y); dd < near)
                near = dd, best = q;
        };
        for (const auto& leg : legs)
            take (Closest (leg.a, leg.b, p));
        for (const auto& lobby : s.lobbies)
            take (Mid (lobby));
        if (stairs || near > 1e299)
            for (const auto& core : s.cores)
                take (core.centre);
        return std::pair { best, near };
    };
    for (size_t i = 0; i < s.cores.size (); ++i) {
        const auto& core = s.cores[i];
        g.nodes.push_back ({ Graph::Kind::Stair, int (i), core.centre });
        // A stair stands on the corridor it serves: joined when one passes within its own size.
        if (const auto [to, far] = circulation (core.centre, false);
            far < (std::max) (core.width, core.depth) + 3.0 && far > 1e-6)
            g.edges.push_back ({ { core.centre, to } });
    }
    for (size_t i = 0; i < s.flats.size (); ++i) {
        const auto& flat = s.flats[i];
        const Vec at = Mid (flat.shape);
        g.nodes.push_back ({ Graph::Kind::Flat, int (i), at });
        const bool door = std::hypot (flat.door.b.x - flat.door.a.x, flat.door.b.y - flat.door.a.y) > 1e-6;
        const Vec through { (flat.door.a.x + flat.door.b.x) / 2, (flat.door.a.y + flat.door.b.y) / 2 };
        const auto [to, far] = circulation (door ? through : at, true);
        if (far > 1e299)
            continue;
        g.edges.push_back (door ? Graph::Edge { { at, through, to }, Graph::Edge::Kind::Access, int (i) }
                                : Graph::Edge { { at, to }, Graph::Edge::Kind::Access, int (i) });
        // Inside it: the rooms, each entered from the room before it on the way from the hall.
        const auto& rooms = flat.roomList;
        std::vector<Vec> centre (rooms.size ());
        std::vector<int> from (rooms.size (), -2); // -2 not reached, -1 the hall itself
        std::vector<size_t> queue;
        for (size_t k = 0; k < rooms.size (); ++k) {
            centre[k] = Mid (rooms[k].shape);
            if (rooms[k].extends >= 0)
                continue; // a further box of another room: that room's node stands for both
            g.nodes.push_back ({ Graph::Kind::Room, int (i), centre[k], int (k), rooms[k].kind });
            if (rooms[k].kind == RoomKind::Hall && queue.empty ())
                from[k] = -1, queue.push_back (k);
        }
        if (queue.empty ())
            continue;
        const size_t hall = queue.front ();
        if (door)
            g.edges.push_back ({ { through, centre[hall] }, Graph::Edge::Kind::Inside, int (i) });
        // The room a box belongs to, and whether two rooms share a wall a door fits in.
        const auto owner = [&] (size_t k) { return rooms[k].extends >= 0 ? size_t (rooms[k].extends) : k; };
        const auto beside = [&] (size_t a, size_t b) {
            for (size_t p = 0; p < rooms.size (); ++p)
                for (size_t q = 0; q < rooms.size (); ++q)
                    if (owner (p) == a && owner (q) == b && detail::Touch (rooms[p].shape, rooms[q].shape, 0.3) >= 0.8)
                        return true;
            return false;
        };
        for (size_t head = 0; head < queue.size (); ++head)
            for (size_t k = 0; k < rooms.size (); ++k)
                if (from[k] == -2 && rooms[k].extends < 0 && beside (queue[head], k))
                    from[k] = int (queue[head]), queue.push_back (k);
        for (size_t k = 0; k < rooms.size (); ++k) {
            if (rooms[k].extends >= 0 || k == hall)
                continue;
            // A room no wall joins to the others is still entered from the hall.
            const size_t parent = from[k] >= 0 ? size_t (from[k]) : hall;
            g.edges.push_back ({ { centre[parent], centre[k] }, Graph::Edge::Kind::Inside, int (i) });
        }
    }
    return g;
}
} // namespace geomsrv::archviz::floorscheme::sketch
