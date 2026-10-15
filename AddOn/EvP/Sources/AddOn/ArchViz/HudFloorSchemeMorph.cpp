#include "ArchViz/HudFloorSchemeEditDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include <clipper2/clipper.h>

// A floor's flats flowing from where they were to where they are when the floor is planned again
// (user, 2026-10-14: "Some animation would be when a drag dropping unit like how they shift and
// reflow like amebae, bubbles or voronoi"). MAIN THREAD, under ImGui's lock.
namespace geomsrv::archviz::hudfloorscheme {
namespace detail {
namespace {
namespace cp = Clipper2Lib;
constexpr size_t kPoints = 48; // along every outline, so any two can flow into each other

double SignedArea (const fs::Ring& r)
{
    double a = 0;
    for (size_t i = 0; i < r.size (); ++i) {
        const auto& p = r[i];
        const auto& q = r[(i + 1) % r.size ()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}
// `r` counter-clockwise, `n` points at equal steps along its perimeter from its first corner.
std::vector<fs::Vec> Resample (fs::Ring r, size_t n)
{
    std::vector<fs::Vec> out;
    if (r.size () < 3)
        return out;
    if (SignedArea (r) < 0)
        std::reverse (r.begin (), r.end ());
    double perimeter = 0;
    for (size_t i = 0; i < r.size (); ++i)
        perimeter += std::hypot (r[(i + 1) % r.size ()].x - r[i].x, r[(i + 1) % r.size ()].y - r[i].y);
    if (perimeter <= 1e-9)
        return out;
    size_t edge = 0;
    double walked = 0; // perimeter before `edge`
    for (size_t k = 0; k < n; ++k) {
        const double at = perimeter * double (k) / double (n);
        while (edge + 1 < r.size ()) {
            const auto& a = r[edge];
            const auto& b = r[(edge + 1) % r.size ()];
            const double length = std::hypot (b.x - a.x, b.y - a.y);
            if (walked + length >= at)
                break;
            walked += length, ++edge;
        }
        const auto& a = r[edge];
        const auto& b = r[(edge + 1) % r.size ()];
        const double length = std::hypot (b.x - a.x, b.y - a.y);
        const double t = length > 1e-12 ? std::clamp ((at - walked) / length, 0.0, 1.0) : 0.0;
        out.push_back ({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
    }
    return out;
}
// `to` turned to start where it follows `from` most closely: the outline flows without twisting.
void Align (const std::vector<fs::Vec>& from, std::vector<fs::Vec>& to)
{
    const size_t n = from.size ();
    size_t best = 0;
    double least = 1e300;
    for (size_t shift = 0; shift < n; ++shift) {
        double sum = 0;
        for (size_t i = 0; i < n && sum < least; ++i) {
            const auto& p = from[i];
            const auto& q = to[(i + shift) % n];
            sum += (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
        }
        if (sum < least)
            least = sum, best = shift;
    }
    std::rotate (to.begin (), to.begin () + static_cast<std::ptrdiff_t> (best), to.end ());
}
double Overlap (const fs::Ring& a, const fs::Ring& b)
{
    const auto path = [] (const fs::Ring& r) {
        cp::PathD p;
        for (const auto& q : r)
            p.emplace_back (q.x, q.y);
        return p;
    };
    double area = 0;
    for (const auto& p : cp::Intersect ({ path (a) }, { path (b) }, cp::FillRule::NonZero, 4))
        area += std::abs (cp::Area (p));
    return area;
}
ImU32 FlatColour (const fs::Flat& f, const floorprogramme::Programme& programme)
{
    ImU32 colour = hudshell::Packed (floorprogramme::Colour (programme, f.type, f.rooms));
    if (f.manual)
        colour = (colour & 0x00FFFFFF) | 0x99000000;
    return colour;
}
} // namespace

std::vector<Morph> Morphs (const std::vector<std::pair<fs::Ring, ImU32>>& before, const fs::Scheme& after,
                           const floorprogramme::Programme& programme)
{
    std::vector<Morph> out;
    std::vector<bool> taken (before.size (), false);
    for (const auto& f : after.flats) {
        Morph m;
        m.colour = FlatColour (f, programme);
        m.to = Resample (f.shape, kPoints);
        if (m.to.empty ())
            continue;
        size_t best = before.size ();
        double most = 0.5; // m2: a touch is not where it came from
        for (size_t i = 0; i < before.size (); ++i)
            if (const double o = Overlap (before[i].first, f.shape); o > most)
                most = o, best = i;
        if (best < before.size ()) {
            m.from = Resample (before[best].first, kPoints);
            taken[best] = true;
        }
        if (m.from.size () != m.to.size ()) // nothing there before: it grows from its middle
            m.from.assign (kPoints, Mid (f.shape));
        Align (m.from, m.to);
        out.push_back (std::move (m));
    }
    for (size_t i = 0; i < before.size (); ++i) // what no flat took shrinks away
        if (!taken[i]) {
            Morph m;
            m.colour = before[i].second;
            m.from = Resample (before[i].first, kPoints);
            if (m.from.empty ())
                continue;
            m.to.assign (kPoints, Mid (before[i].first));
            out.push_back (std::move (m));
        }
    return out;
}
std::vector<fs::Vec> Between (const Morph& m, double t)
{
    const double e = 1.0 - std::pow (1.0 - std::clamp (t, 0.0, 1.0), 3.0); // eased out: it settles
    std::vector<fs::Vec> out (m.from.size ());
    for (size_t i = 0; i < out.size () && i < m.to.size (); ++i)
        out[i] = { m.from[i].x + (m.to[i].x - m.from[i].x) * e, m.from[i].y + (m.to[i].y - m.from[i].y) * e };
    return out;
}
bool Flowing (const Editor& e, double now)
{
    return !e.morphs.empty () && e.morphStart >= 0 && now - e.morphStart < kMorphSeconds;
}
std::vector<std::pair<fs::Ring, ImU32>> Showing (const Editor& e, const floorprogramme::Programme& programme,
                                                 double now)
{
    std::vector<std::pair<fs::Ring, ImU32>> out;
    if (Flowing (e, now)) {
        const double t = (now - e.morphStart) / kMorphSeconds;
        for (const auto& m : e.morphs)
            out.push_back ({ Between (m, t), m.colour });
        return out;
    }
    for (const auto& f : e.scheme.flats)
        out.push_back ({ f.shape, FlatColour (f, programme) });
    return out;
}
} // namespace detail

bool Animating (const std::map<std::string, EditorPtr>& editors)
{
    if (!ImGui::GetCurrentContext ())
        return false;
    const double now = ImGui::GetTime ();
    return std::any_of (editors.begin (), editors.end (),
                        [&] (const auto& entry) { return entry.second && detail::Flowing (*entry.second, now); });
}
} // namespace geomsrv::archviz::hudfloorscheme
