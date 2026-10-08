// ArchViz/SlabSlices -- see the header.

#include "ArchViz/SlabSlices.hpp"

#include "ArchViz/PlanOverlayContent.hpp" // TessellateEdgeDouble: the one reading of an arc
#include "Geometry/SliceEngine.hpp"       // SliceMesh: the one plane cut of a mesh

#include <algorithm>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace slabslices {

namespace {

constexpr double kEpsilon = 1e-6; // a micron: two heights closer than this are one
constexpr size_t kMaxFloors = 1000;
// The DevKit's reading of an arc, as the plan overlay draws the walls' arcs.
constexpr double kArcSign = 1.0;
// Five centimetres per chord: a 10 m radius strays 0.03 mm from its arc.
constexpr double kChordMetres = 0.05;

double SignedArea (const std::vector<double>& xy)
{
    const size_t n = xy.size () / 2;
    double area = 0.0;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        area += xy[j * 2] * xy[i * 2 + 1] - xy[i * 2] * xy[j * 2 + 1];
    return n < 3 ? 0.0 : area * 0.5;
}

bool InsideRing (double x, double y, const std::vector<double>& xy)
{
    const size_t n = xy.size () / 2;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = xy[i * 2], yi = xy[i * 2 + 1];
        const double xj = xy[j * 2], yj = xy[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            inside = !inside;
    }
    return n >= 3 && inside;
}

// The cross-section of `body` at `z`: its loops as contours, and the area its closed
// rings enclose, even-odd -- one solid's rings are disjoint, so a ring inside an odd
// number of others is a hole. An open chain (a body that is not closed there) is kept
// for the outline and counts for nothing.
std::vector<SliceChain> CrossSection (const Mesh& body, double z, double& area)
{
    area = 0.0;
    std::vector<SliceChain> chains;
    if (body.vertices.empty () || body.triangles.empty ())
        return chains;
    if (body.bounds.Valid () && (body.bounds.mn[2] > z || body.bounds.mx[2] < z))
        return chains;
    // The storey slices' tangency lift (ExtractionStorySlices.cpp): a plane on a face
    // of the body has no cross-section there.
    if (IsTangentToPlane (body.vertices.data (), body.VertexCount (), z))
        z += 1e-6;
    const std::vector<Polyline> loops =
        SliceMesh (body.vertices.data (), body.VertexCount (), body.triangles.data (), body.TriangleCount (), z);
    for (const Polyline& loop : loops) {
        SliceChain chain;
        chain.closed = loop.closed;
        // A closed loop ends on its start; a contour does not repeat it.
        const size_t n = loop.PointCount () - (loop.closed && loop.PointCount () > 1 ? 1 : 0);
        for (size_t i = 0; i < n; ++i) {
            chain.xy.push_back (loop.pts[i * 3]);
            chain.xy.push_back (loop.pts[i * 3 + 1]);
        }
        if (chain.Count () >= (chain.closed ? 3u : 2u))
            chains.push_back (std::move (chain));
    }
    for (size_t i = 0; i < chains.size (); ++i) {
        if (!chains[i].closed)
            continue;
        size_t around = 0;
        for (size_t j = 0; j < chains.size (); ++j)
            if (j != i && chains[j].closed && InsideRing (chains[i].xy[0], chains[i].xy[1], chains[j].xy))
                ++around;
        const double ring = std::fabs (SignedArea (chains[i].xy));
        area += around % 2 == 0 ? ring : -ring;
    }
    area = (std::max) (area, 0.0);
    return chains;
}

void StoreyFloors (double bottom, double top, double minTop, const std::vector<double>& levels,
                   std::vector<Floor>& floors, std::string& problem)
{
    if (levels.size () < 2) {
        problem = "the storey rule needs two storeys to know a storey's height; use step or levels";
        return;
    }
    std::vector<double> gaps;
    for (size_t i = 0; i + 1 < levels.size (); ++i) {
        const double gap = levels[i + 1] - levels[i];
        if (gap <= kEpsilon) {
            problem = "the project's storey levels do not rise";
            return;
        }
        gaps.push_back (gap);
    }
    // The storey the bottom sits in, or the lowest one below them all.
    const size_t above =
        size_t (std::upper_bound (levels.begin (), levels.end (), bottom + kEpsilon) - levels.begin ());
    size_t index = above == 0 ? 0 : above - 1;
    double base = bottom;
    double remaining = top - bottom;
    while (remaining > kEpsilon && floors.size () < kMaxFloors) {
        const double gap = gaps[(std::min) (index, gaps.size () - 1)];
        if (remaining + kEpsilon < gap) {
            if (remaining + kEpsilon >= minTop)
                floors.push_back ({ base, remaining });
            break;
        }
        floors.push_back ({ base, gap });
        base += gap;
        remaining -= gap;
        ++index;
    }
}

void StoreyLevelFloors (double bottom, double top, double minTop, const std::vector<double>& levels,
                        std::vector<Floor>& floors)
{
    for (size_t i = 0; i < levels.size () && floors.size () < kMaxFloors; ++i) {
        const double level = levels[i];
        if (level < bottom - kEpsilon || level >= top - kEpsilon)
            continue;
        const bool complete = i + 1 < levels.size () && levels[i + 1] <= top + kEpsilon;
        const double height = (complete ? levels[i + 1] : top) - level;
        if (complete || height + kEpsilon >= minTop)
            floors.push_back ({ level, height });
    }
}

void StepFloors (double bottom, double top, const Rule& rule, std::vector<Floor>& floors, std::string& problem)
{
    if (!(rule.stepMetres >= 0.001)) {
        problem = "the floor height must be at least 1 mm";
        return;
    }
    const double height = top - bottom;
    const size_t whole = size_t (height / rule.stepMetres + 1e-9);
    for (size_t k = 0; k < whole && floors.size () < kMaxFloors; ++k)
        floors.push_back ({ bottom + double (k) * rule.stepMetres, rule.stepMetres });
    const double remainder = height - double (whole) * rule.stepMetres;
    if (remainder > kEpsilon && remainder + kEpsilon >= rule.minTopMetres && floors.size () < kMaxFloors)
        floors.push_back ({ bottom + double (whole) * rule.stepMetres, remainder });
}

void LevelFloors (double bottom, double top, const std::vector<double>& given, std::vector<Floor>& floors)
{
    std::vector<double> levels;
    for (const double level : given)
        if (level >= bottom - kEpsilon && level <= top + kEpsilon)
            levels.push_back ((std::min) ((std::max) (level, bottom), top));
    std::sort (levels.begin (), levels.end ());
    levels.erase (std::unique (levels.begin (), levels.end (), [] (double a, double b) { return b - a < kEpsilon; }),
                  levels.end ());
    for (size_t i = 0; i < levels.size () && floors.size () < kMaxFloors; ++i)
        floors.push_back ({ levels[i], (i + 1 < levels.size () ? levels[i + 1] : top) - levels[i] });
}

} // namespace

const char* CutName (Cut cut)
{
    switch (cut) {
        case Cut::Storeys:
            return "storeys";
        case Cut::StoreyLevels:
            return "storeyLevels";
        case Cut::Step:
            return "step";
        case Cut::Levels:
            return "levels";
    }
    return "storeys";
}

std::vector<Floor> Floors (double bottom, double top, const Rule& rule, const std::vector<double>& levels,
                           std::string& problem)
{
    std::vector<Floor> floors;
    problem.clear ();
    if (!(top - bottom > kEpsilon)) {
        problem = "the slab has no height";
        return floors;
    }
    switch (rule.cut) {
        case Cut::Storeys:
            StoreyFloors (bottom, top, rule.minTopMetres, levels, floors, problem);
            break;
        case Cut::StoreyLevels:
            StoreyLevelFloors (bottom, top, rule.minTopMetres, levels, floors);
            break;
        case Cut::Step:
            StepFloors (bottom, top, rule, floors, problem);
            break;
        case Cut::Levels:
            LevelFloors (bottom, top, rule.levels, floors);
            break;
    }
    if (floors.size () >= kMaxFloors)
        problem = "stopped at " + std::to_string (kMaxFloors) + " floors";
    else if (floors.empty () && problem.empty ())
        problem = rule.cut == Cut::Levels         ? "no level given lies within the slab"
                  : rule.cut == Cut::StoreyLevels ? "no storey level lies within the slab"
                                                  : "the slab is lower than one floor";
    return floors;
}

SliceChain Contour (const Ring& ring, double chordMetres)
{
    SliceChain chain;
    chain.closed = true;
    const size_t n = ring.xy.size () / 2;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const double arc = i < ring.arcs.size () ? ring.arcs[i] : 0.0;
        plancontent::TessellateEdgeDouble (ring.xy[i * 2], ring.xy[i * 2 + 1], ring.xy[j * 2], ring.xy[j * 2 + 1], arc,
                                           kArcSign, chordMetres, chain.xy);
    }
    return chain;
}

int StoreyAt (const ProjectStoreys& storeys, double z)
{
    if (storeys.levels.empty ())
        return 0;
    size_t at = 0;
    for (size_t i = 0; i < storeys.levels.size (); ++i)
        if (storeys.levels[i] <= z + kEpsilon)
            at = i;
    return at < storeys.indices.size () ? storeys.indices[at] : int (at);
}

std::vector<int> StoreysAt (const ProjectStoreys& storeys, const std::vector<double>& heights)
{
    std::vector<int> out (heights.size (), 0);
    if (heights.empty ())
        return out;
    int first = 0;
    if (!storeys.levels.empty ()) {
        const double lowest = storeys.levels.front (), highest = storeys.levels.back ();
        if (heights.front () < lowest - kEpsilon) {
            int below = 0;
            for (double z : heights)
                below += z < lowest - kEpsilon ? 1 : 0;
            first = StoreyAt (storeys, lowest) - below;
        }
        else if (heights.front () > highest + kEpsilon)
            first = StoreyAt (storeys, highest) + 1;
        else
            first = StoreyAt (storeys, heights.front ());
    }
    // Slice floors, not the project story containing each cut: two different
    // floors within one tall Archicad story must never reuse its number.
    for (size_t k = 0; k < heights.size (); ++k)
        out[k] = first + int (k);
    return out;
}

namespace {

// Where each floor is cut: its base raised by the rule's offset, kept within the slab.
std::vector<double> CutHeights (const std::vector<Floor>& floors, const Rule& rule, const Slab& slab)
{
    std::vector<double> heights;
    heights.reserve (floors.size ());
    for (const Floor& floor : floors)
        heights.push_back ((std::min) ((std::max) (floor.base + rule.offsetMetres, slab.bottom), slab.top));
    return heights;
}

} // namespace

Summary SliceSlab (const Slab& slab, const Rule& rule, const ProjectStoreys& storeys,
                   std::vector<storysliceoverlay::Slice>& out)
{
    Summary summary;
    summary.guid = slab.guid;
    summary.id = slab.id;
    summary.bottom = slab.bottom;
    summary.top = slab.top;
    summary.slopedEdges = slab.slopedEdges;

    std::vector<SliceChain> chains;
    const SliceChain outer = Contour (slab.outer, kChordMetres);
    if (outer.Count () < 3) {
        summary.problem = "the slab's outline has fewer than three corners";
        return summary;
    }
    chains.push_back (outer);
    // The area in double, from the rings themselves: a slab's holes lie inside its
    // outline and apart, so the outline less the holes is the slice. The fill's
    // triangles are float, which a survey-placed project cannot afford for a figure.
    summary.footprintM2 = std::fabs (SignedArea (outer.xy));
    summary.sliceAreaM2 = summary.footprintM2;
    for (const Ring& hole : slab.holes) {
        SliceChain chain = Contour (hole, kChordMetres);
        if (chain.Count () < 3)
            continue;
        summary.sliceAreaM2 -= std::fabs (SignedArea (chain.xy));
        chains.push_back (std::move (chain));
    }
    summary.sliceAreaM2 = (std::max) (summary.sliceAreaM2, 0.0);

    summary.floors = Floors (slab.bottom, slab.top, rule, storeys.levels, summary.problem);
    const std::string owner = slab.id.empty () ? std::string ("Slab") : slab.id;
    const std::vector<double> heights = CutHeights (summary.floors, rule, slab);
    const std::vector<int> storeyOf = StoreysAt (storeys, heights);
    for (size_t k = 0; k < summary.floors.size (); ++k) {
        storysliceoverlay::Slice slice;
        slice.chains = chains;
        slice.z = heights[k];
        slice.areaM2 = summary.sliceAreaM2;
        slice.name = owner + " F" + std::to_string (k + 1);
        slice.storey = storeyOf[k];
        summary.floors[k].areaM2 = summary.sliceAreaM2;
        out.push_back (std::move (slice));
    }
    summary.areaM2 = summary.sliceAreaM2 * double (summary.floors.size ());
    return summary;
}

bool FromBody (Slab& source, const Mesh& body, std::string& error)
{
    error.clear ();
    if (body.vertices.empty () || body.triangles.empty () || body.vertices.size () % 3 || body.triangles.size () % 3 ||
        body.vertices.size () > 600000 || body.triangles.size () > 600000) {
        error = "Massing source body is empty or exceeds its geometry budget.";
        return false;
    }
    double bottom = 1e300, top = -1e300;
    for (size_t i = 0; i < body.vertices.size (); ++i) {
        const double coordinate = body.vertices[i];
        if (!std::isfinite (coordinate) || std::abs (coordinate) > 1e9) {
            error = "Invalid massing source body coordinate.";
            return false;
        }
        if (i % 3 == 2) {
            bottom = (std::min) (bottom, coordinate);
            top = (std::max) (top, coordinate);
        }
    }
    for (uint32_t index : body.triangles)
        if (index >= body.VertexCount ()) {
            error = "Invalid massing source body index.";
            return false;
        }
    if (top - bottom <= 1e-6) {
        error = "Massing source needs a 3D volume, not a flat surface.";
        return false;
    }
    source.bottom = bottom;
    source.top = top;
    source.bodyRequired = true;
    return true;
}

Summary SliceBody (const Slab& slab, const Mesh& body, const Rule& rule, const ProjectStoreys& storeys,
                   std::vector<storysliceoverlay::Slice>& out, bool keepEmptyFloors)
{
    Summary summary;
    summary.guid = slab.guid;
    summary.id = slab.id;
    summary.bottom = slab.bottom;
    summary.top = slab.top;
    summary.body = true;
    const SliceChain outer = Contour (slab.outer, kChordMetres);
    summary.footprintM2 = outer.Count () < 3 ? 0.0 : std::fabs (SignedArea (outer.xy));

    summary.floors = Floors (slab.bottom, slab.top, rule, storeys.levels, summary.problem);
    const std::string owner = slab.id.empty () ? std::string ("Slab") : slab.id;
    // Numbered over every floor, so one the operations removed leaves its number unused.
    const std::vector<double> heights = CutHeights (summary.floors, rule, slab);
    const std::vector<int> storeyOf = StoreysAt (storeys, heights);
    uint32_t emptied = 0;
    for (size_t k = 0; k < summary.floors.size (); ++k) {
        storysliceoverlay::Slice slice;
        slice.z = heights[k];
        slice.chains = CrossSection (body, slice.z, slice.areaM2);
        summary.floors[k].areaM2 = slice.areaM2;
        if (slice.chains.empty ()) {
            ++emptied;
            if (!keepEmptyFloors)
                continue;
        }
        slice.name = owner + " F" + std::to_string (k + 1);
        slice.storey = storeyOf[k];
        summary.areaM2 += slice.areaM2;
        summary.sliceAreaM2 = (std::max) (summary.sliceAreaM2, slice.areaM2);
        out.push_back (std::move (slice));
    }
    if (emptied != 0 && !keepEmptyFloors && summary.problem.empty ())
        summary.problem = std::to_string (emptied) + " floor(s) with no cross-section: the operations removed them";
    return summary;
}

} // namespace slabslices
} // namespace archviz
} // namespace geomsrv
