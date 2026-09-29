// ArchViz/SlabSlices -- see the header.

#include "ArchViz/SlabSlices.hpp"

#include "ArchViz/PlanOverlayContent.hpp" // TessellateEdgeDouble: the one reading of an arc

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
    for (size_t k = 0; k < summary.floors.size (); ++k) {
        storysliceoverlay::Slice slice;
        slice.chains = chains;
        slice.z = (std::min) ((std::max) (summary.floors[k].base + rule.offsetMetres, slab.bottom), slab.top);
        slice.areaM2 = summary.sliceAreaM2;
        slice.name = owner + " F" + std::to_string (k + 1);
        slice.storey = StoreyAt (storeys, slice.z);
        out.push_back (std::move (slice));
    }
    summary.areaM2 = summary.sliceAreaM2 * double (summary.floors.size ());
    return summary;
}

} // namespace slabslices
} // namespace archviz
} // namespace geomsrv
