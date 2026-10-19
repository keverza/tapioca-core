#include "Edit/EditPathContours.hpp"
#include "Edit/EditPathNetwork.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>

namespace geomsrv::edit {
namespace {
struct Cut {
    uint64_t key;
    Vec3 p;
};
void Trace (const PavedArea& mesh, double level, Id path, std::vector<Contour>& out)
{
    std::vector<std::pair<Cut, Cut>> pieces;
    std::set<std::pair<uint64_t, uint64_t>> seen;
    const auto cut = [&] (uint32_t a, uint32_t b) {
        const Vec3 p = mesh.vertices[a], q = mesh.vertices[b];
        if (std::abs (p.z - level) < 1e-12)
            return Cut { a, { p.x, p.y, level } };
        if (std::abs (q.z - level) < 1e-12)
            return Cut { b, { q.x, q.y, level } };
        Vec3 at = Lerp (p, q, (level - p.z) / (q.z - p.z));
        at.z = level;
        return Cut { (uint64_t (1) << 63) | (uint64_t (std::min (a, b)) * mesh.vertices.size () + std::max (a, b)),
                     at };
    };
    for (size_t i = 0; i + 2 < mesh.triangles.size (); i += 3) {
        const uint32_t ids[] = { mesh.triangles[i], mesh.triangles[i + 1], mesh.triangles[i + 2] };
        std::vector<Cut> cuts;
        for (int k = 0; k < 3; ++k) {
            const auto a = ids[k], b = ids[(k + 1) % 3];
            if ((mesh.vertices[a].z >= level) != (mesh.vertices[b].z >= level))
                cuts.push_back (cut (a, b));
        }
        if (cuts.size () != 2 || DistanceXY (cuts[0].p, cuts[1].p) < 1e-10)
            continue;
        const auto key = std::minmax (cuts[0].key, cuts[1].key);
        if (seen.emplace (key.first, key.second).second)
            pieces.push_back ({ cuts[0], cuts[1] });
    }
    std::map<uint64_t, std::vector<size_t>> adjacency;
    for (size_t i = 0; i < pieces.size (); ++i) {
        adjacency[pieces[i].first.key].push_back (i);
        adjacency[pieces[i].second.key].push_back (i);
    }
    std::vector<bool> used (pieces.size (), false);
    const auto next = [&] (Cut& end) {
        for (const auto i : adjacency[end.key])
            if (!used[i]) {
                used[i] = true;
                end = pieces[i].first.key == end.key ? pieces[i].second : pieces[i].first;
                return true;
            }
        return false;
    };
    for (size_t i = 0; i < pieces.size (); ++i) {
        if (used[i])
            continue;
        used[i] = true;
        Cut first = pieces[i].first, last = pieces[i].second;
        std::deque<Vec3> points { first.p, last.p };
        while (next (last))
            points.push_back (last.p);
        while (next (first))
            points.push_front (first.p);
        Contour contour;
        contour.path = path;
        contour.z = level;
        contour.points.assign (points.begin (), points.end ());
        out.push_back (std::move (contour));
    }
}
} // namespace

std::vector<Contour> PathContours (const std::vector<Path>& paths, double interval, double step, size_t maxLevels)
{
    std::vector<Contour> out;
    if (!(interval > 0) || !std::isfinite (interval) || !(step > 0) || !std::isfinite (step))
        return out;
    const auto mesh = Pave (paths, step);
    return SurfaceContours (mesh, interval, paths.size () == 1 ? paths[0].id : kNoId, maxLevels);
}

std::vector<Contour> SurfaceContours (const PavedArea& mesh, double interval, Id path, size_t maxLevels)
{
    std::vector<Contour> out;
    if (!(interval > 0) || !std::isfinite (interval))
        return out;
    if (mesh.triangles.empty ())
        return out;
    double lo = std::numeric_limits<double>::infinity (), hi = -lo;
    for (const auto& p : mesh.vertices) {
        lo = std::min (lo, p.z);
        hi = std::max (hi, p.z);
    }
    const auto levels = Levels (lo, hi, interval);
    if (levels.size () > maxLevels)
        return out;
    // A connected network's contour has no unique source path. Its geometry is
    // cut from exactly the same triangles as the displayed united surface.
    for (double level : levels)
        Trace (mesh, level, path, out);
    return out;
}

bool PathHeightRange (const std::vector<Path>& paths, double& lo, double& hi, double step)
{
    lo = std::numeric_limits<double>::infinity ();
    hi = -lo;
    for (const auto& path : paths) {
        PathOutline outline;
        std::string error;
        if (!Outline (path, step, outline, error))
            continue;
        for (const auto* line : { &outline.centre, &outline.left, &outline.right })
            for (const auto& p : *line) {
                lo = std::min (lo, p.z);
                hi = std::max (hi, p.z);
            }
    }
    return std::isfinite (lo) && std::isfinite (hi);
}

double NiceStep (double range, int divisions)
{
    const double raw = range / std::max (1, divisions);
    if (!(raw > 0) || !std::isfinite (raw))
        return 0;
    const double magnitude = std::pow (10.0, std::floor (std::log10 (raw))), f = raw / magnitude;
    return (f < 1.5 ? 1.0 : f < 2.25 ? 2.0 : f < 3.5 ? 2.5 : f < 7.5 ? 5.0 : 10.0) * magnitude;
}
std::vector<double> Levels (double lo, double hi, double step)
{
    std::vector<double> out;
    if (!(step > 0) || !(hi >= lo) || !std::isfinite (step) || !std::isfinite (lo) || !std::isfinite (hi))
        return out;
    const double first = std::ceil (lo / step - 1e-9), last = std::floor (hi / step + 1e-9);
    for (double m = first; m <= last && out.size () < 100000; m += 1)
        out.push_back (m * step);
    return out;
}
int StepDecimals (double step)
{
    for (int d = 0; d < 4; ++d) {
        const double v = step * std::pow (10.0, d);
        if (std::abs (v - std::round (v)) < 1e-6)
            return d;
    }
    return 4;
}
} // namespace geomsrv::edit
