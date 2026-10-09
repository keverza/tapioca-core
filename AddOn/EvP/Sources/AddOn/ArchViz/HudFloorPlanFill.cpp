// The quick plan's programme fill, score and bounded unit search, and its walking egress.
// Rules and weights follow the private generator (S6 counts, S7 penalties, S10 egress) so
// both read one programme; this is a quick interactive scheme, not a compliance check.
#include "ArchViz/HudFloorPlanFrame.hpp"
#include <algorithm>
#include <deque>
#include <map>

namespace geomsrv::archviz::buildingplan {
namespace {
using namespace frame;
constexpr double kRangeWeight = 2.0; // per m2 of net area outside the type's range
constexpr double kNoWindow = 50.0;   // a flat with less than one window of facade
constexpr double kFrontage = 1.0;    // per metre of facade below the type's frontage
constexpr double kNoEntrance = 30.0; // a flat with no door onto circulation
constexpr double kCornerSmall = 6.0; // per room below kCornerRooms on a corner
constexpr double kCornerRooms = 4.0; // the largest flats take the corners (A/B picks)
constexpr double kLockWeight = 40.0; // per kept trait a locked flat lost
constexpr double kLongWeight = 4.0;  // a shallow flat stretched along the facade
constexpr double kLongRatio = 1.5;
constexpr double kMixWeight = 3.0;     // per unit of programme mix deviation
constexpr double kEmptyWeight = 4.0;   // per m2 of floor no flat or circulation reaches
constexpr double kEgressWeight = 10.0; // per metre of corridor beyond its egress limit
constexpr double kDeadEnd = 25.0, kTwoExits = 40.0, kSharedPath = 0.0;
double Area (const PlanRegion& region, const QuickPlan& plan)
{
    return std::abs (cp::Area (Paths (plan, region.rings)));
}
struct Move {
    enum Kind : uint8_t { Retype, Swap, Remove, Add, Order } kind;
    size_t a = 0, b = 0;
};
std::vector<size_t> Ordered (const std::vector<UnitSeed>& seeds, size_t segment)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < seeds.size (); ++i)
        if (seeds[i].segment == segment)
            out.push_back (i);
    std::sort (out.begin (), out.end (), [&] (size_t a, size_t b) { return seeds[a].along < seeds[b].along; });
    return out;
}
std::vector<Move> Moves (const QuickPlan& plan, const std::vector<UnitSeed>& seeds)
{
    std::vector<Move> moves;
    const size_t types = plan.programme.types.size ();
    for (size_t i = 0; i < seeds.size (); ++i)
        if (!seeds[i].locked)
            for (size_t t = 0; t < types; ++t)
                if (t != seeds[i].type)
                    moves.push_back ({ Move::Retype, i, t });
    for (size_t i = 0; i < seeds.size (); ++i)
        for (size_t j = i + 1; j < seeds.size (); ++j)
            if (!seeds[i].locked && !seeds[j].locked && seeds[i].type != seeds[j].type &&
                seeds[i].segment != seeds[j].segment)
                moves.push_back ({ Move::Swap, i, j });
    for (size_t s = 0; s < plan.segments.size (); ++s) {
        const auto order = Ordered (seeds, s);
        for (size_t k = 0; k + 1 < order.size (); ++k)
            moves.push_back ({ Move::Order, order[k], order[k + 1] });
        const auto& segment = plan.segments[s];
        if (order.size () > 1)
            for (size_t i : order)
                if (!seeds[i].locked)
                    moves.push_back ({ Move::Remove, i });
        if (!segment.endAccess && double (order.size () + 1) * kMinWidth <= segment.hi - segment.lo &&
            seeds.size () < kMaxUnits)
            for (size_t i : order)
                moves.push_back ({ Move::Add, i });
    }
    return moves;
}
bool Apply (const QuickPlan& plan, std::vector<UnitSeed>& seeds, const Move& move, uint32_t& nextId)
{
    const auto& types = plan.programme.types;
    switch (move.kind) {
        case Move::Retype:
            seeds[move.a].type = move.b;
            seeds[move.a].target = floorprogramme::Target (types[move.b]);
            return true;
        case Move::Swap:
            std::swap (seeds[move.a].type, seeds[move.b].type);
            std::swap (seeds[move.a].target, seeds[move.b].target);
            return true;
        case Move::Remove:
            seeds.erase (seeds.begin () + std::ptrdiff_t (move.a));
            return true;
        case Move::Order:
            std::swap (seeds[move.a].along, seeds[move.b].along);
            return true;
        case Move::Add: {
            // Split the flat in two: the new one takes the smaller half's nearest type.
            const auto& seed = seeds[move.a];
            UnitSeed added = seed;
            added.id = nextId++;
            added.locked = false;
            added.keep = 0;
            added.along = (seed.lo + seed.along) / 2;
            added.type = floorprogramme::Nearest (plan.programme, Traits (plan, seed).net / 2);
            added.target = floorprogramme::Target (types[added.type]);
            if (added.along >= seed.along - 1e-6)
                return false;
            seeds.push_back (added);
            return true;
        }
    }
    return false;
}
} // namespace

double Score (const QuickPlan& plan, const std::vector<UnitSeed>& seeds)
{
    const auto& types = plan.programme.types;
    if (types.empty ())
        return 0;
    std::vector<int> counts (types.size (), 0);
    double score = 0;
    for (const auto& seed : seeds) {
        const size_t type = (std::min) (seed.type, types.size () - 1);
        ++counts[type];
        const auto& t = types[type];
        const auto traits = Traits (plan, seed);
        score += kRangeWeight * ((std::max) (0.0, t.minM2 - traits.net) + (std::max) (0.0, traits.net - t.maxM2));
        if (traits.facade < 1.2)
            score += kNoWindow;
        else
            score += kFrontage * (std::max) (0.0, floorprogramme::Frontage (t) - traits.facade);
        if (traits.access < 0.9)
            score += kNoEntrance;
        if ((traits.traits & kCorner) && t.rooms < kCornerRooms)
            score += kCornerSmall * (kCornerRooms - t.rooms);
        if (seed.locked)
            for (uint8_t trait : { kCorner, kDualAspect, kStraightFacade })
                if ((seed.keep & trait) && !(traits.traits & trait))
                    score += kLockWeight;
        const double ratio = traits.depth > 1e-6 ? (seed.hi - seed.lo) / traits.depth : 0;
        if (!(traits.traits & kCorner) && ratio > kLongRatio)
            score += kLongWeight * (ratio - kLongRatio);
    }
    score += kMixWeight * floorprogramme::MixCost (plan.programme, counts);
    for (const auto& region : plan.unassigned)
        score += kEmptyWeight * Area (region, plan);
    score += kEgressWeight * double (plan.egress.invalid.size ()) * kModule;
    return score;
}

void Fill (QuickPlan& plan)
{
    plan.seeds.clear ();
    plan.nextId = 1;
    const auto& programme = plan.programme;
    if (programme.types.empty () || plan.segments.empty ())
        return;
    const double mean = floorprogramme::MeanArea (programme);
    struct Band {
        double net = 0, depth = 0;
        size_t count = 1;
        bool low = false, high = false; // end facades: corners
    };
    std::vector<Band> bands (plan.segments.size ());
    size_t total = 0;
    for (size_t s = 0; s < plan.segments.size (); ++s) {
        const auto& segment = plan.segments[s];
        auto& band = bands[s];
        const double length = segment.hi - segment.lo, gross = AreaBefore (segment, segment.hi);
        band.depth = length > 1e-6 ? gross / length : 0;
        band.net = gross - floorprogramme::kFacade * FacadeLength (segment, segment.lo, segment.hi);
        const size_t widest = (std::max) (size_t (1), size_t (std::floor (length / kMinWidth + 1e-9)));
        band.count = segment.endAccess
                         ? 1
                         : std::clamp (size_t (std::llround (band.net / (mean + floorprogramme::kWall * band.depth))),
                                       size_t (1), widest);
        const uint8_t forward = segment.alongX ? 0 : 1, backward = segment.alongX ? 2 : 3;
        for (const auto& edge : segment.facade)
            if (edge.a1 - edge.a0 <= 1e-6) {
                band.low |= edge.side == backward && edge.a0 < segment.lo + kMinWidth;
                band.high |= edge.side == forward && edge.a0 > segment.hi - kMinWidth;
            }
        total += band.count;
    }
    while (total > kMaxUnits) {
        auto most = std::max_element (bands.begin (), bands.end (),
                                      [] (const Band& a, const Band& b) { return a.count < b.count; });
        if (most->count <= 1)
            break;
        --most->count;
        --total;
    }
    // Integer counts by share, largest types to the largest slots, then pairwise swaps that
    // bring each band's targets nearer its net area.
    const auto counts = floorprogramme::Counts (programme, int (total));
    std::vector<size_t> pool;
    for (size_t t = 0; t < counts.size (); ++t)
        pool.insert (pool.end (), size_t (counts[t]), t);
    std::stable_sort (pool.begin (), pool.end (), [&] (size_t a, size_t b) {
        return floorprogramme::Target (programme.types[a]) > floorprogramme::Target (programme.types[b]);
    });
    struct Slot {
        size_t band;
        double capacity;
        size_t type = 0;
    };
    std::vector<Slot> slots;
    for (size_t s = 0; s < bands.size (); ++s)
        for (size_t k = 0; k < bands[s].count; ++k)
            slots.push_back ({ s, (bands[s].net - floorprogramme::kWall * bands[s].depth * bands[s].count) /
                                      double (bands[s].count) });
    std::stable_sort (slots.begin (), slots.end (),
                      [] (const Slot& a, const Slot& b) { return a.capacity > b.capacity; });
    for (size_t i = 0; i < slots.size () && i < pool.size (); ++i)
        slots[i].type = pool[i];
    std::vector<double> sums (bands.size (), 0);
    for (const auto& slot : slots)
        sums[slot.band] += floorprogramme::Target (programme.types[slot.type]);
    const auto error = [&] (size_t band, double sum) {
        const double want = bands[band].net - floorprogramme::kWall * bands[band].depth * bands[band].count;
        return (sum - want) * (sum - want);
    };
    for (int round = 0; round < 3; ++round) {
        bool improved = false;
        for (size_t i = 0; i < slots.size (); ++i)
            for (size_t j = i + 1; j < slots.size (); ++j) {
                auto& a = slots[i];
                auto& b = slots[j];
                if (a.band == b.band || a.type == b.type)
                    continue;
                const double ta = floorprogramme::Target (programme.types[a.type]);
                const double tb = floorprogramme::Target (programme.types[b.type]);
                const double before = error (a.band, sums[a.band]) + error (b.band, sums[b.band]);
                const double after = error (a.band, sums[a.band] - ta + tb) + error (b.band, sums[b.band] - tb + ta);
                if (after < before - 1e-9) {
                    sums[a.band] += tb - ta;
                    sums[b.band] += ta - tb;
                    std::swap (a.type, b.type);
                    improved = true;
                }
            }
        if (!improved)
            break;
    }
    for (size_t s = 0; s < bands.size (); ++s) {
        std::vector<size_t> types;
        for (const auto& slot : slots)
            if (slot.band == s)
                types.push_back (slot.type);
        std::stable_sort (types.begin (), types.end (), [&] (size_t a, size_t b) {
            return floorprogramme::Target (programme.types[a]) > floorprogramme::Target (programme.types[b]);
        });
        // Largest flats on the corners: both ends lit, the two largest at the ends.
        std::vector<size_t> order;
        if (bands[s].low && bands[s].high && types.size () > 1) {
            order.push_back (types[0]);
            order.insert (order.end (), types.begin () + 2, types.end ());
            order.push_back (types[1]);
        }
        else if (bands[s].high && !bands[s].low)
            order.assign (types.rbegin (), types.rend ());
        else
            order = types;
        const auto& segment = plan.segments[s];
        const double gross = AreaBefore (segment, segment.hi);
        double weight = 0;
        for (size_t t : order)
            weight += floorprogramme::Target (programme.types[t]) + floorprogramme::kWall * bands[s].depth;
        double cursor = 0;
        for (size_t t : order) {
            const double w = floorprogramme::Target (programme.types[t]) + floorprogramme::kWall * bands[s].depth;
            UnitSeed seed;
            seed.id = plan.nextId++;
            seed.segment = s;
            seed.type = t;
            seed.target = floorprogramme::Target (programme.types[t]);
            seed.along = AlongAtArea (segment, gross * (cursor + w / 2) / (std::max) (weight, 1e-9));
            plan.seeds.push_back (seed);
            cursor += w;
        }
    }
    // Reduce an overfull initial band instead of accepting undersized / unserved flats.
    while (!RelaxUnits (plan) && !plan.seeds.empty ()) {
        size_t failed = plan.segments.size ();
        for (size_t s = 0; s < plan.segments.size (); ++s) {
            std::vector<UnitSeed> band;
            for (const auto& seed : plan.seeds)
                if (seed.segment == s)
                    band.push_back (seed);
            std::string note;
            if (!SolveCuts (plan, band, note)) {
                failed = s;
                break;
            }
        }
        const auto victim = std::find_if (plan.seeds.begin (), plan.seeds.end (), [&] (const UnitSeed& seed) {
            return seed.segment == failed && Ordered (plan.seeds, seed.segment).size () > 1;
        });
        if (victim == plan.seeds.end ()) {
            plan.seeds.clear ();
            plan.units.clear ();
            break;
        }
        plan.seeds.erase (victim);
    }
}

bool OptimiseUnits (QuickPlan& plan, size_t budget)
{
    if (!plan.ready || plan.seeds.empty () || plan.programme.types.empty ())
        return false;
    std::string note;
    auto current = plan.seeds;
    if (!SolveCuts (plan, current, note))
        return false;
    double best = Score (plan, current);
    const double start = best;
    uint32_t nextId = plan.nextId;
    size_t evaluations = 0, cursor = 0, quiet = 0;
    auto moves = Moves (plan, current);
    while (evaluations < budget && !moves.empty () && quiet < moves.size ()) {
        const auto move = moves[cursor % moves.size ()];
        ++cursor;
        ++quiet;
        auto candidate = current;
        uint32_t id = nextId;
        if (!Apply (plan, candidate, move, id) || !SolveCuts (plan, candidate, note))
            continue;
        ++evaluations;
        const double score = Score (plan, candidate);
        if (score < best - 1e-6) {
            best = score;
            current = std::move (candidate);
            nextId = id;
            moves = Moves (plan, current);
            quiet = 0;
        }
    }
    if (best >= start - 1e-9)
        return false;
    plan.seeds = std::move (current);
    plan.nextId = nextId;
    plan.selected = -1;
    return RelaxUnits (plan);
}

Egress AnalyseEgress (const QuickPlan& plan)
{
    Egress out;
    if (plan.corridors.empty () || plan.cores.empty ())
        return out;
    cp::PathsD corridors;
    for (const auto& region : plan.corridors) {
        const auto paths = Paths (plan, region.rings);
        corridors.insert (corridors.end (), paths.begin (), paths.end ());
    }
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& path : corridors)
        for (const auto& p : path) {
            x0 = (std::min) (x0, p.x);
            y0 = (std::min) (y0, p.y);
            x1 = (std::max) (x1, p.x);
            y1 = (std::max) (y1, p.y);
        }
    if (x0 > x1)
        return out;
    const int nx = int (std::ceil ((x1 - x0) / kModule)), ny = int (std::ceil ((y1 - y0) / kModule));
    if (nx <= 0 || ny <= 0 || double (nx) * ny > 200000)
        return out;
    std::map<std::pair<int, int>, size_t> index;
    std::vector<Point> centres;
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j) {
            const Point c { x0 + (i + 0.5) * kModule, y0 + (j + 0.5) * kModule };
            if (Inside (corridors, c)) {
                index[{ i, j }] = centres.size ();
                centres.push_back (c);
            }
        }
    out.cells = centres.size ();
    if (centres.empty ())
        return out;
    // Walking distance in modules from each stair's door cells (corridor cells beside it).
    constexpr int kFar = 1 << 29;
    std::vector<std::vector<int>> distance;
    std::vector<std::vector<size_t>> doors;
    for (const auto& core : plan.cores) {
        const auto r = CoreRect (plan, core);
        std::vector<size_t> door;
        for (size_t c = 0; c < centres.size (); ++c) {
            const double dx = (std::max) ({ r.x0 - centres[c].x, 0.0, centres[c].x - r.x1 });
            const double dy = (std::max) ({ r.y0 - centres[c].y, 0.0, centres[c].y - r.y1 });
            if (std::hypot (dx, dy) <= kModule + 1e-6)
                door.push_back (c);
        }
        std::vector<int> d (centres.size (), kFar);
        std::deque<size_t> queue;
        for (size_t c : door) {
            d[c] = 0;
            queue.push_back (c);
        }
        while (!queue.empty ()) {
            const size_t c = queue.front ();
            queue.pop_front ();
            const int i = int (std::floor ((centres[c].x - x0) / kModule)),
                      j = int (std::floor ((centres[c].y - y0) / kModule));
            for (const auto& [di, dj] : { std::pair { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) {
                const auto next = index.find ({ i + di, j + dj });
                if (next != index.end () && d[next->second] == kFar) {
                    d[next->second] = d[c] + 1;
                    queue.push_back (next->second);
                }
            }
        }
        distance.push_back (std::move (d));
        doors.push_back (std::move (door));
    }
    const size_t stairs = distance.size ();
    std::vector<std::vector<int>> between (stairs, std::vector<int> (stairs, kFar));
    for (size_t a = 0; a < stairs; ++a)
        for (size_t b = 0; b < stairs; ++b)
            for (size_t c : doors[b])
                between[a][b] = (std::min) (between[a][b], distance[a][c]);
    for (size_t c = 0; c < centres.size (); ++c) {
        int nearest = kFar;
        size_t first = 0;
        for (size_t a = 0; a < stairs; ++a)
            if (distance[a][c] < nearest) {
                nearest = distance[a][c];
                first = a;
            }
        if (nearest == kFar)
            continue; // a corridor no stair reaches: its flats have no access
        bool two = false;
        for (size_t b = 0; b < stairs && !two; ++b)
            if (b != first && distance[b][c] < kFar && between[first][b] < kFar)
                two = (nearest + distance[b][c] - between[first][b]) / 2.0 * kModule <= kSharedPath + 1e-9;
        const double metres = nearest * kModule;
        out.longest = (std::max) (out.longest, metres);
        if (metres > (two ? kTwoExits : kDeadEnd) + 1e-9)
            out.invalid.push_back (World (plan, centres[c]));
    }
    return out;
}
} // namespace geomsrv::archviz::buildingplan
