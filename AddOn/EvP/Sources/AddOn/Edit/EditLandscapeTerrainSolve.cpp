#include "Edit/EditLandscapeTerrain.hpp"
#include "Edit/EditLandscapeTerrainInternal.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include <array>
#include <bit>
#include <exception>

namespace tapioca::geometry {
namespace {
using namespace landscape_detail;

enum class Place { Inside, On, Outside };

// Nonzero winding over Clipper-oriented output: outers positive, holes negative.
// A mask edge is reported separately so callers can treat closed regions as closed.
Place Locate (const cp::PathsD& paths, double x, double y)
{
    int winding = 0;
    for (const auto& path : paths) {
        const auto place = cp::PointInPolygon (cp::PointD (x, y), path);
        if (place == cp::PointInPolygonResult::IsOn)
            return Place::On;
        if (place == cp::PointInPolygonResult::IsInside)
            winding += cp::IsPositive (path) ? 1 : -1;
    }
    return winding > 0 ? Place::Inside : Place::Outside;
}

std::vector<double> Flatten (const std::vector<Vec3>& ring)
{
    std::vector<double> result;
    result.reserve (ring.size () * 3);
    for (const auto& point : ring)
        result.insert (result.end (), { point.x, point.y, point.z });
    return result;
}

bool Apply (const geomsrv::edit::Document& doc, const SplitLandscapeSolveRequest& request,
            SplitLandscapeSolveResult& candidate, std::string& error)
{
    const auto& baseline = request.baseline;
    const auto& split = baseline.split;
    const EditTerrainInput input {
        { split.points, split.triangles }, baseline.meshProvenance, baseline.revision, baseline.sourceTerrainGuid
    };
    if (!terrain_detail::ValidateInput (doc, input, error) ||
        !terrain_detail::ValidateOperations (doc, input.provenance, std::array<OperationId, 1> { request.operation },
                                             error))
        return false;
    LandscapeResult landscape;
    landscape_terrain_detail::ScopeMasks masks;
    if (!landscape_terrain_detail::DeriveScopeMasks (doc, input.provenance, request.operation, landscape, masks, error))
        return false;
    const size_t vertexCount = split.points.size () / 3;
    for (uint32_t vertex : request.vertices) {
        if (vertex >= vertexCount)
            continue; // The solve adapter names the locator refusal.
        const double x = split.points[vertex * 3], y = split.points[vertex * 3 + 1];
        if (Locate (masks.protectedPaths, x, y) != Place::Outside) {
            error = "SolveProtectedVertex: an unknown lies on or inside a protected zone";
            return false;
        }
        if (masks.envelope && Locate (masks.allowed, x, y) != Place::Inside) {
            error = "SolveOutsideGradingLimit: an unknown lies on or outside its grading-limit envelope";
            return false;
        }
    }
    if (!SolveEditTerrain (doc, { input, request.operation, request.vertices, request.system }, candidate.solve, error))
        return false;

    auto& terrain = candidate.terrain;
    terrain.revision = baseline.revision;
    terrain.sourceTerrainGuid = baseline.sourceTerrainGuid;
    terrain.points = split.points;
    terrain.triangles = split.triangles;
    terrain.inputProvenance = baseline.meshProvenance;
    terrain.vertexOperations.assign (vertexCount, std::nullopt);
    std::vector<std::optional<uint32_t>> owners (vertexCount);
    TerrainEditReport report;
    const auto& values = candidate.solve.solution.values;
    for (size_t i = 0; i < request.vertices.size (); ++i) {
        const uint32_t vertex = request.vertices[i];
        auto& height = terrain.points.at (vertex * 3 + 2);
        if (std::bit_cast<uint64_t> (height) == std::bit_cast<uint64_t> (values.at (i)))
            continue;
        height = values[i];
        terrain.vertexOperations[vertex] = request.operation;
        owners[vertex] = 0; // The only edit index of the volume request below.
        ++report.moved;
    }
    for (size_t face = 0; face < split.triangles.size () / 3; ++face)
        if (owners.at (split.triangles[face * 3]) || owners.at (split.triangles[face * 3 + 1]) ||
            owners.at (split.triangles[face * 3 + 2]))
            terrain.faces.push_back ({ static_cast<uint32_t> (face), request.operation });
    terrain.reports = { { request.operation, report } };
    if (!SampleTerrain ({ terrain.points, terrain.triangles }, request.queriesXY, request.sampleStep, terrain.samples,
                        terrain.summary, error) ||
        !VerifyEditTerrainOutside (baseline, terrain, candidate.preservation, error))
        return false;
    if (request.volumes) {
        std::vector<double> heights (vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
            heights[i] = terrain.points[i * 3 + 2];
        const auto property = Flatten (landscape.propertyLine);
        TerrainVolumeRequest volume;
        volume.existing = input.mesh;
        volume.proposed = TerrainVolumeRequest::Proposed::Edited;
        volume.heights = heights;
        volume.owners = owners;
        volume.cell = request.volumes->cell;
        volume.subdivisions = request.volumes->subdivisions;
        volume.property = property;
        TerrainVolumeReport measured;
        if (!MeasureTerrainVolumes (volume, measured, error))
            return false;
        EditTerrainVolumes volumes;
        volumes.site = measured.site;
        volumes.context = measured.context;
        volumes.cells = measured.cells;
        for (const auto& edit : measured.edits) {
            if (edit.edit != 0) {
                error = "TerrainOutputShape: volume report names an unknown edit";
                return false;
            }
            volumes.operations.push_back ({ request.operation, edit.cut, edit.fill, edit.net, edit.area });
        }
        terrain.volumes = std::move (volumes);
    }
    return true;
}
} // namespace

bool ApplySplitLandscapeSolve (const geomsrv::edit::Document& doc, const SplitLandscapeSolveRequest& request,
                               SplitLandscapeSolveResult& result, std::string& error)
{
    try {
        SplitLandscapeSolveResult candidate;
        if (!Apply (doc, request, candidate, error))
            return false;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainSolveRefused: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
