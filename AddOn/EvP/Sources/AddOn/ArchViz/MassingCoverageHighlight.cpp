#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/TerrainProjection.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::massingslices {
bool UnbuiltHighlight (const Result& result, const Mesh* terrain, overlaylayers::Layer& plan,
                       overlaylayers::Layer& projected, std::string& error)
{
    error.clear ();
    if (!result.hasCoverage)
        return error = "Parcel coverage highlight awaits complete current footprint geometry.", false;
    namespace cp = Clipper2Lib;
    double ox = 0, oy = 0, drawingZ = 0;
    bool origin = false, elevation = false;
    cp::PathsD paths;
    size_t points = 0;
    for (const auto& patch : result.coverage) {
        if (patch.hasElevation && std::isfinite (patch.z) && (!elevation || patch.z < drawingZ)) {
            elevation = true;
            drawingZ = patch.z;
        }
        for (const auto& chain : patch.unbuilt) {
            if (!chain.closed || chain.xy.size () % 2 || chain.Count () < 3)
                return error = "Invalid unbuilt coverage contour.", false;
            if (!origin) {
                ox = chain.xy[0];
                oy = chain.xy[1];
                origin = true;
            }
            cp::PathD path;
            for (size_t i = 0; i < chain.Count (); ++i) {
                const double x = chain.xy[i * 2], y = chain.xy[i * 2 + 1];
                if (!std::isfinite (x) || !std::isfinite (y) || std::abs (x) > 1e9 || std::abs (y) > 1e9)
                    return error = "Invalid unbuilt coverage coordinate.", false;
                path.emplace_back (x - ox, y - oy);
            }
            points += path.size ();
            if (points > 200000)
                return error = "Unbuilt coverage contour budget exceeded.", false;
            paths.push_back (std::move (path));
        }
    }
    // Stats still reports a parcel sum; inspection is one geometric union so
    // overlapping parcels never double-darken the same hatch or fill holes.
    std::vector<SliceChain> chains;
    for (const auto& path : cp::Union (paths, cp::FillRule::NonZero, 6)) {
        SliceChain chain;
        chain.closed = true;
        for (const auto& p : path)
            chain.xy.insert (chain.xy.end (), { p.x + ox, p.y + oy });
        chains.push_back (std::move (chain));
    }
    terrainprojection::Style style;
    style.name = kHighlightLayer;
    style.category = "coverageHighlight";
    style.fillRgba = 0; // Strokes only in both views: no solid coverage highlight.
    style.hatchRgba = 0x66BB6AFF;
    overlaylayers::Layer planOut, projectedOut;
    if (!terrainprojection::HatchPlan (chains, 0, drawingZ, style, planOut, error))
        return false;
    style.name = kUnbuiltProjectedLayer;
    if (terrain) {
        if (!terrainprojection::Project (chains, 0, *terrain, style, projectedOut, error))
            return false;
    }
    else {
        projectedOut.name = kUnbuiltProjectedLayer;
        projectedOut.graphicsCategory = style.category;
        projectedOut.views = overlaylayers::Views::ThreeD;
    }
    plan = std::move (planOut);
    projected = std::move (projectedOut);
    return true;
}
} // namespace geomsrv::archviz::massingslices
