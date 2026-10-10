#include "ArchViz/FloorSchemeDetail.hpp"
#include <cmath>
#include <functional>

// T5-T6: the facade is divided by room widths. A flat of n rooms is as wide as its
// living room, n - 1 bedrooms with 0.1 m walls between them and a 0.2 m party wall; its
// depth is the band's. The programme's area range narrows that width on each band, and
// the counts are kept for the whole massing floor. Inside each flat the rooms face the
// facade (the living room in the corner of a corner flat); hall, bath, storage and a WC
// line the corridor side.
namespace geomsrv::archviz::floorscheme {
namespace {
constexpr double kLivingMax = 4.5, kBedroomPref = 2.7, kBedroomMax = 3.3;
constexpr double kAlcoveMin = 1.5, kAlcovePref = 1.8, kAlcoveMax = 2.4;
double LivingPref (int rooms)
{
    return rooms <= 1 ? 3.5 : rooms == 2 ? 3.7 : 4.0;
}
} // namespace
Frontage RoomFrontage (double rooms)
{
    const int whole = (std::max) (1, static_cast<int> (std::floor (rooms + 1e-9)));
    const bool half = rooms - whole >= 0.5 - 1e-9;
    Frontage f;
    f.min = kLivingMin + (whole - 1) * (kInnerWall + kBedroomMin) + kPartyWall;
    f.pref = LivingPref (whole) + (whole - 1) * (kInnerWall + kBedroomPref) + kPartyWall;
    f.max = kLivingMax + (whole - 1) * (kInnerWall + kBedroomMax) + kPartyWall;
    if (half) {
        // The alcove has its own window, 1.6 m of wall away from the living room's.
        f.min += kInnerWall + kAlcoveMin, f.pref += kInnerWall + kAlcovePref, f.max += kInnerWall + kAlcoveMax;
    }
    return f;
}
Frontage ThroughFrontage (double rooms)
{
    // Living room (and a bedroom from 4 rooms) on one facade, the other bedrooms (and the
    // alcove) on the other: the wider of the two rows.
    const int whole = (std::max) (1, static_cast<int> (std::floor (rooms + 1e-9)));
    const bool half = rooms - whole >= 0.5 - 1e-9;
    const int lowBeds = whole >= 4 ? 1 : 0, highBeds = whole - 1 - lowBeds;
    auto row = [&] (double living, double bed, double alcove, int beds, bool withLiving, bool withAlcove) {
        int n = beds + (withLiving ? 1 : 0) + (withAlcove ? 1 : 0);
        return (withLiving ? living : 0) + beds * bed + (withAlcove ? alcove : 0) + (std::max) (0, n - 1) * kInnerWall +
               kPartyWall;
    };
    Frontage f;
    f.min = (std::max) (row (kLivingMin, kBedroomMin, kAlcoveMin, lowBeds, true, false),
                        row (0, kBedroomMin, kAlcoveMin, highBeds, false, half));
    f.pref = (std::max) (row (LivingPref (whole), kBedroomPref, kAlcovePref, lowBeds, true, false),
                         row (0, kBedroomPref, kAlcovePref, highBeds, false, half));
    f.max = (std::max) (row (kLivingMax, kBedroomMax, kAlcoveMax, lowBeds, true, false),
                        row (0, kBedroomMax, kAlcoveMax, highBeds, false, half));
    return f;
}
double NetArea (double frontage, double depth)
{
    return frontage * depth - kPartyWall * depth - floorprogramme::kFacade * frontage;
}
double MaxFlat (const floorprogramme::Programme& programme, const Options& options)
{
    double largest = 0;
    for (const auto& t : programme.types)
        largest = (std::max) (largest, t.maxM2);
    return (std::max) (options.maxFlat, largest);
}
} // namespace geomsrv::archviz::floorscheme

namespace geomsrv::archviz::floorscheme::detail {
bool Fillable (const floorprogramme::Programme& programme, double length)
{
    std::vector<std::pair<double, double>> one, sums;
    for (const auto& t : programme.types) {
        const auto f = RoomFrontage (t.rooms);
        one.emplace_back (f.min, f.max);
    }
    if (one.empty ())
        return false;
    sums = one;
    for (int k = 1; k < 24; ++k) {
        bool below = false;
        for (const auto& [lo, hi] : sums) {
            if (length >= lo - 1e-6 && length <= hi + 1e-6)
                return true;
            below = below || lo <= length;
        }
        if (!below)
            return false;
        std::vector<std::pair<double, double>> next;
        for (const auto& [a, b] : sums)
            for (const auto& [c, d] : one)
                if (a + c <= length + 1e-6)
                    next.emplace_back (a + c, b + d);
        std::sort (next.begin (), next.end ());
        sums.clear ();
        for (const auto& r : next)
            if (!sums.empty () && r.first <= sums.back ().second + 1e-9)
                sums.back ().second = (std::max) (sums.back ().second, r.second);
            else
                sums.push_back (r);
        if (sums.empty ())
            return false;
    }
    return false;
}

namespace {
struct Fit {
    size_t type = 0;
    double lo = 0, hi = 0, pref = 0;
    bool strict = true; // within the programme's area range
    double roomLo = 0;  // narrowest the rooms allow, when the area range cannot be met
};
// Widths each type may take on a band of `depth`: room widths, narrowed by its area range.
std::vector<Fit> Fits (const floorprogramme::Programme& p, double depth, bool through = false)
{
    std::vector<Fit> out;
    for (size_t t = 0; t < p.types.size (); ++t) {
        const auto& type = p.types[t];
        const auto room = through ? ThroughFrontage (type.rooms) : RoomFrontage (type.rooms);
        const double usable = (std::max) (0.5, depth - floorprogramme::kFacade);
        const double a0 = (type.minM2 + kPartyWall * depth) / usable, a1 = (type.maxM2 + kPartyWall * depth) / usable;
        Fit f { t, (std::max) (room.min, a0), (std::min) (room.max, a1), 0, true, room.min };
        if (f.lo > f.hi + 1e-9)
            f = { t, room.min, room.max, 0, false, room.min };
        f.pref = std::clamp (room.pref, f.lo, f.hi);
        out.push_back (f);
    }
    return out;
}
struct Plan {
    std::vector<size_t> fits; // index into Fits, in u order
    std::vector<double> widths;
    double score = 1e18;
};
// Widths for flats in u order: minimum first (the end flats at least as wide as it takes
// to reach the corridor), then toward the preferred room widths, then the maximum; past
// that the rooms grow wider than preferred (never narrower than minimum). Returns empty
// when even the minimum widths do not fit.
std::vector<double> Widths (const std::vector<Fit>& fits, const std::vector<size_t>& order, double length,
                            double& excess, double minFirst = 0, double minLast = 0, bool relaxed = false)
{
    std::vector<double> w, floor;
    double sum = 0;
    for (size_t k = 0; k < order.size (); ++k) {
        double lo = relaxed ? fits[order[k]].roomLo : fits[order[k]].lo;
        if (k == 0)
            lo = (std::max) (lo, minFirst);
        if (k + 1 == order.size ())
            lo = (std::max) (lo, minLast);
        w.push_back (lo), floor.push_back (lo), sum += lo;
    }
    double slack = length - sum;
    if (slack < -1e-6)
        return {};
    for (int phase = 0; phase < 2 && slack > 1e-9; ++phase) {
        double room = 0;
        for (size_t k = 0; k < order.size (); ++k)
            room += (std::max) (0.0, (phase ? fits[order[k]].hi : fits[order[k]].pref) - w[k]);
        if (room <= 1e-9)
            continue;
        const double take = (std::min) (1.0, slack / room);
        for (size_t k = 0; k < order.size (); ++k) {
            const double add = (std::max) (0.0, (phase ? fits[order[k]].hi : fits[order[k]].pref) - w[k]) * take;
            w[k] += add, slack -= add;
        }
    }
    excess = (std::max) (0.0, slack);
    for (auto& x : w)
        x += excess / static_cast<double> (w.size ());
    return w;
}
struct Tally {
    std::vector<int> target, made;
};
// How far the counts so far, with `chosen` added, stray from the programme at the same
// progress: the mix is kept proportional as the floor fills, band by band.
double MixScore (const Tally& tally, const std::vector<Fit>& fits, const std::vector<size_t>& chosen)
{
    std::vector<int> add (tally.target.size (), 0);
    for (size_t i : chosen)
        ++add[fits[i].type];
    double made = 0, wanted = 0;
    for (size_t t = 0; t < add.size (); ++t)
        made += tally.made[t] + add[t], wanted += tally.target[t];
    const double progress = wanted > 0 ? made / wanted : 1;
    double score = 0;
    for (size_t t = 0; t < add.size (); ++t) {
        const double off = tally.made[t] + add[t] - tally.target[t] * progress;
        score += off * off;
        score += 3.0 * (std::max) (0, tally.made[t] + add[t] - tally.target[t]); // beyond the whole floor's count
    }
    return score;
}
// Larger flats to the corners: the order of the multiset along u.
std::vector<size_t> Order (const floorprogramme::Programme& p, const std::vector<Fit>& fits, std::vector<size_t> chosen,
                           bool cornerLo, bool cornerHi)
{
    std::stable_sort (chosen.begin (), chosen.end (),
                      [&] (size_t a, size_t b) { return p.types[fits[a].type].rooms > p.types[fits[b].type].rooms; });
    if (cornerHi && !cornerLo)
        std::reverse (chosen.begin (), chosen.end ());
    else if (cornerLo && cornerHi && chosen.size () > 2) {
        // Largest at u0, second largest at u1, the rest between.
        std::vector<size_t> out { chosen[0] };
        out.insert (out.end (), chosen.begin () + 2, chosen.end ());
        out.push_back (chosen[1]);
        chosen = out;
    }
    return chosen;
}
// Floor beyond a band's long sides that the mould will add to the flat in front of it, sampled
// every `kBeyondStep` along u from the span's start.
constexpr double kBeyondStep = 0.25;
double Beyond (const std::vector<double>& beyond, double a, double b)
{
    double sum = 0;
    for (size_t k = 0; k < beyond.size (); ++k) {
        const double u = (static_cast<double> (k) + 0.5) * kBeyondStep;
        if (u >= a && u < b)
            sum += beyond[k] * kBeyondStep;
    }
    return sum;
}
Plan Best (const floorprogramme::Programme& p, const std::vector<Fit>& fits, const Tally& tally, double length,
           double depth, double cap, const std::vector<double>& beyond, double gableLo, double gableHi, int count,
           bool cornerLo, bool cornerHi, double minFirst, double minLast)
{
    Plan best;
    std::vector<size_t> chosen;
    size_t visits = 0;
    std::function<void (size_t, double)> walk = [&] (size_t i, double sumLo) {
        if (++visits > 200000)
            return;
        if (i == fits.size ()) {
            if (chosen.empty () || (count > 0 && static_cast<int> (chosen.size ()) != count))
                return;
            const auto order = Order (p, fits, chosen, cornerLo, cornerHi);
            double excess = 0;
            bool relaxed = false;
            auto w = Widths (fits, order, length, excess, minFirst, minLast);
            if (w.empty ()) {
                // Below the area range but no narrower than the rooms allow: a small flat.
                w = Widths (fits, order, length, excess, minFirst, minLast, true);
                relaxed = true;
            }
            if (w.empty ())
                return;
            double score = MixScore (tally, fits, chosen) + 10.0 * excess + (relaxed ? 3.0 : 0.0);
            for (size_t k = 0; k < order.size (); ++k)
                score += 0.3 * std::abs (w[k] - fits[order[k]].pref) + (fits[order[k]].strict ? 0 : 2.0);
            // A flat over the cap, with what the mould will add, costs more than any mix: more,
            // smaller flats instead.
            double at = 0;
            for (size_t k = 0; k < w.size (); ++k) {
                const double gable = (k == 0 ? gableLo : 0) + (k + 1 == w.size () ? gableHi : 0);
                score += 50.0 * (std::max) (0.0, NetArea (w[k], depth) + Beyond (beyond, at, at + w[k]) + gable - cap);
                at += w[k];
            }
            if (score < best.score) {
                best.score = score;
                best.fits = order;
                best.widths = w;
            }
            return;
        }
        for (int c = 0; sumLo + c * fits[i].roomLo <= length + 1e-6; ++c) {
            if (count > 0 && static_cast<int> (chosen.size ()) > count)
                break;
            walk (i + 1, sumLo + c * fits[i].roomLo);
            chosen.push_back (i);
        }
        while (!chosen.empty () && chosen.back () == i)
            chosen.pop_back ();
    };
    walk (0, 0);
    return best;
}

struct Builder {
    const floorprogramme::Programme& p;
    const Pins& pins;
    Scheme& scheme;
    Tally tally;
    double cap = 90.0;         // largest net area of a flat
    cp::PathsD outline, taken; // the floor; circulation and every band
};
// Depth of free floor beyond both long sides of `span`, out to the outline (as the mould's
// strips reach, 3.5 m), every kBeyondStep.
std::vector<double> Profile (const Builder& b, const Slot& slot, const Box& span)
{
    std::vector<double> out;
    for (double u = span.u0 + kBeyondStep / 2; u < span.u1; u += kBeyondStep) {
        double sum = 0;
        for (int side = 0; side < 2; ++side) {
            double e = 0;
            while (e + 0.1 <= 3.5) {
                const Vec q = slot.frame.World (u, side ? span.v1 + e + 0.1 : span.v0 - e - 0.1);
                if (!Inside (b.outline, q) || Inside (b.taken, q))
                    break;
                e += 0.1;
            }
            sum += e < 3.4 ? e : 0; // a strip that does not reach the outline stays out
        }
        out.push_back (sum);
    }
    return out;
}
// Free floor beyond a span's end (`hi` at u1, else at u0), out to the outline within 2 m (the
// gable strip the mould gives the end flat).
double Gable (const Builder& b, const Slot& slot, const Box& span, bool hi)
{
    double sum = 0;
    for (double v = span.v0 + kBeyondStep / 2; v < span.v1; v += kBeyondStep) {
        double e = 0;
        while (e + 0.1 <= 2.0) {
            const Vec q = slot.frame.World (hi ? span.u1 + e + 0.1 : span.u0 - e - 0.1, v);
            if (!Inside (b.outline, q) || Inside (b.taken, q))
                break;
            e += 0.1;
        }
        sum += e < 1.9 ? e * kBeyondStep : 0;
    }
    return sum;
}
void Emit (Builder& b, const Slot& slot, const std::vector<Fit>& fits, const std::vector<size_t>& order,
           const std::vector<double>& widths)
{
    double u = slot.box.u0;
    for (size_t k = 0; k < order.size (); ++k) {
        const auto& fit = fits[order[k]];
        const Box box { u, slot.box.v0, k + 1 == order.size () ? slot.box.u1 : u + widths[k], slot.box.v1 };
        u = box.u1;
        Flat flat;
        flat.shape = ToRing (slot.frame, box);
        flat.type = fit.type;
        flat.rooms = b.p.types[fit.type].rooms;
        flat.frontage = box.W (), flat.depth = box.H ();
        flat.gross = box.W () * box.H ();
        flat.net = NetArea (box.W (), box.H ());
        flat.band = slot.cap ? -1 : slot.band;
        flat.section = slot.section;
        flat.axis = slot.frame.u;
        flat.cap = slot.cap;
        flat.through = slot.through;
        const bool lo = k == 0 && slot.cornerLo, hi = k + 1 == order.size () && slot.cornerHi;
        flat.corner = lo || hi;
        const auto& type = b.p.types[fit.type];
        flat.inRange = flat.net >= type.minM2 - 0.5 && flat.net <= type.maxM2 + 0.5;
        LayRooms (flat, slot, box, lo ? 0 : hi ? 1 : -1, { slot.door, slot.doorLo, slot.doorHi });
        if (!slot.extra.Empty ()) {
            // Sectional side flat: its hall steps in beside the stair to reach the landing.
            const Box e = slot.extra;
            // Joined in the slot's own frame, where both are exact axis-aligned boxes; in world
            // coordinates the corner of one lands a rounding error off the other's edge.
            auto local = [] (Box r) {
                return cp::PathD { { r.u0, r.v0 }, { r.u1, r.v0 }, { r.u1, r.v1 }, { r.u0, r.v1 } };
            };
            const auto joined = cp::Union ({ local (box), local (e) }, cp::FillRule::NonZero, kPrecision);
            if (joined.size () == 1) {
                Ring ring;
                for (const auto& q : cp::SimplifyPath (joined.front (), 1e-4))
                    ring.push_back (slot.frame.World (q.x, q.y));
                flat.shape = Counter (ring);
            }
            flat.gross += e.W () * e.H (), flat.net += e.W () * e.H ();
            flat.roomList.push_back ({ ToRing (slot.frame, e), RoomKind::Hall, e.W (), e.H () });
            const double du = slot.door == 2 ? e.u1 : e.u0, mid = (e.v0 + e.v1) / 2;
            flat.door = { slot.frame.World (du, mid - 0.45), slot.frame.World (du, mid + 0.45), RoomKind::Hall };
        }
        ++b.tally.made[fit.type];
        b.scheme.flats.push_back (std::move (flat));
    }
}
bool Contains (const Slot& s, Vec p, double margin)
{
    const auto q = s.frame.Local (p);
    return q.x >= s.box.u0 - margin && q.x <= s.box.u1 + margin && q.y >= s.box.v0 - margin && q.y <= s.box.v1 + margin;
}
// Divide one free band [u0, u1] of `slot`.
bool DivideSpan (Builder& b, const Slot& slot, Box span, int count, bool cornerLo, bool cornerHi)
{
    Slot part = slot;
    part.box = span;
    part.cornerLo = cornerLo, part.cornerHi = cornerHi;
    // Every flat opens off the corridor: the end flats reach at least a door into it.
    constexpr double door = 1.2;
    const double reach = (std::min) (span.u1, slot.doorHi) - (std::max) (span.u0, slot.doorLo);
    if (reach < door - 1e-6)
        return false;
    const double minFirst = (std::max) (0.0, slot.doorLo + door - span.u0);
    const double minLast = (std::max) (0.0, span.u1 - (slot.doorHi - door));
    const auto fits = Fits (b.p, span.H ());
    const auto plan =
        Best (b.p, fits, b.tally, span.W (), span.H (), b.cap, Profile (b, slot, span), Gable (b, slot, span, false),
              Gable (b, slot, span, true), count, cornerLo, cornerHi, minFirst, minLast);
    if (plan.fits.empty ())
        return false;
    auto order = plan.fits;
    const auto& widths = plan.widths;
    // A room-count pin retypes the flat under it when its width allows that many rooms.
    double u = span.u0;
    for (size_t k = 0; k < order.size (); ++k) {
        const double u1 = u + widths[k];
        for (const auto& pin : b.pins.rooms) {
            const auto q = slot.frame.Local (pin.at);
            if (q.x < u || q.x > u1 || q.y < span.v0 || q.y > span.v1)
                continue;
            size_t pick = fits.size ();
            const double net = NetArea (widths[k], span.H ());
            for (size_t j = 0; j < fits.size (); ++j) {
                const auto& t = b.p.types[fits[j].type];
                if (std::abs (t.rooms - pin.rooms) > 1e-6 || widths[k] < RoomFrontage (t.rooms).min - 1e-6)
                    continue;
                if (pick == fits.size () || std::abs (floorprogramme::Target (t) - net) <
                                                std::abs (floorprogramme::Target (b.p.types[fits[pick].type]) - net))
                    pick = j;
            }
            if (pick < fits.size ())
                order[k] = pick;
            else
                b.scheme.diagnostics.push_back ({ Diagnostic::Warning, "pin.rooms",
                                                  "The pinned room count does not fit this flat's width.", pin.at });
        }
        u = u1;
    }
    Emit (b, part, fits, order, widths);
    return true;
}
} // namespace

void Divide (const floorprogramme::Programme& p, const std::vector<Slot>& slots, Scheme& scheme, const Pins& pins,
             const Options& o, bool resume)
{
    Builder b { p, pins, scheme, {}, MaxFlat (p, o), ToPaths (scheme.outline), {} };
    for (const auto& c : scheme.corridors)
        b.taken.push_back (ToPath (c.shape));
    for (const auto& c : scheme.cores)
        b.taken.push_back (ToPath (c.shape));
    for (const auto& l : scheme.lobbies)
        b.taken.push_back (ToPath (l));
    for (const auto& f : scheme.flats)
        b.taken.push_back (ToPath (f.shape));
    for (const auto& slot : slots)
        b.taken.push_back (ToPath (ToRing (slot.frame, slot.box)));
    b.taken = cp::Union (b.taken, cp::FillRule::NonZero, kPrecision);
    // Flat count for the whole floor from the frontage it offers, then counts by share.
    double estimate = 0;
    double mean = 0;
    for (const auto& t : p.types)
        mean += t.share * RoomFrontage (t.rooms).pref;
    for (const auto& s : slots)
        estimate += s.count > 0 ? s.count : s.box.W () / (std::max) (1.0, mean);
    b.tally.target = floorprogramme::Counts (p, static_cast<int> (std::round (estimate)));
    b.tally.made.assign (p.types.size (), 0);
    if (resume && scheme.targets.size () == p.types.size () && scheme.counts.size () == p.types.size ()) {
        // The whole floor's counts grow by what the extra floor offers.
        const auto more = b.tally.target;
        b.tally.target = scheme.targets, b.tally.made = scheme.counts;
        int total = 0;
        for (int n : b.tally.target)
            total += n;
        int extra = 0;
        for (int n : more)
            extra += n;
        b.tally.target = floorprogramme::Counts (p, total + extra);
    }
    // Fixed flats first (caps, sectional and studio flats), then bands, longest first.
    std::vector<size_t> order (slots.size ());
    for (size_t i = 0; i < order.size (); ++i)
        order[i] = i;
    std::stable_sort (order.begin (), order.end (), [&] (size_t a, size_t c) {
        const bool fa = slots[a].count > 0, fc = slots[c].count > 0;
        return fa != fc ? fa : slots[a].box.W () > slots[c].box.W ();
    });
    for (size_t i : order) {
        const auto& slot = slots[i];
        if (slot.count > 0) {
            // A through flat (two facades, entered at its end) holds its rooms in two rows.
            const bool through = slot.through && slot.door != 0 && slot.box.H () >= 8.0;
            const auto fits = Fits (p, slot.box.H (), through);
            Plan best;
            // Rooms that do not fit the box (a through flat whose hall needs a row) rule a type out.
            auto lays = [&] (size_t k) {
                Flat trial;
                trial.rooms = p.types[fits[k].type].rooms;
                LayRooms (trial, slot, slot.box,
                          slot.cornerLo   ? 0
                          : slot.cornerHi ? 1
                                          : -1,
                          { slot.door, slot.doorLo, slot.doorHi });
                for (const auto& r : trial.roomList)
                    for (const auto& q : r.shape) {
                        const auto l = slot.frame.Local (q);
                        if (l.x < slot.box.u0 - 1e-3 || l.x > slot.box.u1 + 1e-3)
                            return false;
                    }
                return true;
            };
            for (size_t k = 0; k < fits.size (); ++k) {
                const bool room = slot.box.W () >= fits[k].roomLo - 1e-6;
                if (!room || !lays (k))
                    continue;
                const bool strict = slot.box.W () >= fits[k].lo - 1e-6 && slot.box.W () <= fits[k].hi + 1e-6;
                // A type whose area range misses this fixed width costs more than the mix gains.
                double score = MixScore (b.tally, fits, { k }) + (strict && fits[k].strict ? 0 : 50.0) +
                               0.3 * std::abs (slot.box.W () - fits[k].pref);
                for (const auto& pin : pins.rooms)
                    if (Contains (slot, pin.at, 0) && std::abs (pin.rooms - p.types[fits[k].type].rooms) > 1e-6)
                        score += 100;
                if (score < best.score)
                    best.score = score, best.fits = { k };
            }
            if (best.fits.empty ()) {
                scheme.unassigned.push_back ({ ToRing (slot.frame, slot.box), "too narrow for the smallest flat" });
                continue;
            }
            Emit (b, slot, fits, best.fits, { slot.box.W () });
            continue;
        }
        // Pinned party walls split the band; each part must still divide (a wall may move
        // by up to the pin tolerance to make it so).
        std::vector<double> cuts;
        for (const auto& pin : pins.walls)
            if (Contains (slot, pin.at, 0.3))
                cuts.push_back (std::clamp (slot.frame.Local (pin.at).x, slot.box.u0, slot.box.u1));
        std::sort (cuts.begin (), cuts.end ());
        std::vector<double> edges { slot.box.u0 };
        for (double cut : cuts) {
            double best = cut;
            for (double d = 0; d <= kPinTolerance + 1e-9; d += 0.1) {
                if (Fillable (p, cut - d - edges.back ())) {
                    best = cut - d;
                    break;
                }
                if (Fillable (p, cut + d - edges.back ())) {
                    best = cut + d;
                    break;
                }
            }
            if (std::abs (best - cut) > 1e-6)
                scheme.diagnostics.push_back ({ Diagnostic::Info, "pin.wall_moved",
                                                "A pinned wall moved to leave whole flats.",
                                                slot.frame.World (best, (slot.box.v0 + slot.box.v1) / 2) });
            edges.push_back (best);
        }
        edges.push_back (slot.box.u1);
        for (size_t k = 0; k + 1 < edges.size (); ++k) {
            const Box span { edges[k], slot.box.v0, edges[k + 1], slot.box.v1 };
            if (span.W () < 0.3)
                continue;
            const bool lo = k == 0 && slot.cornerLo, hi = k + 2 == edges.size () && slot.cornerHi;
            // A flat-count pin holds the span between pinned walls (or the band) it lies in.
            int count = 0;
            for (const auto& pin : pins.counts) {
                const auto q = slot.frame.Local (pin.at);
                if (Contains (slot, pin.at, 0) && q.x >= span.u0 - 1e-6 && q.x <= span.u1 + 1e-6)
                    count = (std::max) (1, pin.flats);
            }
            if (!DivideSpan (b, slot, span, count, lo, hi))
                scheme.unassigned.push_back ({ ToRing (slot.frame, span), "band too short for a whole flat" });
        }
    }
    scheme.targets = b.tally.target;
    scheme.counts = b.tally.made;
}
} // namespace geomsrv::archviz::floorscheme::detail
