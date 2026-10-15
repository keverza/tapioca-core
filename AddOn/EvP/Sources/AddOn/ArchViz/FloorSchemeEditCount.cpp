#include "ArchViz/FloorSchemeEditDetail.hpp"
#include <algorithm>

// The floor's flat count (FloorSchemeEdit.hpp `SetCount`): rows laid again with one more, joined
// with fewer, between the programme's smallest and largest flats.
namespace geomsrv::archviz::floorscheme::edit {
namespace {
using namespace detail;
using namespace shared;
// The widths of a run of flats laid again with one more: the widest split in two, then every flat
// narrower than it may be widened to that, the others giving in proportion to what they have over
// theirs. A flat is at least `least`; the run's first and last at least `first` and `last` (to
// reach the corridor by a door). Empty when one more does not fit.
std::vector<double> OneMore (std::vector<double> w, double least, double first, double last)
{
    double length = 0;
    for (double x : w)
        length += x;
    const size_t n = w.size () + 1;
    const auto lowest = [&] (size_t k) {
        return (std::max) ({ least, k == 0 ? first : 0.0, k + 1 == n ? last : 0.0 });
    };
    double need = 0;
    for (size_t k = 0; k < n; ++k)
        need += lowest (k);
    if (w.empty () || need > length + 1e-9)
        return {};
    const auto widest = std::max_element (w.begin (), w.end ());
    const double half = *widest / 2;
    *widest = half;
    w.insert (widest + 1, half);
    double deficit = 0, spare = 0;
    for (size_t k = 0; k < n; ++k)
        (w[k] < lowest (k) ? deficit : spare) += std::abs (w[k] - lowest (k));
    if (deficit > 1e-9)
        for (size_t k = 0; k < n; ++k)
            w[k] = w[k] < lowest (k) ? lowest (k) : w[k] - (w[k] - lowest (k)) * deficit / spare;
    return w;
}
} // namespace

double Narrowest (const floorprogramme::Programme& programme)
{
    double fewest = 1e9;
    for (const auto& type : programme.types)
        fewest = (std::min) (fewest, type.rooms);
    return RoomFrontage (fewest < 1e8 ? fewest : 1.5).min;
}
Design SetCount (const Scheme& s, Design d, int total, std::string* why, double narrowest, double most)
{
    const double least = (narrowest > 0 ? narrowest : kNarrowest) + 0.2;
    const double cap = most > 0 ? most + 0.5 : 1e18;
    std::vector<Frozen> bands;
    for (size_t b = 0; b < s.bands.size (); ++b)
        if (auto f = Freeze (s, static_cast<int> (b)))
            bands.push_back (std::move (*f));
    std::vector<char> touched (bands.size (), 0);
    const auto free = [&] (const Frozen& f, size_t k) { return f.flats[k] < 0 || !Locked (s, d, f.flats[k]); };
    // A run of unlocked flats in a row, and how wide its ends must be to reach the corridor by a door.
    struct Run {
        size_t band, a, z;
        double first, last;
    };
    const auto runs = [&] () {
        std::vector<Run> out;
        for (size_t b = 0; b < bands.size (); ++b) {
            const auto& f = bands[b];
            const auto [dlo, dhi] = Reach (s, f);
            const bool known = dlo <= dhi;
            for (size_t a = 0; a < f.lo.size ();) {
                if (!free (f, a)) {
                    ++a;
                    continue;
                }
                size_t z = a;
                while (z + 1 < f.lo.size () && free (f, z + 1))
                    ++z;
                out.push_back ({ b, a, z, known ? dlo + kDoor - f.lo[a] : 0, known ? f.hi[z] - (dhi - kDoor) : 0 });
                a = z + 1;
            }
        }
        return out;
    };
    // Run `r` laid again as `widths`: a flat whose walls stay keeps its rooms, the rest take what
    // their width fits.
    const auto relay = [&] (const Run& r, const std::vector<double>& widths) {
        auto& f = bands[r.band];
        const size_t m = r.z - r.a + 1;
        std::vector<double> lo, hi, rooms;
        std::vector<int> flats;
        double u = f.lo[r.a];
        for (const double width : widths) {
            size_t was = r.a + m;
            for (size_t k = r.a; k <= r.z; ++k)
                if (std::abs (f.lo[k] - u) < 0.05 && std::abs (f.hi[k] - (u + width)) < 0.05)
                    was = k;
            const bool same = was <= r.z;
            lo.push_back (u), hi.push_back (u + width);
            rooms.push_back (same ? f.rooms[was] : 0);
            flats.push_back (same ? f.flats[was] : -1);
            u += width;
        }
        const auto at = static_cast<std::ptrdiff_t> (r.a), to = static_cast<std::ptrdiff_t> (r.z + 1);
        f.lo.erase (f.lo.begin () + at, f.lo.begin () + to), f.lo.insert (f.lo.begin () + at, lo.begin (), lo.end ());
        f.hi.erase (f.hi.begin () + at, f.hi.begin () + to), f.hi.insert (f.hi.begin () + at, hi.begin (), hi.end ());
        f.rooms.erase (f.rooms.begin () + at, f.rooms.begin () + to);
        f.rooms.insert (f.rooms.begin () + at, rooms.begin (), rooms.end ());
        f.flats.erase (f.flats.begin () + at, f.flats.begin () + to);
        f.flats.insert (f.flats.begin () + at, flats.begin (), flats.end ());
        touched[r.band] = 1;
    };
    int now = static_cast<int> (s.flats.size ());
    while (now < total && !d.pins.joins.empty ()) {
        // The latest join undone first: each gives back the flat it took.
        d.pins.joins.pop_back ();
        ++now;
    }
    bool full = false;
    while (now < total) {
        // The run that stays widest laid again with one more.
        const auto all = runs ();
        size_t pick = all.size ();
        std::vector<double> best;
        double widest = 0;
        for (size_t i = 0; i < all.size (); ++i) {
            const auto& f = bands[all[i].band];
            std::vector<double> w;
            for (size_t k = all[i].a; k <= all[i].z; ++k)
                w.push_back (f.hi[k] - f.lo[k]);
            w = OneMore (std::move (w), least, all[i].first, all[i].last);
            const double narrow = w.empty () ? 0 : *std::min_element (w.begin (), w.end ());
            if (narrow > widest)
                widest = narrow, pick = i, best = std::move (w);
        }
        if (pick == all.size ()) {
            Refuse ({}, why,
                    "%d flats is as many as the floor takes now: no row has room for one more flat of %.1f m with a "
                    "door on the corridor.",
                    now, least);
            full = true;
            break;
        }
        relay (all[pick], best);
        ++now;
    }
    // At the most, each row it filled is laid evenly: between its ends, every flat the smallest.
    if (full)
        for (const auto& r : runs ()) {
            if (!touched[r.band])
                continue;
            const auto& f = bands[r.band];
            const size_t n = r.z - r.a + 1;
            std::vector<double> even (n, least);
            even.front () = (std::max) (even.front (), r.first);
            even.back () = (std::max) (even.back (), r.last);
            double slack = f.hi[r.z] - f.lo[r.a];
            for (const double x : even)
                slack -= x;
            if (slack < 0)
                continue;
            for (auto& x : even)
                x += slack / static_cast<double> (n);
            relay (r, even);
        }
    std::vector<Pins::Join> joins;
    // Which flats this call has joined into one: a join within one group would join a flat to itself.
    std::vector<int> group (s.flats.size ());
    for (size_t i = 0; i < group.size (); ++i)
        group[i] = static_cast<int> (i);
    const auto root = [&] (int i) {
        while (group[i] != i)
            i = group[i];
        return i;
    };
    while (now > total) {
        // The narrowest free flat with a free neighbour joins the narrower of them, if the two are
        // no larger than the largest flat.
        size_t bi = bands.size (), ki = 0, ji = 0;
        double narrow = 1e18;
        for (size_t b = 0; b < bands.size (); ++b) {
            const auto& f = bands[b];
            for (size_t k = 0; k < f.lo.size (); ++k) {
                if (!free (f, k))
                    continue;
                const bool before = k > 0 && free (f, k - 1), after = k + 1 < f.lo.size () && free (f, k + 1);
                if (!before && !after)
                    continue;
                const size_t j =
                    !after || (before && f.hi[k - 1] - f.lo[k - 1] <= f.hi[k + 1] - f.lo[k + 1]) ? k - 1 : k + 1;
                const double joined = (std::max) (f.hi[k], f.hi[j]) - (std::min) (f.lo[k], f.lo[j]);
                if (NetArea (joined, s.bands[f.band].depth) <= cap && f.hi[k] - f.lo[k] < narrow)
                    narrow = f.hi[k] - f.lo[k], bi = b, ki = k, ji = j;
            }
        }
        if (bi < bands.size ()) {
            auto& f = bands[bi];
            const size_t keep = (std::min) (ki, ji);
            const auto i = static_cast<std::ptrdiff_t> (keep);
            f.hi[keep] = f.hi[keep + 1];
            f.rooms[keep] = 0;
            f.lo.erase (f.lo.begin () + i + 1);
            f.hi.erase (f.hi.begin () + i + 1);
            f.rooms.erase (f.rooms.begin () + i + 1);
            f.flats.erase (f.flats.begin () + i + 1);
            touched[bi] = 1, --now;
            continue;
        }
        // No row left to close: the smallest flat (with what it has joined) joins a flat beside it.
        std::vector<double> net (s.flats.size (), 0);
        for (size_t i = 0; i < s.flats.size (); ++i)
            net[root (static_cast<int> (i))] += s.flats[i].net;
        int gone = -1, into = -1;
        for (size_t i = 0; i < s.flats.size (); ++i) {
            const int flat = static_cast<int> (i);
            if (Locked (s, d, flat) || (gone >= 0 && net[root (flat)] >= net[root (gone)]))
                continue;
            // A row flat a merge above took is part of its neighbour now.
            const auto row = std::find_if (bands.begin (), bands.end (),
                                           [&] (const Frozen& f) { return f.band == s.flats[i].band; });
            if (row != bands.end () && std::find (row->flats.begin (), row->flats.end (), flat) == row->flats.end ())
                continue;
            std::vector<int> mine;
            for (size_t j = 0; j < s.flats.size (); ++j)
                if (root (static_cast<int> (j)) == root (flat))
                    mine.push_back (static_cast<int> (j));
            if (const int other = Beside (s, d, flat, mine); other >= 0 && net[root (flat)] + net[root (other)] <= cap)
                gone = flat, into = other;
        }
        if (gone < 0) {
            if (most > 0)
                Refuse ({}, why,
                        "%d flats is as few as the floor takes now: two more joined would be over the largest flat "
                        "(%.0f m2).",
                        now, most);
            else
                Refuse ({}, why, "%d flats is as few as the floor takes now: no unlocked flats are left to join.", now);
            break;
        }
        joins.push_back ({ Within (s.flats[gone].shape), Within (s.flats[into].shape) });
        group[root (gone)] = root (into), --now;
    }
    for (size_t b = 0; b < bands.size (); ++b)
        if (touched[b])
            d = Write (s, std::move (d), bands[b]);
    for (const auto& j : joins) {
        const int gone = FlatAt (s, j.from);
        if (gone >= 0)
            std::erase_if (d.pins.rooms, [&] (const Pins::Rooms& r) { return In (s.flats[gone].shape, r.at); });
        d.pins.joins.push_back (j);
    }
    return d;
}
} // namespace geomsrv::archviz::floorscheme::edit
