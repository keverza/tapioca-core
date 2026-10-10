#include "ArchViz/FloorSchemeDetail.hpp"
#include <array>
#include <tuple>
#include <numbers>

// T2-T4: access per wing, corridor runs, sections, stair cores, end caps and the band
// slots left between the circulation and the facade. A run is one wing (straight) or two
// wings meeting at a right-angled corner (one L turn), so a corridor never branches.
// Every box is drawn in the run's frame; a section is one stair with its own corridor.
namespace geomsrv::archviz::floorscheme::detail {
namespace {
constexpr double kMinArm = 10.0;

struct Ctx {
    const cp::PathsD& outline;
    const floorprogramme::Programme& programme;
    const Pins& pins;
    const Options& o;
    std::vector<Diagnostic>& notes;
    Layout out;
    std::vector<char> fill; // Fillable by 0.1 m
    bool Fits (double length) const
    {
        if (length < 0.05)
            return true;
        const size_t i = static_cast<size_t> (std::lround (length * 10));
        return i < fill.size () ? fill[i] != 0 : Fillable (programme, length);
    }
};
Frame Mirror (const Frame& f)
{
    Frame m = f;
    m.v = { -f.v.x, -f.v.y };
    return m;
}
Frame FlipU (const Frame& f, double length)
{
    Frame m = f;
    m.o = f.World (length, 0);
    m.u = { -f.u.x, -f.u.y };
    return m;
}
// Facade length on one side of a box: 0 at v0, 1 at u1, 2 at v1, 3 at u0.
double SideFacade (const Ctx& c, const Frame& f, Box b, int side)
{
    switch (side) {
        case 0:
            return FacadeAlong (c.outline, f.World (b.u0, b.v0), f.World (b.u1, b.v0), { -f.v.x, -f.v.y });
        case 1:
            return FacadeAlong (c.outline, f.World (b.u1, b.v0), f.World (b.u1, b.v1), f.u);
        case 2:
            return FacadeAlong (c.outline, f.World (b.u0, b.v1), f.World (b.u1, b.v1), f.v);
        default:
            return FacadeAlong (c.outline, f.World (b.u0, b.v0), f.World (b.u0, b.v1), { -f.u.x, -f.u.y });
    }
}
// u intervals of the v1 side that face outside.
std::vector<std::pair<double, double>> FacadeRuns (const Ctx& c, const Frame& f, Box b)
{
    std::vector<std::pair<double, double>> runs;
    const int samples = (std::max) (1, static_cast<int> (std::ceil (b.W () / 0.3)));
    const double step = b.W () / samples;
    for (int i = 0; i < samples; ++i) {
        const double u = b.u0 + (i + 0.5) * step;
        if (Inside (c.outline, f.World (u, b.v1 + kFacadeOffset)))
            continue;
        if (!runs.empty () && std::abs (runs.back ().second - (b.u0 + i * step)) < 1e-6)
            runs.back ().second = b.u0 + (i + 1) * step;
        else
            runs.emplace_back (b.u0 + i * step, b.u0 + (i + 1) * step);
    }
    return runs;
}
// A rectangle to divide, in frame f; `facadeHigh` says its facade is at v1 of f (else at v0,
// and the slot is mirrored so its facade is always v1). The door interval is in f too.
void AddSlot (Ctx& c, const Frame& f, Box b, bool facadeHigh, int section, int door = 0, int count = 0,
              bool darkCheck = true, double doorLo = -1e18, double doorHi = 1e18, Box extra = {})
{
    if (b.Empty ())
        return;
    Slot s;
    s.doorLo = doorLo, s.doorHi = doorHi;
    if (!facadeHigh && door != 0 && doorLo > -1e17)
        s.doorLo = -doorHi, s.doorHi = -doorLo; // an interval along v turns with the mirror
    s.frame = facadeHigh ? f : Mirror (f);
    s.box = facadeHigh ? b : Box { b.u0, -b.v1, b.u1, -b.v0 };
    if (!extra.Empty ())
        s.extra = facadeHigh ? extra : Box { extra.u0, -extra.v1, extra.u1, -extra.v0 };
    s.section = section, s.door = door, s.count = count;
    if (darkCheck && door == 0 && count == 0) {
        // A band whose facade is covered by another wing (a T or + junction) cannot hold
        // flats there: long dark stretches become storage, short ones stay in the band.
        const auto runs = FacadeRuns (c, s.frame, s.box);
        std::vector<std::pair<double, double>> dark;
        double from = s.box.u0;
        for (size_t i = 0; i <= runs.size (); ++i) {
            const double to = i < runs.size () ? runs[i].first : s.box.u1;
            if (to - from >= 2.0)
                dark.emplace_back (from, to);
            if (i < runs.size ())
                from = runs[i].second;
        }
        if (!dark.empty ()) {
            double start = s.box.u0;
            for (const auto& [g0, g1] : dark) {
                c.out.storage.push_back (
                    { ToRing (s.frame, { g0, s.box.v0, g1, s.box.v1 }), "band without facade (storage)" });
                if (g0 - start > 0.5)
                    AddSlot (c, s.frame, { start, s.box.v0, g0, s.box.v1 }, true, section, door, count, false, doorLo,
                             doorHi);
                start = g1;
            }
            if (s.box.u1 - start > 0.5)
                AddSlot (c, s.frame, { start, s.box.v0, s.box.u1, s.box.v1 }, true, section, door, count, false, doorLo,
                         doorHi);
            return;
        }
    }
    s.cornerLo = SideFacade (c, s.frame, s.box, 3) >= 0.5 * s.box.H ();
    s.cornerHi = SideFacade (c, s.frame, s.box, 1) >= 0.5 * s.box.H ();
    s.through = SideFacade (c, s.frame, s.box, 0) >= 0.5 * s.box.W ();
    s.band = static_cast<int> (c.out.slots.size ());
    c.out.slots.push_back (std::move (s));
}
// Facade parts of a band piece, split where the facade is covered for 2 m or more (as
// AddSlot splits them); each must take whole flats or be empty.
bool PieceFits (const Ctx& c, const Frame& f, Box b, bool facadeHigh)
{
    if (b.W () < 0.05)
        return true;
    const Frame sf = facadeHigh ? f : Mirror (f);
    const Box sb = facadeHigh ? b : Box { b.u0, -b.v1, b.u1, -b.v0 };
    const auto runs = FacadeRuns (c, sf, sb);
    double start = sb.u0, from = sb.u0;
    bool ok = true;
    for (size_t i = 0; i <= runs.size (); ++i) {
        const double to = i < runs.size () ? runs[i].first : sb.u1;
        if (to - from >= 2.0) {
            if (from - start > 0.5)
                ok = ok && c.Fits (from - start);
            start = to;
        }
        if (i < runs.size ())
            from = runs[i].second;
    }
    if (sb.u1 - start > 0.5)
        ok = ok && c.Fits (sb.u1 - start);
    return ok;
}
// A flat that owns a corridor end. Its rooms face the gable when the gable is a facade,
// otherwise the long side; the door is on the corridor end, where the corridor [cv0, cv1]
// (v in f) meets it.
void AddCap (Ctx& c, const Frame& f, Box b, bool doorHigh, int section, double cv0, double cv1)
{
    if (b.Empty ())
        return;
    const double gable = SideFacade (c, f, b, doorHigh ? 3 : 1);
    if (gable >= 0.6 * b.H ()) {
        Frame g;
        g.u = f.v;
        if (doorHigh)
            g.o = f.World (b.u1, b.v0), g.v = { -f.u.x, -f.u.y };
        else
            g.o = f.World (b.u0, b.v0), g.v = f.u;
        AddSlot (c, g, { 0, 0, b.H (), b.W () }, true, section, 0, 1, false, cv0 - b.v0, cv1 - b.v0);
    }
    else {
        const bool high = SideFacade (c, f, b, 2) >= SideFacade (c, f, b, 0);
        AddSlot (c, f, b, high, section, doorHigh ? 2 : 1, 1, false, cv0, cv1);
    }
    c.out.slots.back ().cap = true;
}
void AddCore (Ctx& c, const Frame& f, Box b, int section, bool pinned)
{
    Core core;
    core.shape = ToRing (f, b);
    core.centre = f.World ((b.u0 + b.u1) / 2, (b.v0 + b.v1) / 2);
    core.width = b.W (), core.depth = b.H ();
    core.section = section, core.pinned = pinned;
    c.out.cores.push_back (std::move (core));
}
void AddCorridor (Ctx& c, const Frame& f, const std::vector<Box>& boxes, const std::vector<Vec>& axis, int section)
{
    cp::PathsD parts;
    for (const auto& b : boxes)
        if (!b.Empty ())
            parts.push_back (ToPath (ToRing (f, b)));
    if (parts.empty ())
        return;
    const auto joined = cp::Union (parts, cp::FillRule::NonZero, kPrecision);
    if (joined.empty ())
        return;
    Corridor corridor;
    corridor.shape = Counter (FromPath (cp::SimplifyPath (joined.front (), 1e-4)));
    for (const auto& p : axis)
        corridor.axis.push_back (f.World (p.x, p.y));
    corridor.section = section;
    c.out.corridors.push_back (std::move (corridor));
}
int AddSection (Ctx& c, int run, double gross, char access)
{
    c.out.sections.push_back ({ run, gross, access });
    return static_cast<int> (c.out.sections.size ()) - 1;
}
Vec North (const Options& o)
{
    return { -std::sin (o.north), std::cos (o.north) };
}
// A pinned core in this box of the frame, nearest first; null when none.
const Pins::Core* PinIn (const Ctx& c, const Frame& f, Box b, double margin)
{
    const Pins::Core* best = nullptr;
    for (const auto& p : c.pins.cores) {
        const auto q = f.Local (p.centre);
        if (q.x >= b.u0 - margin && q.x <= b.u1 + margin && q.y >= b.v0 - 0.01 && q.y <= b.v1 + 0.01)
            best = best ? best : &p;
    }
    return best;
}
// Cap lengths from corridor-end pins near this section's ends.
void EndPins (const Ctx& c, const Frame& f, Box section, double c0, double c1, double& capLo, double& capHi)
{
    for (const auto& pin : c.pins.ends) {
        const auto q = f.Local (pin.at);
        if (q.y < c0 - 1 || q.y > c1 + 1 || q.x < section.u0 || q.x > section.u1)
            continue;
        const double lo = q.x - section.u0, hi = section.u1 - q.x;
        if (std::abs (lo - capLo) <= std::abs (hi - capHi))
            capLo = (std::max) (c.o.minCap * 0.7, lo);
        else
            capHi = (std::max) (c.o.minCap * 0.7, hi);
    }
}

// --- straight sections ------------------------------------------------------------------
// Centre corridor: core in the band on side `up` (true: v = depth), caps on that side too,
// so the other band runs on to both gables with corner flats.
void CentreSection (Ctx& c, const Frame& f, double depth, double s0, double s1, bool up, int run, int coreEnd = 0)
{
    if (coreEnd != 0) {
        // A short section: the stair takes the corridor end at its inner end (-1: s0, +1: s1),
        // so only the other end needs a cap.
        const double corr = c.o.corridor, w = c.o.coreWidth;
        const double c0 = (depth - corr) / 2, c1 = c0 + corr;
        const double coreLo = up ? c0 : 0, coreHi = up ? depth : c1;
        const double cap0 = Clip ((std::max) (up ? depth - c1 : c0, RoomFrontage (2).pref), c.o.minCap, c.o.maxCap);
        const double coreU = coreEnd > 0 ? s1 - w : s0;
        // Cap length so the band beside the corridor takes whole flats (or none).
        double cap = cap0;
        for (double d = 0; d <= 3.0; d += 0.1) {
            const double a = (s1 - s0 - w) - (cap0 - d), b = (s1 - s0 - w) - (cap0 + d);
            if (cap0 - d >= c.o.minCap && c.Fits (a)) {
                cap = cap0 - d;
                break;
            }
            if (c.Fits (b)) {
                cap = cap0 + d;
                break;
            }
        }
        const int id = AddSection (c, run, (s1 - s0) * depth, 'C');
        const double e0 = coreEnd > 0 ? s0 + cap : s0 + w, e1 = coreEnd > 0 ? s1 - w : s1 - cap;
        AddCore (c, f, { coreU, coreLo, coreU + w, coreHi }, id, false);
        AddCorridor (c, f, { { e0, c0, e1, c1 } }, { { e0, (c0 + c1) / 2 }, { e1, (c0 + c1) / 2 } }, id);
        const double capV0 = up ? c0 : 0, capV1 = up ? depth : c1;
        const double oppV0 = up ? 0 : c0, oppV1 = up ? c1 : depth;
        // The cap moves across the corridor when its own side has no facade.
        const Box sigma = coreEnd > 0 ? Box { s0, capV0, e0, capV1 } : Box { e1, capV0, s1, capV1 };
        const Box other = coreEnd > 0 ? Box { s0, oppV0, e0, oppV1 } : Box { e1, oppV0, s1, oppV1 };
        auto lit = [&] (Box b, bool high) {
            return SideFacade (c, f, b, high ? 2 : 0) + SideFacade (c, f, b, 3) + SideFacade (c, f, b, 1);
        };
        const bool flip = lit (sigma, up) < kBedroomMin && lit (other, !up) >= kBedroomMin;
        if (!flip && lit (sigma, up) < kBedroomMin)
            c.out.storage.push_back ({ ToRing (f, sigma), "corridor end without facade (storage)" });
        else
            AddCap (c, f, flip ? other : sigma, coreEnd > 0, id, c0, c1);
        const double b0 = flip && coreEnd > 0 ? s0 : e0, b1 = flip && coreEnd < 0 ? s1 : e1;
        AddSlot (c, f, { b0, up ? c1 : 0, b1, up ? depth : c0 }, up, id, 0, 0, true, e0, e1);
        const double r0 = flip && coreEnd > 0 ? e0 : s0, r1 = flip && coreEnd < 0 ? e1 : s1;
        if (up)
            AddSlot (c, f, { r0, 0, r1, c0 }, false, id, 0, 0, true, e0, e1);
        else
            AddSlot (c, f, { r0, c1, r1, depth }, true, id, 0, 0, true, e0, e1);
        return;
    }
    const double corr = c.o.corridor;
    const double c0 = (depth - corr) / 2, c1 = c0 + corr;
    const double coreLo = up ? c1 : 0, coreHi = up ? depth : c0;
    const double band = coreHi - coreLo;
    double capLo = Clip ((std::max) (band, RoomFrontage (2).pref), c.o.minCap, c.o.maxCap), capHi = capLo;
    EndPins (c, f, { s0, 0, s1, depth }, c0, c1, capLo, capHi);
    const int id = AddSection (c, run, (s1 - s0) * depth, 'C');
    const double e0 = s0 + capLo, e1 = s1 - capHi;
    const double w = c.o.coreWidth;
    const auto* pin = PinIn (c, f, { s0, 0, s1, depth }, kPinTolerance);
    const double wanted = pin ? f.Local (pin->centre).x - w / 2 : (e0 + e1 - w) / 2;
    // Caps take the corridor end and the core-side band; the door is the corridor end. A cap
    // whose side is covered by another wing (under the stem of a T) moves across the corridor.
    const double capV0 = up ? c0 : 0, capV1 = up ? depth : c1;
    const double oppV0 = up ? 0 : c0, oppV1 = up ? c1 : depth;
    auto lit = [&] (Box b, bool high) {
        return SideFacade (c, f, b, high ? 2 : 0) + SideFacade (c, f, b, 3) + SideFacade (c, f, b, 1);
    };
    const bool lowFlip =
        lit ({ s0, capV0, e0, capV1 }, up) < kBedroomMin && lit ({ s0, oppV0, e0, oppV1 }, !up) >= kBedroomMin;
    const bool highFlip =
        lit ({ e1, capV0, s1, capV1 }, up) < kBedroomMin && lit ({ e1, oppV0, s1, oppV1 }, !up) >= kBedroomMin;
    // Core along the corridor: pieces on both sides must take whole flats (or none),
    // and the corridor stays within the dead-end limit of the stair.
    const double range = pin ? kPinTolerance : (e1 - e0) / 2;
    double uc = wanted;
    bool fitted = false;
    // Among the positions that work, prefer pieces that take ordinary flats (or none): a
    // piece only a studio fits pushes the programme toward the smallest type.
    double bestCost = 1e18;
    const double ordinary = RoomFrontage (2).min;
    for (int k = -static_cast<int> (range / 0.1); k <= static_cast<int> (range / 0.1); ++k) {
        const double u = wanted + k * 0.1;
        if (u < e0 - 1e-9 || u > e1 - w + 1e-9)
            continue;
        const double p1 = u - e0, p2 = e1 - u - w;
        if (p1 > c.o.maxDeadEnd || p2 > c.o.maxDeadEnd)
            continue;
        if (SideFacade (c, f, { u, coreLo, u + w, coreHi }, up ? 2 : 0) < c.o.stairWindow)
            continue;
        if (!PieceFits (c, f, { lowFlip ? s0 : e0, coreLo, u, coreHi }, up) ||
            !PieceFits (c, f, { u + w, coreLo, highFlip ? s1 : e1, coreHi }, up))
            continue;
        auto small = [&] (double p) { return p >= 0.05 && p < ordinary ? 3.0 : 0.0; };
        const double cost = 0.05 * std::abs (u - wanted) + small (p1) + small (p2);
        if (cost < bestCost)
            bestCost = cost, uc = u, fitted = true;
    }
    if (!fitted) {
        uc = Clip (wanted, e0, e1 - w);
        c.notes.push_back ({ Diagnostic::Warning, "core.no_fit",
                             "No stair position leaves whole flats on both sides; the band beside it will not divide.",
                             f.World (uc + w / 2, (coreLo + coreHi) / 2) });
    }
    if (pin && std::abs (uc - wanted) > 1e-6)
        c.notes.push_back ({ Diagnostic::Info, "pin.core_moved", "A pinned stair moved along the corridor to fit.",
                             f.World (uc + w / 2, (coreLo + coreHi) / 2) });
    AddCore (c, f, { uc, coreLo, uc + w, coreHi }, id, pin != nullptr);
    AddCorridor (c, f, { { e0, c0, e1, c1 } }, { { e0, (c0 + c1) / 2 }, { e1, (c0 + c1) / 2 } }, id);
    // No facade on either side (the crossing of a +): the corridor ends in storage.
    auto cap = [&] (Box sigma, Box other, bool flip, bool doorHigh) {
        if (!flip && lit (sigma, up) < kBedroomMin && lit (other, !up) < kBedroomMin)
            c.out.storage.push_back ({ ToRing (f, sigma), "corridor end without facade (storage)" });
        else
            AddCap (c, f, flip ? other : sigma, doorHigh, id, c0, c1);
    };
    cap ({ s0, capV0, e0, capV1 }, { s0, oppV0, e0, oppV1 }, lowFlip, true);
    cap ({ e1, capV0, s1, capV1 }, { e1, oppV0, s1, oppV1 }, highFlip, false);
    AddSlot (c, f, { lowFlip ? s0 : e0, coreLo, uc, coreHi }, up, id, 0, 0, true, e0, e1);
    AddSlot (c, f, { uc + w, coreLo, highFlip ? s1 : e1, coreHi }, up, id, 0, 0, true, e0, e1);
    // The band that runs on to the gables: its end flats must still reach the corridor.
    const double r0 = lowFlip ? e0 : s0, r1 = highFlip ? e1 : s1;
    if (up)
        AddSlot (c, f, { r0, 0, r1, c0 }, false, id, 0, 0, true, e0, e1);
    else
        AddSlot (c, f, { r0, c1, r1, depth }, true, id, 0, 0, true, e0, e1);
}
// Corridor along the facade on side `up`; the stair takes the full depth in the middle and
// the corridor runs to it from both caps (two straight corridors meeting at the stair).
void OneSideSection (Ctx& c, const Frame& f, double depth, double s0, double s1, bool up, int run)
{
    const double corr = c.o.corridor, w = c.o.coreWidth;
    const double c0 = up ? depth - corr : 0, c1 = up ? depth : corr;
    double capLo = Clip (depth - corr, c.o.minCap, c.o.maxCap), capHi = capLo;
    EndPins (c, f, { s0, 0, s1, depth }, c0, c1, capLo, capHi);
    const int id = AddSection (c, run, (s1 - s0) * depth, 'O');
    const double e0 = s0 + capLo, e1 = s1 - capHi;
    const auto* pin = PinIn (c, f, { s0, 0, s1, depth }, kPinTolerance);
    const double wanted = pin ? f.Local (pin->centre).x - w / 2 : (e0 + e1 - w) / 2;
    double uc = Clip (wanted, e0, e1 - w);
    for (int k = 0; k <= 30; ++k) {
        const double u = Clip (wanted + (k % 2 ? 1 : -1) * (k / 2) * 0.1, e0, e1 - w);
        const double p1 = u - e0, p2 = e1 - u - w;
        if ((p1 < 0.05 || c.Fits (p1)) && (p2 < 0.05 || c.Fits (p2))) {
            uc = u;
            break;
        }
    }
    AddCore (c, f, { uc, 0, uc + w, depth }, id, pin != nullptr);
    const double mid = (c0 + c1) / 2;
    if (uc - e0 > 0.05)
        AddCorridor (c, f, { { e0, c0, uc, c1 } }, { { e0, mid }, { uc, mid } }, id);
    if (e1 - uc - w > 0.05)
        AddCorridor (c, f, { { uc + w, c0, e1, c1 } }, { { uc + w, mid }, { e1, mid } }, id);
    AddCap (c, f, { s0, 0, e0, depth }, true, id, c0, c1);
    AddCap (c, f, { e1, 0, s1, depth }, false, id, c0, c1);
    const double b0 = up ? 0 : corr, b1 = up ? depth - corr : depth;
    AddSlot (c, f, { e0, b0, uc, b1 }, !up, id);
    AddSlot (c, f, { uc + w, b0, e1, b1 }, !up, id);
}
// Just a stair (user sketches, 2026-10-10): the stair with its landing on the facade `up`,
// one or two flats behind it facing the other facade, and a through flat on each side whose
// hall reaches the landing. Four flats when the side halls step in beside the stair, three
// when one flat sits behind it, two (through flats only) when the wing is too shallow for a
// flat behind the stair.
struct Sectional {
    int units = 2;
    double side = 0, middle = 0; // side flat width; width of the zone behind the stair
};
// Programme area of the types with room counts in [lo, hi], or `fallback`.
double TypicalArea (const floorprogramme::Programme& p, double lo, double hi, double fallback)
{
    double sum = 0, weight = 0;
    for (const auto& t : p.types)
        if (t.rooms >= lo - 1e-9 && t.rooms <= hi + 1e-9)
            sum += floorprogramme::Target (t) * (t.share + 1e-3), weight += t.share + 1e-3;
    return weight > 0 ? sum / weight : fallback;
}
double WidthFor (double area, double depth)
{
    return (area + kPartyWall * depth) / (std::max) (1.0, depth - floorprogramme::kFacade);
}
// Sections along `length` and the plan of each: fewest stairs whose flats stay near the sizes
// of the sketch (2-3 rooms at the sides, 1-2 rooms behind the stair).
// `beyond`: mean depth of floor past the wing's two facades that the mould adds to the
// through flats (a skewed or stepped outline).
std::pair<int, Sectional> SectionalPlan (const Ctx& c, double depth, double length, double beyond)
{
    const double S = c.o.coreWidth, behind = depth - c.o.coreDepth;
    const bool middle = behind >= 4.5;
    const double sideT = WidthFor (TypicalArea (c.programme, 2, 3, 60), depth);
    const double midT = middle ? WidthFor (TypicalArea (c.programme, 1, 2, 38), behind) : 0;
    // Side flats are through flats: as narrow as the smallest through type (the programme check
    // below rules out the types that do not fit the width).
    const double sideLo = ThroughFrontage (1).min, sideHi = 10.0, midLo = RoomFrontage (1).min, midHi = 7.5;
    const double limit = (c.o.sectionArea + c.o.sectionSlack) / c.o.grossFactor;
    const double cap = MaxFlat (c.programme, c.o);
    double best = 1e18;
    std::pair<int, Sectional> out { 1, { 2, (std::max) (0.0, (length - S) / 2), S } };
    for (int n = 1; n <= (std::max) (1, static_cast<int> (length / 8)); ++n) {
        const double Ls = length / n;
        if (Ls * depth > limit + 1e-6)
            continue;
        // Score: how far each flat's net area falls from the nearest programme type (in m2,
        // over 10), plus a stair's worth of cost per section.
        // Only the types whose rooms fit the width count; none fitting rules the plan out.
        // `extra`: the hall that steps in beside the stair belongs to the side flat.
        auto miss = [&] (double width, double d, bool through, double extra = 0) {
            double best = 1e18;
            const double area = NetArea (width, d) + extra;
            if (area > cap + 0.5)
                return 1e18; // too large a flat: more sections instead
            for (const auto& t : c.programme.types) {
                const auto f = through ? ThroughFrontage (t.rooms) : RoomFrontage (t.rooms);
                if (width < f.min - 1e-6)
                    continue;
                const double m = area < t.minM2 ? t.minM2 - area : area > t.maxM2 ? area - t.maxM2 : 0;
                best = (std::min) (best, m);
            }
            return best / 10.0;
        };
        auto consider = [&] (int units, double M) {
            const double side = (Ls - M) / 2;
            if (side < sideLo - 1e-9 || side > sideHi + 1e-9)
                return;
            const double hall = units > 2 ? (M - S) / 2 * c.o.coreDepth : 0;
            double cost = 2 * miss (side, depth, depth >= 8.0, hall + side * beyond) + 2.0 * n;
            if (units == 4)
                cost += 2 * miss (M / 2, behind, false);
            if (units == 3)
                cost += miss (M, behind, false);
            cost -= 0.25 * units; // more flats per stair is cheaper
            (void) sideT, (void) midT;
            if (cost < best && cost < 1e17)
                best = cost, out = { n, { units, side, M } };
        };
        if (middle) {
            // The side halls step in beside the stair: each at least a hall's width.
            for (double M = (std::max) (2 * midLo, S + 2 * (kHallMin + kInnerWall)); M <= 2 * midHi + 1e-9; M += 0.1)
                consider (4, M);
            // One flat behind the stair, as wide as the stair or wide enough for halls beside it.
            consider (3, S);
            for (double M = S + 2 * (kHallMin + kInnerWall); M <= midHi + 1e-9; M += 0.1)
                consider (3, M);
        }
        else
            consider (2, S);
    }
    return out;
}
void CoreSection (Ctx& c, const Frame& f, double depth, double s0, double s1, bool up, int run, Sectional plan)
{
    const double S = c.o.coreWidth, ds = c.o.coreDepth;
    // The stair goes to the facade the wing's sections face unless its own stretch of that
    // facade is covered (a neighbouring wing at a corner) and the other side is open.
    {
        const double centre = (s0 + s1 - S) / 2;
        const Box stair { centre, 0, centre + S, depth };
        if (SideFacade (c, f, stair, up ? 2 : 0) < c.o.stairWindow &&
            SideFacade (c, f, stair, up ? 0 : 2) >= c.o.stairWindow)
            up = !up;
    }
    // Flats behind the stair face the other facade: where another wing covers it, the
    // section keeps only the two side flats.
    if (plan.units > 2 && SideFacade (c, f, { s0, 0, s1, depth }, up ? 0 : 2) < 0.6 * (s1 - s0))
        plan.units = 2;
    const int id = AddSection (c, run, (s1 - s0) * depth, 'S');
    const double middle = plan.units == 2 ? S : plan.middle;
    double x0 = s0 + (s1 - s0 - middle) / 2, x3 = x0 + middle;
    double uc = x0 + (middle - S) / 2;
    // A pinned stair moves the whole middle by up to the pin tolerance, keeping both sides.
    if (const auto* pin = PinIn (c, f, { s0, 0, s1, depth }, kPinTolerance)) {
        const double shift = Clip (f.Local (pin->centre).x - S / 2 - uc, -kPinTolerance, kPinTolerance);
        const double d = Clip (shift, -(std::max) (0.0, x0 - s0 - 4.5), (std::max) (0.0, s1 - x3 - 4.5));
        x0 += d, x3 += d, uc += d;
    }
    const double sv0 = up ? depth - ds : 0, sv1 = up ? depth : ds;
    const double mv0 = up ? 0 : ds, mv1 = up ? depth - ds : depth;
    if (plan.units == 2)
        AddCore (c, f, { uc, 0, uc + S, depth }, id, false);
    else
        AddCore (c, f, { uc, sv0, uc + S, sv1 }, id, false);
    if (plan.units == 3)
        AddSlot (c, f, { x0, mv0, x3, mv1 }, !up, id, 0, 1, false, uc, uc + S);
    if (plan.units == 4) {
        const double xm = (x0 + x3) / 2;
        AddSlot (c, f, { x0, mv0, xm, mv1 }, !up, id, 0, 1, false, uc, uc + S);
        AddSlot (c, f, { xm, mv0, x3, mv1 }, !up, id, 0, 1, false, uc, uc + S);
    }
    const double dv0 = plan.units == 2 ? 0 : sv0, dv1 = plan.units == 2 ? depth : sv1;
    const Box leftHall = plan.units > 2 && uc - x0 > 0.05 ? Box { x0, sv0, uc, sv1 } : Box {};
    const Box rightHall = plan.units > 2 && x3 - uc - S > 0.05 ? Box { uc + S, sv0, x3, sv1 } : Box {};
    AddSlot (c, f, { s0, 0, x0, depth }, up, id, 2, 1, false, dv0, dv1, leftHall);
    AddSlot (c, f, { x3, 0, s1, depth }, up, id, 1, 1, false, dv0, dv1, rightHall);
}

struct Portion {
    int wing = -1;
    double s0 = 0, s1 = 0;
};
// One wing span: split into sections by stair area and dead ends, then laid out.
void Straight (Ctx& c, const Skeleton& sk, Portion p, Access access, int run)
{
    const auto& wing = sk.wings[p.wing];
    const Frame& f = sk.frames[p.wing];
    const double depth = wing.depth, length = p.s1 - p.s0;
    if (length < 1.0)
        return;
    // Core and caps go to the side facing north, if it is a facade.
    const Box all { p.s0, 0, p.s1, depth };
    bool up = Dot (f.v, North (c.o)) >= 0;
    const double facadeUp = SideFacade (c, f, all, 2), facadeDown = SideFacade (c, f, all, 0);
    if ((up ? facadeUp : facadeDown) < 0.5 * length && (up ? facadeDown : facadeUp) >= 0.5 * length)
        up = !up;
    const double w = c.o.coreWidth;
    const double minCentre = c.o.minCap + w + 5.0;
    if (access == Access::Centre && length < minCentre)
        access = Access::CoreOnly;
    if (access == Access::OneSide && length < 2 * c.o.minCap + w)
        access = Access::CoreOnly;
    int n = 1;
    Sectional sectional;
    if (access == Access::CoreOnly) {
        // Floor past either facade within the mould's reach, averaged along the wing.
        double beyond = 0;
        int samples = 0;
        for (double u = p.s0 + 0.25; u < p.s1; u += 0.5, ++samples)
            for (int side = 0; side < 2; ++side) {
                double e = 0;
                while (e + 0.1 <= 3.5 && Inside (c.outline, f.World (u, side ? depth + e + 0.1 : -e - 0.1)))
                    e += 0.1;
                beyond += e < 3.4 ? e : 0;
            }
        std::tie (n, sectional) = SectionalPlan (c, depth, length, samples ? beyond / samples : 0);
    }
    else {
        n = (std::max) (1, static_cast<int> (std::ceil (
                               length * depth * c.o.grossFactor / (c.o.sectionArea + c.o.sectionSlack) - 1e-9)));
        const double reach = 2 * c.o.maxDeadEnd + w + 2 * c.o.maxCap;
        n = (std::max) (n, static_cast<int> (std::ceil (length / reach - 1e-9)));
        const double shortest = access == Access::Centre ? minCentre : 2 * c.o.minCap + w;
        n = (std::min) (n, (std::max) (1, static_cast<int> (length / shortest)));
    }
    // Pinned stairs each get a section; boundaries fall midway between them.
    std::vector<double> pinned;
    for (const auto& pin : c.pins.cores) {
        const auto q = f.Local (pin.centre);
        if (q.x >= p.s0 && q.x <= p.s1 && q.y >= -0.01 && q.y <= depth + 0.01)
            pinned.push_back (q.x);
    }
    std::sort (pinned.begin (), pinned.end ());
    std::vector<double> cuts { p.s0 };
    if (pinned.size () >= static_cast<size_t> (n) && !pinned.empty ()) {
        for (size_t i = 0; i + 1 < pinned.size (); ++i)
            cuts.push_back ((pinned[i] + pinned[i + 1]) / 2);
    }
    else
        for (int k = 1; k < n; ++k)
            cuts.push_back (p.s0 + length * k / n);
    cuts.push_back (p.s1);
    int previousEnd = 0;
    for (size_t k = 0; k + 1 < cuts.size (); ++k) {
        const double a = cuts[k], b = cuts[k + 1];
        if (access == Access::Centre && b - a < 2 * c.o.maxCap + w + 5.0) {
            // Short: the stair goes to an end that is not a gable, where it still has a window.
            const bool lowGable = SideFacade (c, f, { a, 0, b, depth }, 3) >= 0.5 * depth;
            const bool highGable = SideFacade (c, f, { a, 0, b, depth }, 1) >= 0.5 * depth;
            const double c0 = (depth - c.o.corridor) / 2;
            const Box core { 0, up ? c0 : 0, w, up ? depth : c0 + c.o.corridor };
            auto window = [&] (double u0) {
                const Box k { u0, core.v0, u0 + w, core.v1 };
                return SideFacade (c, f, k, up ? 2 : 0) + SideFacade (c, f, k, u0 <= a + 1e-6 ? 3 : 1);
            };
            const bool highOk = window (b - w) >= c.o.stairWindow, lowOk = window (a) >= c.o.stairWindow;
            // Never back to back with the previous section's stair.
            const bool lowFree = lowOk && previousEnd != 1;
            int end = 0;
            if (highOk && (!highGable || !lowFree || lowGable))
                end = 1;
            else if (lowFree)
                end = -1;
            CentreSection (c, f, depth, a, b, up, run, end);
            previousEnd = end;
            continue;
        }
        previousEnd = 0;
        if (access == Access::Centre)
            CentreSection (c, f, depth, cuts[k], cuts[k + 1], up, run);
        else if (access == Access::OneSide)
            OneSideSection (c, f, depth, cuts[k], cuts[k + 1], up, run);
        else
            CoreSection (c, f, depth, cuts[k], cuts[k + 1], up, run, sectional);
    }
}

// --- the L run --------------------------------------------------------------------------
struct Corner {
    int parent = -1, child = -1;
    int parentEnd = 0, childEnd = 0; // 0: u = 0, 1: u = length
    double score = 0;
};
// Frame of an L: u along the parent from the corner, the child on the +v side at u = 0.
Frame CornerFrame (const Skeleton& sk, const Corner& k)
{
    const auto& p = sk.wings[k.parent];
    Frame f = k.parentEnd == 0 ? sk.frames[k.parent] : FlipU (sk.frames[k.parent], p.length);
    double t = 0;
    for (const auto& q : sk.wings[k.child].rect)
        t += f.Local (q).y;
    t /= static_cast<double> (sk.wings[k.child].rect.size ());
    if (t < p.depth / 2) {
        f.o = f.World (0, p.depth);
        f.v = { -f.v.x, -f.v.y };
    }
    return f;
}
void LRun (Ctx& c, const Skeleton& sk, const Corner& k, Portion pa, Portion pb, int run, std::vector<Portion>& rest)
{
    const auto& A = sk.wings[k.parent];
    const auto& B = sk.wings[k.child];
    const Frame f = CornerFrame (sk, k);
    // Swapped frame for the child arm: u' runs up the arm.
    Frame g;
    g.o = f.o, g.u = f.v, g.v = f.u;
    const double DA = A.depth, DB = B.depth, corr = c.o.corridor, w = c.o.coreWidth;
    const double LAp = pa.s1 - pa.s0, LBp = pb.s1 - pb.s0; // portion lengths from the corner
    const double s = (std::min) (c.o.stairWindow, w);
    const double ca = (DA - corr) / 2, cb = (DB - corr) / 2;
    const double bandA = DA - ca - corr, bandB = DB - cb - corr;
    const double cap2 = RoomFrontage (2).pref;
    const double capA0 = Clip ((std::max) (bandA, cap2), c.o.minCap, c.o.maxCap),
                 capB0 = Clip ((std::max) (bandB, cap2), c.o.minCap, c.o.maxCap);
    // The stair stands in the inner corner and reaches past it for a window: along the
    // parent's inner facade (A), or else along the arm's (B).
    const bool alongA = FacadeAlong (c.outline, f.World (DB, DA), f.World (DB + s, DA), f.v) >= c.o.stairWindow - 0.05;
    const bool alongB =
        !alongA && FacadeAlong (c.outline, f.World (DB, DA), f.World (DB, DA + s), f.u) >= c.o.stairWindow - 0.05;
    const double coreU1 = alongB ? DB : DB + s, coreU0 = (std::max) (cb + corr, coreU1 - w);
    const double armStart = alongB ? s : 0.0; // the arm's inner band starts above the stair
    if (!alongA && !alongB) {
        // Neither inner facade is free at the corner: say how far a deeper stair would reach.
        double reach = 0;
        for (double d = s; d <= 12.0 && reach == 0; d += 0.3)
            if (FacadeAlong (c.outline, f.World (DB + d - 0.3, DA), f.World (DB + d, DA), f.v) > 0.2)
                reach = d;
        c.notes.push_back ({ Diagnostic::Warning, "stair.deepen",
                             reach > 0 ? "The corner stair has no window: make it about " +
                                             std::to_string (static_cast<int> (std::round (w + reach - s))) +
                                             " m long to reach the facade."
                                       : "The corner stair has no window: move it or deepen it to a facade.",
                             f.World (DB, DA) });
    }
    // Arm lengths: the band between the stair and the cap takes whole flats (or none), an
    // arm is cut only where the rest can be a section of its own, and the corner section
    // keeps to the stair area where it can.
    struct Arm {
        double length, cap;
        bool small = false; // only a studio fits beside the stair
    };
    const double minRest = c.o.maxCap + w + 5.0;
    auto arms = [&] (double full, double start, double cap0) {
        std::vector<Arm> out;
        std::vector<double> lengths;
        for (double len = start + c.o.minCap; len <= full - minRest + 1e-6; len += 0.1)
            lengths.push_back (len);
        lengths.push_back (full);
        const double ordinary = RoomFrontage (2).min;
        for (double at : lengths) {
            // First a cap that leaves an ordinary flat's width (or none) beside the stair,
            // then any cap that leaves whole flats.
            double best = -1;
            bool small = false;
            // A short arm may give its whole length to a larger cap (up to 4 m past the
            // usual maximum) rather than leave a studio beside the stair.
            for (int pass = 0; pass < 2 && best < 0; ++pass)
                for (double d = 0; d <= c.o.maxCap + 4 - c.o.minCap && best < 0; d += 0.1)
                    for (double cap : { cap0 - d, cap0 + d }) {
                        const double inner = at - start - cap;
                        const double most = c.o.maxCap + (pass == 0 ? 4 : 2);
                        if (cap < c.o.minCap - 1e-9 || cap > most + 1e-9 || inner < -1e-9 || inner > c.o.maxDeadEnd)
                            continue;
                        if (pass == 0 && inner >= 0.05 && inner < ordinary)
                            continue;
                        if (c.Fits (inner)) {
                            best = cap, small = pass == 1;
                            break;
                        }
                    }
            if (best >= 0)
                out.push_back ({ at, best, small });
        }
        if (out.empty ())
            out.push_back ({ full, Clip (full - start, 0.0, cap0), true });
        return out;
    };
    const auto optionsA = arms (LAp, coreU1, capA0), optionsB = arms (LBp, armStart, capB0);
    const double limit = (c.o.sectionArea + c.o.sectionSlack) / c.o.grossFactor;
    Arm armA = optionsA.back (), armB = optionsB.back ();
    double bestArea = -1;
    bool under = false;
    // Under the stair area: the largest corner section, a studio-only band counting as 40 m2
    // less; over it: the smallest.
    double bestValue = -1e18;
    for (const auto& x : optionsA)
        for (const auto& y : optionsB) {
            const double area = DA * x.length + DB * y.length;
            const bool fits = area <= limit + 1e-6;
            const double value = area - 40.0 * (x.small + y.small);
            if ((fits && (!under || value > bestValue)) || (!fits && !under && (bestArea < 0 || area < bestArea))) {
                armA = x, armB = y, bestArea = area, bestValue = value, under = fits;
            }
        }
    double LA = armA.length, LB = armB.length;
    double capA = armA.cap, capB = armB.cap;
    if (!under)
        c.notes.push_back ({ Diagnostic::Info, "section.over_area",
                             "The corner section is larger than one stair's area: its arms are too short to split.",
                             f.World (DB, DA) });
    // Remainders of the arms, in their wings' own coordinates.
    if (LA < LAp - 1e-6)
        rest.push_back (k.parentEnd == 0 ? Portion { k.parent, pa.s0 + LA, pa.s1 }
                                         : Portion { k.parent, pa.s0, pa.s1 - LA });
    if (LB < LBp - 1e-6)
        rest.push_back (k.childEnd == 0 ? Portion { k.child, pb.s0 + LB, pb.s1 }
                                        : Portion { k.child, pb.s0, pb.s1 - LB });
    double unused = 0;
    EndPins (c, f, { DB, 0, LA, DA }, ca, ca + corr, unused, capA);
    EndPins (c, g, { DA, 0, DA + LB, DB }, cb, cb + corr, unused, capB);
    const int id = AddSection (c, run, DA * LA + DB * LB, 'C');
    // The stair stands in the inner corner and reaches past it for a window.
    double ueA = (std::max) (LA - capA, coreU1), veB = (std::max) (DA + LB - capB, DA + armStart);
    if (alongB) {
        // Across the arm's inner band, from the lobby up past the parent's facade line.
        const double v0 = (std::max) (ca + corr, DA + s - w);
        AddCore (c, f, { cb + corr, v0, DB, DA + s }, id, false);
        if (v0 - (ca + corr) > 0.05)
            c.out.lobbies.push_back (ToRing (f, { cb + corr, ca + corr, DB, v0 }));
    }
    else {
        AddCore (c, f, { coreU0, ca + corr, coreU1, DA }, id, false);
        if (coreU0 - (cb + corr) > 0.05)
            c.out.lobbies.push_back (ToRing (f, { cb + corr, ca + corr, coreU0, DA }));
    }
    const double ma = ca + corr / 2, mb = cb + corr / 2;
    AddCorridor (c, f, { { cb, ca, ueA, ca + corr }, { cb, ca, cb + corr, veB } },
                 { { ueA, ma }, { mb, ma }, { mb, veB } }, id);
    AddCap (c, f, { ueA, ca, LA, DA }, false, id, ca, ca + corr);
    AddCap (c, g, { veB, cb, DA + LB, DB }, false, id, cb, cb + corr);
    AddSlot (c, f, { 0, 0, LA, ca }, false, id, 0, 0, true, cb, ueA); // outer band of the parent, round the corner
    AddSlot (c, g, { ca, 0, DA + LB, cb }, false, id, 0, 0, true, ca, veB); // outer band of the arm
    AddSlot (c, f, { coreU1, ca + corr, ueA, DA }, true, id);               // inner band of the parent
    AddSlot (c, g, { DA + armStart, cb + corr, veB, DB }, true, id);        // inner band of the arm
}
std::vector<Corner> Corners (const Skeleton& sk, const std::vector<Access>& access)
{
    std::vector<Corner> out;
    const double right = std::sin (10 * std::numbers::pi / 180);
    for (size_t i = 0; i < sk.wings.size (); ++i)
        for (size_t j = 0; j < sk.wings.size (); ++j) {
            if (i == j || access[i] != Access::Centre || access[j] != Access::Centre)
                continue;
            const auto& a = sk.wings[i];
            const auto& b = sk.wings[j];
            const auto& fa = sk.frames[i];
            const auto& fb = sk.frames[j];
            if (std::abs (Dot (fa.u, fb.u)) > right || a.depth > b.length)
                continue;
            for (int e = 0; e < 2; ++e) {
                const double u = e ? a.length : 0;
                const Vec p0 = fa.World (u, 0), p1 = fa.World (u, a.depth);
                const auto q0 = fb.Local (p0), q1 = fb.Local (p1);
                // The arm's end lies on one long side of the parent...
                const bool onSide =
                    (std::abs (q0.y - q1.y) < 0.05) && (std::abs (q0.y) < 0.05 || std::abs (q0.y - b.depth) < 0.05);
                if (!onSide)
                    continue;
                const double lo = (std::min) (q0.x, q1.x), hi = (std::max) (q0.x, q1.x);
                // ...flush with one of its ends.
                if (lo < 0.6 && lo > -0.6)
                    out.push_back ({ static_cast<int> (j), static_cast<int> (i), 0, e, a.length + b.length });
                else if (hi > b.length - 0.6 && hi < b.length + 0.6)
                    out.push_back ({ static_cast<int> (j), static_cast<int> (i), 1, e, a.length + b.length });
            }
        }
    std::sort (out.begin (), out.end (), [] (const Corner& a, const Corner& b) { return a.score > b.score; });
    return out;
}
Access AutoAccess (const Ctx& c, const Skeleton& sk, size_t i)
{
    const auto& w = sk.wings[i];
    for (const auto& pin : c.pins.access)
        if (pin.access != Access::Auto && Inside ({ ToPath (w.rect) }, pin.at))
            return pin.access;
    // Deep wings take a centre corridor; shallower ones are sections of 3-4 flats around a
    // stair (2 when too shallow to fit a flat behind the stair).
    return w.depth >= c.o.centreDepth - 1e-6 ? Access::Centre : Access::CoreOnly;
}
} // namespace

Layout Circulate (const cp::PathsD& outline, Skeleton& sk, const floorprogramme::Programme& programme, const Pins& pins,
                  const Options& o, std::vector<Diagnostic>& notes)
{
    Ctx c { outline, programme, pins, o, notes, {}, {} };
    c.fill.resize (1201);
    for (size_t i = 0; i < c.fill.size (); ++i)
        c.fill[i] = Fillable (programme, i / 10.0) ? 1 : 0;
    std::vector<Access> access (sk.wings.size ());
    for (size_t i = 0; i < sk.wings.size (); ++i)
        sk.wings[i].access = access[i] = AutoAccess (c, sk, i);
    // L runs: corners between two centre-corridor wings, each wing end used once. A wing
    // with a corner at both ends is split between its two L runs.
    std::vector<std::array<int, 2>> endUse (sk.wings.size (), { -1, -1 });
    std::vector<Corner> chosen;
    for (const auto& k : Corners (sk, access)) {
        if (endUse[k.parent][k.parentEnd] >= 0 || endUse[k.child][k.childEnd] >= 0)
            continue;
        // A user stair away from this corner turns the L back into straight runs.
        bool pinnedAway = false;
        for (const auto& pin : pins.cores) {
            const auto& wa = sk.wings[k.parent];
            const auto& wb = sk.wings[k.child];
            const bool inside = Inside ({ ToPath (wa.rect) }, pin.centre) || Inside ({ ToPath (wb.rect) }, pin.centre);
            const auto q = CornerFrame (sk, k).Local (pin.centre);
            pinnedAway =
                pinnedAway || (inside && std::hypot (q.x - wb.depth, q.y - wa.depth) > kPinTolerance + o.coreWidth);
        }
        if (pinnedAway)
            continue;
        endUse[k.parent][k.parentEnd] = endUse[k.child][k.childEnd] = static_cast<int> (chosen.size ());
        chosen.push_back (k);
    }
    // Portions: the part of each wing an L run takes from its corner end.
    auto portion = [&] (int wing, int end) {
        const auto& w = sk.wings[wing];
        const int other = endUse[wing][1 - end];
        if (other < 0)
            return Portion { wing, 0, w.length };
        // Split between the two corners: each corner zone is the other wing's depth when
        // this wing is the parent there.
        auto zone = [&] (int e) {
            const auto& k = chosen[endUse[wing][e]];
            return k.parent == wing ? sk.wings[k.child].depth : 0.0;
        };
        const double z0 = zone (0), z1 = zone (1);
        const double split = (z0 + w.length - z1) / 2;
        return end == 0 ? Portion { wing, 0, split } : Portion { wing, split, w.length };
    };
    std::vector<Portion> straight;
    int run = 0;
    for (const auto& k : chosen)
        LRun (c, sk, k, portion (k.parent, k.parentEnd), portion (k.child, k.childEnd), run++, straight);
    for (size_t i = 0; i < sk.wings.size (); ++i)
        if (endUse[i][0] < 0 && endUse[i][1] < 0)
            straight.push_back ({ static_cast<int> (i), 0, sk.wings[i].length });
    for (const auto& p : straight)
        Straight (c, sk, p, access[p.wing], run++);
    return std::move (c.out);
}
} // namespace geomsrv::archviz::floorscheme::detail
