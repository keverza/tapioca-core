#include "Geometry/GeometryRustOutlineTerrain.hpp"

#include "GeometryRustFfi.h"

#include <array>
#include <vector>

namespace tapioca::geometry {
namespace {

std::string Failure (TapiocaStatus status, const std::array<char, 256>& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    return length != 0 ? std::string (buffer.data (), length)
                       : "Tapioca Rust outline terrain call failed with status " + std::to_string (status);
}

TapiocaTerrainSlope Slope (const TerrainSlope& slope)
{
    return { slope.a, slope.b, static_cast<uint32_t> (slope.unit), 0 };
}

HybridVolumes Volumes (const TapiocaTerrainHybridVolumes& v)
{
    return { v.cut, v.fill, v.net, v.area, v.triangles };
}

} // namespace

bool BuildOutlineTerrain (const OutlineTerrainRequest& request, OutlineTerrainResult& result, std::string& error)
{
    std::vector<double> linePoints;
    std::vector<uint32_t> lineCounts;
    for (const auto& line : request.lines) {
        if (line.size () % 3 != 0 || line.size () / 3 > UINT32_MAX) {
            error = "TerrainOutlineLines: a breakline is flat xyz";
            return false;
        }
        lineCounts.push_back (static_cast<uint32_t> (line.size () / 3));
        linePoints.insert (linePoints.end (), line.begin (), line.end ());
    }
    TapiocaTerrainOutlineRequest raw {};
    raw.points = { request.mesh.points.data (), request.mesh.points.size () };
    raw.triangles = { request.mesh.triangles.data (), request.mesh.triangles.size () };
    raw.region = { request.region.data (), request.region.size () };
    raw.surfacePoints = { request.surface.points.data (), request.surface.points.size () };
    raw.surfaceTriangles = { request.surface.triangles.data (), request.surface.triangles.size () };
    raw.linePoints = { linePoints.data (), linePoints.size () };
    raw.lineCounts = { lineCounts.data (), lineCounts.size () };
    raw.spots = { request.spots.data (), request.spots.size () };
    raw.cut = Slope (request.cut);
    raw.fill = Slope (request.fill);
    raw.limit = request.limit;
    raw.rounding = request.rounding;
    raw.tolerance = request.tolerance;
    raw.effect = static_cast<uint32_t> (request.effect);
    std::array<char, 256> buffer {};
    TapiocaTerrainOutlineCounts counts {};
    TapiocaTerrainHybridVolumes site {}, context {};
    TapiocaTerrainOutlineOutput output {};
    output.counts = &counts;
    output.site = &site;
    output.context = &context;
    output.queryOnly = 1;
    auto status = tapioca_terrain_outline_build (&raw, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    OutlineTerrainResult candidate;
    candidate.points.resize (counts.points);
    candidate.base.resize (counts.points / 3);
    candidate.triangles.resize (counts.triangles);
    candidate.ring.resize (counts.ring);
    output.points = candidate.points.data ();
    output.pointCapacity = candidate.points.size ();
    output.base = candidate.base.data ();
    output.baseCapacity = candidate.base.size ();
    output.triangles = candidate.triangles.data ();
    output.triangleCapacity = candidate.triangles.size ();
    output.ring = candidate.ring.data ();
    output.ringCapacity = candidate.ring.size ();
    output.queryOnly = 0;
    const auto expected = counts;
    status = tapioca_terrain_outline_build (&raw, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    if (counts.points != expected.points || counts.triangles != expected.triangles || counts.ring != expected.ring) {
        error = "TerrainOutputShape: the outline build changed between query and fill";
        return false;
    }
    candidate.site = Volumes (site);
    candidate.context = Volumes (context);
    candidate.unreached = counts.unreached;
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
