#include "ArchViz/FloorSchemeRuns.hpp"
#include <numbers>

// T3 at corners: an L run (two wings at a right angle, one corridor with one turn, the stair in
// the inner corner) and a bend (an arm at an angle sharing its parent's corridor).
namespace geomsrv::archviz::floorscheme::detail::runs {
// --- the L run --------------------------------------------------------------------------
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
    // A stub arm too shallow for a centre corridor is entered from a corridor along its inner
    // side, its flats facing the outer facade (site B's 12 m end wings, user 2026-10-10).
    const bool single = k.single;
    const double ca = (DA - corr) / 2, cb = single ? DB - corr : (DB - corr) / 2;
    const double bandA = DA - ca - corr, bandB = DB - cb - corr;
    // The arm's outer band produced down over the corner square only makes a deep, dark corner
    // flat: that bay is left out of the massing (user, 2026-10-10), so the parent's outer band
    // and the arm's both end on a new gable beside the corridor's turn.
    const Box bay { 0, 0, cb, DA };
    const bool cull =
        c.o.cullCorners && !single && SideFacade (c, f, bay, 3) >= 0.8 * DA && SideFacade (c, f, bay, 0) >= 0.8 * cb;
    if (cull) {
        c.out.culled.push_back ({ ToRing (f, bay), "outer corner bay of an L: leave it out of the massing" });
        c.outline = cp::Difference (c.outline, { ToPath (ToRing (f, bay)) }, cp::FillRule::NonZero, kPrecision);
    }
    const double cap2 = RoomFrontage (2).pref;
    const double capA0 = Clip ((std::max) (bandA, cap2), c.o.minCap, c.o.maxCap),
                 capB0 = Clip ((std::max) (bandB, cap2), c.o.minCap, c.o.maxCap);
    // The stair stands in the inner corner and reaches past it for a window: along the
    // parent's inner facade (A), or else along the arm's (B).
    // A stub's corridor fills its inner side: the stair stands wholly along the parent's.
    const bool alongA =
        FacadeAlong (c.outline, f.World (DB, DA), f.World (DB + (single ? w : s), DA), f.v) >= c.o.stairWindow - 0.05;
    const bool alongB = !single && !alongA &&
                        FacadeAlong (c.outline, f.World (DB, DA), f.World (DB, DA + s), f.u) >= c.o.stairWindow - 0.05;
    const double coreU1 = single ? DB + w : alongB ? DB : DB + s, coreU0 = (std::max) (cb + corr, coreU1 - w);
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
        bool lobby = false; // the corridor runs on to `reach` with a lobby, not flats, beside it
    };
    const double minRest = c.o.maxCap + w + 5.0;
    // `reach`: corridor the arm needs past the corner, so its outer band still has a door when
    // the corner bay is culled; on a short arm what lies beside that stretch is a lobby.
    // `open`: no inner band (a stub), so any corridor length beside the cap will do.
    auto arms = [&] (double full, double start, double cap0, double reach, bool open) {
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
            bool small = false, lobby = false;
            // A short arm may give its whole length to a larger cap (up to 4 m past the
            // usual maximum) rather than leave a studio beside the stair.
            for (int pass = 0; pass < 2 && best < 0; ++pass)
                for (double d = 0; d <= c.o.maxCap + 4 - c.o.minCap && best < 0; d += 0.1)
                    for (double cap : { cap0 - d, cap0 + d }) {
                        double inner = at - start - cap;
                        const double most = c.o.maxCap + (pass == 0 ? 4 : 2);
                        if (cap < c.o.minCap - 1e-9 || cap > most + 1e-9 || inner < -1e-9 || inner > c.o.maxDeadEnd)
                            continue;
                        const bool short_ = inner + start < reach - 1e-9;
                        if (short_ && (inner >= 0.05 || at - reach < c.o.minCap - 1e-9))
                            continue;
                        if (short_)
                            cap = at - reach, inner = 0;
                        if (pass == 0 && inner >= 0.05 && inner < ordinary && !open)
                            continue;
                        if (open || c.Fits (inner)) {
                            best = cap, small = pass == 1, lobby = short_;
                            break;
                        }
                    }
            if (best >= 0)
                out.push_back ({ at, best, small, lobby });
        }
        if (out.empty ())
            out.push_back ({ full, Clip (full - start, 0.0, cap0), true });
        return out;
    };
    const auto optionsA = arms (LAp, coreU1, capA0, 0, false),
               optionsB = arms (LBp, armStart, capB0, cull ? 1.3 : 0, single);
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
    // A stub's cap spans it: its outer band stops where the corridor does.
    AddCap (c, g, { veB, single ? 0 : cb, DA + LB, DB }, false, id, cb, cb + corr);
    // Outer bands: the parent's runs round the corner unless the corner bay is culled.
    AddSlot (c, f, { cull ? cb : 0, 0, LA, ca }, false, id, 0, 0, true, cb, ueA);
    AddSlot (c, g, { cull ? DA : ca, 0, single ? veB : DA + LB, cb }, false, id, 0, 0, true, cull ? DA : ca, veB);
    AddSlot (c, f, { coreU1, ca + corr, ueA, DA }, true, id); // inner band of the parent
    if (armB.lobby)
        c.out.lobbies.push_back (ToRing (g, { DA + armStart, cb + corr, veB, DB }));
    else
        AddSlot (c, g, { DA + armStart, cb + corr, veB, DB }, true, id); // inner band of the arm
}
std::vector<Corner> Corners (const Skeleton& sk, const std::vector<Access>& access, double corridor)
{
    std::vector<Corner> out;
    const double right = std::sin (10 * std::numbers::pi / 180);
    for (size_t i = 0; i < sk.wings.size (); ++i)
        for (size_t j = 0; j < sk.wings.size (); ++j) {
            // A short shallow arm (a stub up to about twice its depth) joins as a single-loaded arm.
            const bool stub = access[i] == Access::CoreOnly && sk.wings[i].parent == static_cast<int> (j) &&
                              sk.wings[i].joint == 'L' && sk.wings[i].length <= 2 * sk.wings[i].depth + 4.0 &&
                              sk.wings[i].depth - corridor >= kRowMin;
            if (i == j || access[j] != Access::Centre || (access[i] != Access::Centre && !stub))
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
                    out.push_back ({ static_cast<int> (j), static_cast<int> (i), 0, e, a.length + b.length, stub });
                else if (hi > b.length - 0.6 && hi < b.length + 0.6)
                    out.push_back ({ static_cast<int> (j), static_cast<int> (i), 1, e, a.length + b.length, stub });
            }
        }
    std::sort (out.begin (), out.end (), [] (const Corner& a, const Corner& b) { return a.score > b.score; });
    return out;
}

namespace {
// --- the bend: an arm at an angle --------------------------------------------------------
// An arm leaving its parent's end at an angle (a V) shares a section with the parent's end: the
// corridor runs on through the bend instead of the arm taking a stair of its own (user,
// 2026-10-10). Each leg's bands end on the bend's mitre line; the wedges between them are left to
// the mould, which gives them to the flats beside them with their rooms left to the user. The
// parent's floor beyond the arm's outer facade produced is cut from the massing.
struct Leg {
    Frame f; // u toward the bend
    double depth = 0;
    double s0 = 0, sJ = 0; // the far end (a cap) and the bend
    Vec side;              // mitre normal: floor before the bend has Dot (X - P, side) < 0
};
// Last u from the far end where the band [v0, v1] stays on the floor and before the mitre.
double BandEnd (const Ctx& c, const Leg& g, Vec P, double v0, double v1)
{
    auto mitre = [&] (double v) {
        const Vec q = g.f.World (0, v);
        return Dot ({ P.x - q.x, P.y - q.y }, g.side) / Dot (g.f.u, g.side);
    };
    const double limit = (std::min) (mitre (v0), mitre (v1));
    const double step = (std::max) (0.1, (v1 - v0 - 0.1) / 6);
    double u = g.s0;
    while (u + 0.1 <= limit + 1e-9) {
        bool inside = true;
        for (double v = v0 + 0.05; v <= v1 - 0.05 + 1e-9 && inside; v += step)
            inside = Inside (c.outline, g.f.World (u + 0.1, v));
        if (!inside)
            break;
        u += 0.1;
    }
    return u;
}
void BendSection (Ctx& c, const Leg& A, const Leg& B, Vec P, int run)
{
    const double corr = c.o.corridor, w = c.o.coreWidth;
    const int id = AddSection (c, run, (A.sJ - A.s0) * A.depth + (B.sJ - B.s0) * B.depth, 'C');
    struct Laid {
        double c0, c1, e0, coreLo, coreHi, otherLo, otherHi, endCore, endOther;
        bool up;
    };
    auto prepare = [&] (const Leg& g, double cap = -1) {
        Laid l {};
        l.c0 = (g.depth - corr) / 2, l.c1 = l.c0 + corr;
        const Box all { g.s0, 0, g.sJ, g.depth };
        l.up = Dot (g.f.v, North (c.o)) >= 0;
        const double fu = SideFacade (c, g.f, all, 2), fd = SideFacade (c, g.f, all, 0), len = g.sJ - g.s0;
        if ((l.up ? fu : fd) < 0.5 * len && (l.up ? fd : fu) >= 0.5 * len)
            l.up = !l.up;
        l.coreLo = l.up ? l.c1 : 0, l.coreHi = l.up ? g.depth : l.c0;
        l.otherLo = l.up ? 0 : l.c1, l.otherHi = l.up ? l.c0 : g.depth;
        l.e0 = g.s0 +
               (cap > 0 ? cap : Clip ((std::max) (l.coreHi - l.coreLo, RoomFrontage (2).pref), c.o.minCap, c.o.maxCap));
        l.endCore = BandEnd (c, g, P, l.coreLo, l.coreHi);
        l.endOther = BandEnd (c, g, P, l.otherLo, l.otherHi);
        return l;
    };
    // Caps from the usual length outward: the first that leaves the band beside the corridor
    // whole flats (with the stair: on both sides of it).
    auto caps = [&] (const Laid& l, const Leg& g) {
        std::vector<double> out;
        const double cap0 = l.e0 - g.s0;
        for (double d = 0; d <= c.o.maxCap - c.o.minCap + 1e-9; d += 0.1)
            for (double cap : { cap0 - d, cap0 + d })
                if (cap >= c.o.minCap - 1e-9 && cap <= c.o.maxCap + 1e-9 && cap < g.sJ - g.s0 - 1.2)
                    out.push_back (cap);
        return out;
    };
    Laid la = prepare (A), lb = prepare (B);
    // The stair stands along the corridor on the longer leg, both dead ends as short as it can.
    const bool onA = A.sJ - la.e0 >= B.sJ - lb.e0;
    const Leg& g = onA ? A : B;
    Laid& l = onA ? la : lb;
    Laid& n = onA ? lb : la;
    const Leg& h = onA ? B : A;
    for (double cap : caps (n, h)) {
        const Laid m = prepare (h, cap);
        if (PieceFits (c, h.f, { m.e0, m.coreLo, m.endCore, m.coreHi }, m.up)) {
            n = m;
            break;
        }
    }
    const double beyond = h.sJ - n.e0;
    double uc = l.e0, bestCost = 1e18;
    bool fitted = false;
    const double cap0 = l.e0 - g.s0;
    for (double cap : caps (l, g)) {
        const Laid m = prepare (g, cap);
        for (double u = m.e0; u <= m.endCore - w + 1e-9; u += 0.1) {
            if (SideFacade (c, g.f, { u, m.coreLo, u + w, m.coreHi }, m.up ? 2 : 0) < c.o.stairWindow)
                continue;
            const double dead = (std::max) (u - m.e0, g.sJ - u - w + beyond);
            const bool fits = PieceFits (c, g.f, { m.e0, m.coreLo, u, m.coreHi }, m.up) &&
                              PieceFits (c, g.f, { u + w, m.coreLo, m.endCore, m.coreHi }, m.up);
            const double cost = dead + (fits ? 0 : 100) + (dead > c.o.maxDeadEnd ? 50 : 0) + std::abs (cap - cap0);
            if (cost < bestCost)
                bestCost = cost, uc = u, fitted = fits, l = m;
        }
        if (fitted)
            break;
    }
    if (!fitted)
        c.notes.push_back ({ Diagnostic::Warning, "core.no_fit",
                             "No stair position leaves whole flats on both sides; the band beside it will not divide.",
                             g.f.World (uc + w / 2, (l.coreLo + l.coreHi) / 2) });
    AddCore (c, g.f, { uc, l.coreLo, uc + w, l.coreHi }, id, false);
    // One corridor from cap to cap through the bend, mitred at the turn.
    const Vec a = A.f.World (la.e0, A.depth / 2), b = B.f.World (lb.e0, B.depth / 2);
    const auto shape = cp::InflatePaths ({ cp::PathD { { a.x, a.y }, { P.x, P.y }, { b.x, b.y } } }, corr / 2,
                                         cp::JoinType::Miter, cp::EndType::Butt, 4.0, kPrecision);
    if (!shape.empty ()) {
        Corridor corridor;
        corridor.shape = Counter (FromPath (cp::SimplifyPath (shape.front (), 1e-4)));
        corridor.axis = { a, P, b };
        corridor.section = id;
        c.out.corridors.push_back (std::move (corridor));
    }
    for (int k = 0; k < 2; ++k) {
        const Leg& leg = k == 0 ? A : B;
        const Laid& m = k == 0 ? la : lb;
        auto lit = [&] (Box x, bool high) {
            return SideFacade (c, leg.f, x, high ? 2 : 0) + SideFacade (c, leg.f, x, 3) + SideFacade (c, leg.f, x, 1);
        };
        const double capV0 = m.up ? m.c0 : 0, capV1 = m.up ? leg.depth : m.c1;
        const double oppV0 = m.up ? 0 : m.c0, oppV1 = m.up ? m.c1 : leg.depth;
        const Box sigma { leg.s0, capV0, m.e0, capV1 }, other { leg.s0, oppV0, m.e0, oppV1 };
        const bool flip = lit (sigma, m.up) < kBedroomMin && lit (other, !m.up) >= kBedroomMin;
        if (!flip && lit (sigma, m.up) < kBedroomMin)
            c.out.storage.push_back ({ ToRing (leg.f, sigma), "corridor end without facade (storage)" });
        else
            AddCap (c, leg.f, flip ? other : sigma, true, id, m.c0, m.c1);
        const double coreStart = flip ? leg.s0 : m.e0, otherStart = flip ? m.e0 : leg.s0;
        if ((k == 0) == onA) {
            AddSlot (c, leg.f, { coreStart, m.coreLo, uc, m.coreHi }, m.up, id, 0, 0, true, m.e0, leg.sJ);
            AddSlot (c, leg.f, { uc + w, m.coreLo, m.endCore, m.coreHi }, m.up, id, 0, 0, true, m.e0, leg.sJ);
        }
        else
            AddSlot (c, leg.f, { coreStart, m.coreLo, m.endCore, m.coreHi }, m.up, id, 0, 0, true, m.e0, leg.sJ);
        AddSlot (c, leg.f, { otherStart, m.otherLo, m.endOther, m.otherHi }, !m.up, id, 0, 0, true, m.e0, leg.sJ);
    }
}
} // namespace

bool Bend (Ctx& c, const Skeleton& sk, int ia, int ib, int& run)
{
    const auto& A = sk.wings[ia];
    const auto& B = sk.wings[ib];
    const bool flipped = sk.frames[ia].Local (B.a).x < A.length / 2;
    const Frame F = flipped ? FlipU (sk.frames[ia], A.length) : sk.frames[ia];
    const Frame& G = sk.frames[ib];
    // P: where the corridor centre lines meet.
    const Vec pa = F.World (0, A.depth / 2), pb = G.World (0, B.depth / 2);
    const double den = F.u.x * G.u.y - F.u.y * G.u.x;
    if (std::abs (den) < 0.3)
        return false;
    const double t = ((pb.x - pa.x) * G.u.y - (pb.y - pa.y) * G.u.x) / den;
    const Vec P { pa.x + t * F.u.x, pa.y + t * F.u.y };
    const double uB = G.Local (P).x;
    if (t < A.length - A.depth - B.depth || t > A.length + 1.0 || uB > 1.0 || uB < -(A.depth + B.depth))
        return false;
    // The parent's floor beyond the arm's outer facade produced: a small wedge, cut.
    {
        const bool high = Dot (G.v, F.u) > 0;
        const Vec n = high ? G.v : Vec { -G.v.x, -G.v.y }, q = G.World (0, high ? B.depth : 0);
        const double R = A.depth + B.depth + 10.0;
        auto at = [&] (double s, double r) {
            return cp::PointD (q.x + G.u.x * s + n.x * r, q.y + G.u.y * s + n.y * r);
        };
        const cp::PathD half { at (-R, 0), at (R, 0), at (R, R), at (-R, R) };
        for (const auto& piece : cp::Intersect (c.outline, { half }, cp::FillRule::NonZero, kPrecision)) {
            const double area = cp::Area (piece);
            const Ring ring = FromPath (piece);
            Vec mid {};
            for (const auto& p : ring)
                mid.x += p.x / ring.size (), mid.y += p.y / ring.size ();
            if (area > 0.5 && area < 0.3 * A.depth * A.depth && F.Local (mid).x > t - A.depth) {
                c.out.culled.push_back (
                    { Counter (ring), "parent's end beyond the angled arm: leave it out of the massing" });
                c.outline = cp::Difference (c.outline, { piece }, cp::FillRule::NonZero, kPrecision);
            }
        }
    }
    // The bend section as large as one stair's area allows, the rest of each wing on its own.
    const double tA = t, tB = B.length - uB;
    const double limit = (c.o.sectionArea + c.o.sectionSlack) / c.o.grossFactor;
    const double minPart = c.o.minCap + 1.0, minRest = c.o.minCap + c.o.coreWidth + 5.0;
    const Vec side { F.u.x + G.u.x, F.u.y + G.u.y };
    // Floor of each leg between a cut `x` before (A) or after (B) the bend and the mitre.
    const double R = 4 * (A.length + B.length);
    auto floor = [&] (const Frame& f, double from, double to, double sign) {
        auto at = [&] (double u, double v) {
            const Vec q = f.World (u, v);
            return cp::PointD (q.x, q.y);
        };
        const cp::PathD box { at (from, -R), at (to, -R), at (to, R), at (from, R) };
        const Vec d { -side.y, side.x };
        auto hp = [&] (double s, double r) {
            return cp::PointD (P.x + d.x * s - sign * side.x * r, P.y + d.y * s - sign * side.y * r);
        };
        const cp::PathD half { hp (-R, 0), hp (R, 0), hp (R, R), hp (-R, R) };
        return std::abs (
            detail::Area (cp::Intersect (cp::Intersect (c.outline, { box }, cp::FillRule::NonZero, kPrecision),
                                         { half }, cp::FillRule::NonZero, kPrecision)));
    };
    double best = 1e18, la = -1, lb = -1;
    auto lengths = [&] (double total) {
        std::vector<double> out;
        for (double x = minPart; x < total - 1e-9; x += 0.5)
            out.push_back (x);
        out.push_back (total);
        return out;
    };
    const auto xs = lengths (tA), ys = lengths (tB);
    std::vector<double> areaA, areaB;
    for (double x : xs)
        areaA.push_back (floor (F, tA - x, tA + R, 1.0));
    for (double y : ys)
        areaB.push_back (floor (G, -R, uB + y, -1.0));
    for (size_t i = 0; i < xs.size (); ++i)
        for (size_t j = 0; j < ys.size (); ++j) {
            const double x = xs[i], y = ys[j], rx = tA - x, ry = tB - y;
            if ((rx > 0.25 && rx < minRest) || (ry > 0.25 && ry < minRest) || areaA[i] + areaB[j] > limit)
                continue;
            const int stairs = 1 + (rx > 0.25 ? static_cast<int> (std::ceil (rx * A.depth / limit)) : 0) +
                               (ry > 0.25 ? static_cast<int> (std::ceil (ry * B.depth / limit)) : 0);
            const double score = stairs * 1e4 - (areaA[i] + areaB[j]);
            if (score < best)
                best = score, la = x, lb = y;
        }
    if (la < 0)
        return false;
    if (tA - la > 0.25)
        Straight (c, sk, flipped ? Portion { ia, A.length - (tA - la), A.length } : Portion { ia, 0, tA - la },
                  Access::Centre, run++);
    if (tB - lb > 0.25)
        Straight (c, sk, { ib, uB + lb, B.length }, Access::Centre, run++);
    const Leg legA { F, A.depth, tA - la, tA, side };
    const Leg legB { FlipU (G, uB + lb), B.depth, 0, lb, { -side.x, -side.y } };
    BendSection (c, legA, legB, P, run++);
    return true;
}
} // namespace geomsrv::archviz::floorscheme::detail::runs
