#include "SunStudy/SunStudySolarCutCells.hpp"

#include "Geometry/SurfaceCoverage.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace evp::sunstudy {
namespace {
namespace cp = Clipper2Lib;
namespace sc = geomsrv::surfacecoverage;
using V = sc::V;

V At (const double p[3])
{
    return { p[0], p[1], p[2] };
}

cp::PathD Oriented (cp::PathD path)
{
    if (cp::Area (path) < 0.0)
        std::reverse (path.begin (), path.end ());
    return path;
}

// Hours are whole study steps: a millihour key keeps equal values together.
long long KeyOf (float hours)
{
    return std::llround (double (hours) * 1000.0);
}

bool Fill (const cp::PathsD& paths, const sc::Frame& frame, uint32_t mesh, std::vector<SolarCutTriangle>& out,
           std::string& error)
{
    if (paths.empty ())
        return true;
    cp::PathsD triangles;
    if (!sc::TriangulateOnGrid (paths, std::ldexp (1.0, -24), triangles, error))
        return false;
    for (const auto& path : triangles) {
        SolarCutTriangle triangle;
        triangle.mesh = mesh;
        for (size_t c = 0; c < 3; ++c) {
            const V p = frame.Lift (path[c]);
            triangle.v[c][0] = p.x;
            triangle.v[c][1] = p.y;
            triangle.v[c][2] = p.z;
        }
        out.push_back (triangle);
    }
    return true;
}

} // namespace

bool SolarCutCellsResult::Read (uint32_t sample, float& beforeHours, float& afterHours) const
{
    const auto found = std::lower_bound (samples.begin (), samples.end (), sample);
    if (found == samples.end () || *found != sample)
        return false;
    const size_t i = size_t (found - samples.begin ());
    beforeHours = before[i];
    afterHours = after[i];
    return true;
}

bool BuildSolarCutCells (const std::vector<SolarCutCell>& cells,
                         const std::map<uint64_t, std::vector<SolarCutTriangle>>& surfaces, SolarCutCellsResult& out,
                         const std::function<bool ()>& cancelled)
{
    out = {};
    // The hover's table, whatever can be drawn.
    std::vector<size_t> order (cells.size ());
    std::iota (order.begin (), order.end (), size_t (0));
    std::sort (order.begin (), order.end (), [&] (size_t a, size_t b) { return cells[a].sample < cells[b].sample; });
    for (const size_t i : order) {
        if (!out.samples.empty () && out.samples.back () == cells[i].sample)
            continue;
        out.samples.push_back (cells[i].sample);
        out.before.push_back (cells[i].before);
        out.after.push_back (cells[i].after);
        out.mostGained = (std::max) (out.mostGained, cells[i].after - cells[i].before);
    }

    std::map<uint64_t, std::vector<size_t>> bySurface;
    for (size_t i = 0; i < cells.size (); ++i)
        bySurface[cells[i].surface].push_back (i);
    std::map<long long, SolarCutCellGroup> groups;
    for (const auto& [key, members] : bySurface) {
        if (cancelled && cancelled ()) {
            out.error = "Cell re-count cancelled";
            return false;
        }
        const auto found = surfaces.find (key);
        if (found == surfaces.end () || found->second.empty ()) {
            out.unplaced += members.size ();
            continue;
        }
        const auto& triangles = found->second;
        sc::Frame frame;
        bool framed = false;
        for (const auto& t : triangles)
            if (sc::MakeFrame (At (t.v[0]), At (t.v[1]), At (t.v[2]), frame)) {
                framed = true;
                break;
            }
        if (!framed) {
            out.unplaced += members.size ();
            continue;
        }
        const uint32_t mesh = triangles.front ().mesh;
        cp::PathsD pieces;
        for (const auto& t : triangles)
            pieces.push_back (
                Oriented ({ frame.Project (At (t.v[0])), frame.Project (At (t.v[1])), frame.Project (At (t.v[2])) }));
        cp::PathsD domain;
        if (!sc::Boolean (cp::ClipType::Union, pieces, {}, domain, out.error))
            return false;
        std::map<long long, cp::PathsD> squares;
        cp::PathsD all;
        for (const size_t i : members) {
            const auto& cell = cells[i];
            cp::PathD square;
            for (const auto& corner : cell.corners)
                square.push_back (frame.Project (At (corner)));
            square = Oriented (std::move (square));
            squares[KeyOf (cell.after)].push_back (square);
            all.push_back (std::move (square));
        }
        for (auto& [hours, paths] : squares) {
            cp::PathsD region;
            if (!sc::Boolean (cp::ClipType::Intersection, paths, domain, region, out.error))
                return false;
            auto& group = groups[hours];
            group.after = float (double (hours) / 1000.0);
            if (!Fill (region, frame, mesh, group.triangles, out.error))
                return false;
        }
        cp::PathsD merged;
        if (!sc::Boolean (cp::ClipType::Intersection, all, domain, merged, out.error))
            return false;
        for (const auto& path : merged) {
            out.area += cp::Area (path);
            for (size_t i = 0; i < path.size (); ++i) {
                const V a = frame.Lift (path[i]), b = frame.Lift (path[(i + 1) % path.size ()]);
                out.outline.push_back ({ a.x, a.y, a.z, b.x, b.y, b.z });
            }
        }
        out.cells += members.size ();
    }
    out.area = std::abs (out.area);
    for (auto& [hours, group] : groups)
        if (!group.triangles.empty ())
            out.groups.push_back (std::move (group));
    return true;
}

} // namespace evp::sunstudy
