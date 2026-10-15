#include "Edit/EditPathNetwork.hpp"
#include <clipper2/clipper.h>
#include <clipper2/clipper.triangulation.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace geomsrv::edit {

namespace {

namespace cp = Clipper2Lib;

constexpr int kPrecision = 3;          // millimetres
constexpr double kArcTolerance = 0.01; // metres a rounding may stray from the true arc

cp::PathD Polygon (const PathOutline& o)
{
    cp::PathD ring;
    for (const auto& p : o.left)
        ring.push_back ({ p.x, p.y });
    for (size_t i = o.right.size (); i-- > 0;)
        ring.push_back ({ o.right[i].x, o.right[i].y });
    return ring;
}

cp::PathsD Inflate (const cp::PathsD& paths, double delta)
{
    return cp::InflatePaths (paths, delta, cp::JoinType::Round, cp::EndType::Polygon, 2.0, kPrecision, kArcTolerance);
}

// Rounds the outside corners of `area` by `r` (erode, then grow back).
cp::PathsD Opening (const cp::PathsD& area, double r)
{
    return r > 0.0 ? Inflate (Inflate (area, -r), r) : area;
}

// Rounds the re-entrant corners of `area` by `r` (grow, then erode back).
cp::PathsD Closing (const cp::PathsD& area, double r)
{
    return r > 0.0 ? Inflate (Inflate (area, r), -r) : area;
}

struct Source {
    const Path* path = nullptr;
    AlignmentGeometry geometry;
};

// The height at a plan point: the path it lies in (or nearest, at a junction's rounding).
double HeightAt (const std::vector<Source>& sources, Vec3 p)
{
    double best = std::numeric_limits<double>::infinity (), z = 0.0;
    for (const auto& s : sources) {
        double h = 0.0, beyond = 0.0;
        if (SurfaceHeight (*s.path, s.geometry, p, h, beyond) && beyond < best)
            best = beyond, z = h;
    }
    return z;
}

} // namespace

bool SurfaceHeight (const Path& path, const AlignmentGeometry& g, Vec3 p, double& z, double& beyond)
{
    double station = 0.0, offset = 0.0;
    StationFrame f;
    if (!StationOf (g, p, station, offset) || !Evaluate (path, g, station, f))
        return false;
    const bool left = offset >= 0.0;
    const double width = left ? f.section.widthLeft : f.section.widthRight;
    double dropLeft = 0.0, dropRight = 0.0;
    EdgeDrops (f.section, dropLeft, dropRight);
    const double share = width > 1.0e-9 ? std::min (std::abs (offset) / width, 1.0) : 0.0;
    z = f.centre.z + (left ? dropLeft : dropRight) * share;
    beyond = std::abs (offset) - width;
    return true;
}

PavedArea Pave (const std::vector<Path>& paths, double step)
{
    PavedArea out;
    std::vector<Source> sources;
    cp::PathsD united;
    double junction = 0.0;
    for (const auto& path : paths) {
        Source s { &path, {} };
        PathOutline o;
        std::string error;
        if (!ValidatePath (path, s.geometry, error) || !Outline (path, step, o, error) || o.left.size () < 2)
            continue;
        sources.push_back (std::move (s));
        // Its own outline, clean (a tight bend's fold resolved), its outside corners rounded --
        // never by more than most of its narrowest half, or a narrow path would vanish.
        cp::PathsD own = cp::Union (cp::PathsD { Polygon (o) }, cp::FillRule::NonZero, kPrecision);
        double half = std::numeric_limits<double>::infinity ();
        for (const auto& r : path.sections)
            half = std::min ({ half, r.widthLeft, r.widthRight });
        const double r = std::min (path.edgeRadius, 0.9 * (std::isfinite (half) ? half : 0.0));
        own = Opening (own, r);
        united.insert (united.end (), own.begin (), own.end ());
        junction = std::max (junction, path.edgeRadius);
    }
    if (united.empty ())
        return out;
    // One area: crossings and touching ends made junctions, their re-entrant corners rounded.
    united = cp::Union (united, cp::FillRule::NonZero, kPrecision);
    united = Closing (united, junction);
    // Straight runs keep their ends only: the samples along them say nothing (1 mm).
    united = cp::SimplifyPaths (united, 0.001, true);
    const auto lift = [&] (const cp::PointD& q) {
        const Vec3 p { q.x, q.y, 0.0 };
        return Vec3 { q.x, q.y, HeightAt (sources, p) };
    };
    for (const auto& ring : united) {
        if (ring.size () < 3)
            continue;
        std::vector<Vec3> r;
        for (const auto& q : ring)
            r.push_back (lift (q));
        out.rings.push_back (std::move (r));
    }
    // The surface: each outline with its holes triangulated.
    for (const auto& outer : united) {
        if (cp::Area (outer) <= 0.0)
            continue;
        cp::PathsD region { outer };
        for (const auto& hole : united)
            if (cp::Area (hole) < 0.0 && !hole.empty () &&
                cp::PointInPolygon (hole.front (), outer) != cp::PointInPolygonResult::IsOutside)
                region.push_back (hole);
        cp::PathsD triangles;
        if (cp::Triangulate (region, kPrecision, triangles, false) != cp::TriangulateResult::success)
            continue; // a refused region stays outlined, unfilled
        for (auto& t : triangles) {
            if (t.size () != 3)
                continue;
            if (cp::Area (t) < 0.0)
                std::reverse (t.begin (), t.end ());
            for (const auto& q : t) {
                out.triangles.push_back (static_cast<uint32_t> (out.vertices.size ()));
                out.vertices.push_back (lift (q));
            }
        }
    }
    return out;
}

} // namespace geomsrv::edit
