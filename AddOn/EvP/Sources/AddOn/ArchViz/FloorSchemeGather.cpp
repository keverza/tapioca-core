#include "ArchViz/FloorSchemeDetail.hpp"
#include <algorithm>

// T7's last word (user, 2026-11-15): "tiny red strip should always be added to unit next to it";
// a corridor run on past a stair "creates L shaped units"; a stair moved anywhere along its
// corridor resizes the flats beside it. After every other stage:
//
//   - floor left to nobody that no flat could ever be -- a strip, or less than the smallest
//     flat of the programme -- joins the flat it shares the most wall with;
//   - a flat smaller than the smallest flat of the programme (the end of a corridor run past
//     its stair, the rest of a band beside a moved stair) joins the flat beside it.
//
// The flat that grows takes the type its area fits and has its rooms laid again; an L is an L.
// What touches no flat, or would leave a hole in one, stays as it was and stays visible.
namespace geomsrv::archviz::floorscheme::detail {
namespace {
constexpr double kStrip = 1.5;    // m: thinner than this is no room, whatever its area
constexpr double kShared = 0.5;   // m of wall two pieces must share to become one
constexpr double kOverCap = 1.25; // a joined flat may pass the largest flat by this much, not more

double Thickness (const Ring& ring)
{
    double perimeter = 0;
    for (size_t k = 0; k < ring.size (); ++k)
        perimeter +=
            std::hypot (ring[(k + 1) % ring.size ()].x - ring[k].x, ring[(k + 1) % ring.size ()].y - ring[k].y);
    return perimeter > 0 ? 2 * std::abs (Area (ring)) / perimeter : 0;
}
} // namespace

void Gather (Scheme& s, const cp::PathsD& outline, const cp::PathsD& blind, const floorprogramme::Programme& programme,
             const Pins& pins, const Options& o)
{
    if (s.flats.empty () || programme.types.empty ())
        return;
    double smallest = 1e18;
    for (const auto& type : programme.types)
        smallest = (std::min) (smallest, type.minM2);
    const double most = MaxFlat (programme, o) * kOverCap;
    // Nothing is gathered that breaks a hard rule the floor kept: each step is made on a copy and
    // kept only when the copy has no more errors than the floor had (a flat joined across two
    // stairs' circulation, a piece made a flat with no door, are left as they were).
    const auto errors = [&] (const Scheme& of) {
        Scheme copy = of;
        copy.diagnostics.clear ();
        return Check (copy, o);
    };
    size_t broken = errors (s);
    const auto keep = [&] (Scheme& trial) {
        const size_t now = errors (trial);
        if (now > broken)
            return false;
        broken = now;
        s = std::move (trial);
        return true;
    };
    // `gone`'s floor joined to flat `into` of `t`.
    const auto grow = [&] (Scheme& t, size_t into, const Ring& gone, double gross, double net) {
        Ring shape = t.flats[into].shape;
        if (t.flats[into].net + net > most || !JoinRing (shape, ToPath (Counter (gone))))
            return false;
        t.flats[into].shape = std::move (shape);
        t.flats[into].gross += gross, t.flats[into].net += net;
        RefitFlat (t, t.flats[into], outline, blind, programme, pins, o);
        return true;
    };
    // The flats beside `shape`, the one sharing the most wall first.
    const auto beside = [&] (const Ring& shape, int skip) {
        std::vector<std::pair<double, size_t>> out;
        for (size_t i = 0; i < s.flats.size (); ++i)
            if (static_cast<int> (i) != skip && Near (shape, s.flats[i].shape, 0.2))
                if (const double shared = Touch (shape, s.flats[i].shape); shared > kShared)
                    out.push_back ({ shared, i });
        std::sort (out.rbegin (), out.rend ());
        return out;
    };
    // Floor left to nobody. What could be no flat -- a strip, or less than the smallest flat --
    // joins the flat it shares the most wall with that can take it; what could be one, lit and
    // inside a wing, becomes one (the two ends of corridors a moved stair pushed together).
    for (size_t k = 0; k < s.unassigned.size ();) {
        const Piece piece = s.unassigned[k];
        const double area = std::abs (Area (piece.shape));
        const bool strip = Thickness (piece.shape) <= kStrip;
        const bool free = piece.reason.find ("storage") != std::string::npos ||
                          (!strip && piece.reason.find ("outside the skeleton") != std::string::npos);
        bool taken = false;
        if (!free && (strip || area < smallest)) {
            for (const auto& [shared, i] : beside (piece.shape, -1)) {
                Scheme trial = s;
                trial.unassigned.erase (trial.unassigned.begin () + k);
                if (grow (trial, i, piece.shape, area, area) && keep (trial)) {
                    taken = true;
                    break;
                }
            }
        }
        else if (!free && area <= most && FacadeOf (outline, piece.shape, &blind) >= kLivingMin) {
            Scheme trial = s;
            trial.unassigned.erase (trial.unassigned.begin () + k);
            Flat flat;
            flat.shape = Counter (piece.shape);
            flat.gross = flat.net = area;
            flat.cap = true;
            flat.type = programme.types.size (); // none yet: Refit gives it the one it fits
            RefitFlat (trial, flat, outline, blind, programme, pins, o);
            if (flat.type < trial.counts.size ())
                ++trial.counts[flat.type];
            trial.flats.push_back (std::move (flat));
            Note (trial, Diagnostic::Info, "flat.gathered", "Floor left between the flats became a flat.",
                  Centroid (piece.shape));
            taken = keep (trial);
        }
        if (!taken)
            ++k;
    }
    // Flats under the smallest: each joins the flat beside it, the smallest first; one that
    // cannot is left and not asked again.
    std::vector<Vec> left;
    for (;;) {
        int small = -1;
        for (size_t i = 0; i < s.flats.size (); ++i) {
            const Vec at = Centroid (s.flats[i].shape);
            const bool asked = std::any_of (left.begin (), left.end (),
                                            [&] (Vec q) { return std::hypot (q.x - at.x, q.y - at.y) < 1e-6; });
            if (!asked && s.flats[i].net < smallest - 0.5 && (small < 0 || s.flats[i].net < s.flats[small].net))
                small = static_cast<int> (i);
        }
        if (small < 0 || s.flats.size () < 2)
            break;
        const Flat gone = s.flats[small];
        bool joined = false;
        for (const auto& [shared, into] : beside (gone.shape, small)) {
            Scheme trial = s;
            if (!grow (trial, into, gone.shape, gone.gross, gone.net))
                continue;
            Flat& kept = trial.flats[into];
            kept.cap = kept.cap || gone.cap, kept.corner = kept.corner || gone.corner;
            kept.through = kept.through || gone.through;
            if (gone.band >= 0 && gone.band < static_cast<int> (trial.bands.size ()))
                --trial.bands[gone.band].flats;
            if (gone.type < trial.counts.size ())
                --trial.counts[gone.type];
            Note (trial, Diagnostic::Info, "flat.gathered",
                  "A flat under the smallest of the programme joined the flat beside it.", Centroid (gone.shape));
            trial.flats.erase (trial.flats.begin () + small);
            if (keep (trial)) {
                joined = true;
                break;
            }
        }
        if (!joined)
            left.push_back (Centroid (gone.shape));
    }
}
} // namespace geomsrv::archviz::floorscheme::detail
