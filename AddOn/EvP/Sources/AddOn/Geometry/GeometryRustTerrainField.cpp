#include "Geometry/GeometryRustTerrain.hpp"

#include "GeometryRustFfi.h"
#include "GeometryRustIdentityAbi.hpp"

#include <array>
#include <limits>
#include <set>
#include <vector>

namespace tapioca::geometry {
namespace {

constexpr uint32_t kNoOwner = std::numeric_limits<uint32_t>::max ();

std::string Failure (const char* family, TapiocaStatus status, const std::array<char, 256>& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    return length != 0 ? std::string (buffer.data (), length)
                       : std::string ("Tapioca Rust ") + family + " call failed with status " + std::to_string (status);
}

TapiocaTerrainSlope Slope (const TerrainSlope& slope)
{
    return { slope.a, slope.b, static_cast<uint32_t> (slope.unit), 0 };
}

TapiocaTerrainDaylight Daylight (const TerrainDaylight& side)
{
    return { Slope (side.slope), side.extension.value_or (0), side.extension ? 1U : 0U, 0 };
}

// Appends `xyz` to the geometry pool and returns its scalar range.
bool Append (std::vector<double>& pool, std::span<const double> xyz, TapiocaTerrainRing& range, std::string& error)
{
    constexpr auto limit = std::numeric_limits<uint32_t>::max ();
    if (pool.size () > limit || xyz.size () > limit - pool.size ()) {
        error = "TerrainGeometryRange: geometry pool overflow";
        return false;
    }
    range = { static_cast<uint32_t> (pool.size ()), static_cast<uint32_t> (xyz.size ()) };
    pool.insert (pool.end (), xyz.begin (), xyz.end ());
    return true;
}

} // namespace

bool SampleTerrain (const TerrainMeshView& mesh, std::span<const double> queriesXY, double step,
                    std::vector<TerrainSample>& samples, TerrainFieldSummary& summary, std::string& error)
{
    TapiocaTerrainSampleRequest input {};
    input.points = { mesh.points.data (), mesh.points.size () };
    input.triangles = { mesh.triangles.data (), mesh.triangles.size () };
    input.queries = { queriesXY.data (), queriesXY.size () };
    input.step = step;
    std::vector<TapiocaTerrainSample> raw (queriesXY.size () / 2);
    TapiocaTerrainFieldSummary field {};
    const TapiocaTerrainSampleOutput output { &field, raw.data (), raw.size () };
    std::array<char, 256> buffer {};
    const auto status = tapioca_terrain_sample (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure ("terrain sample", status, buffer);
        return false;
    }
    std::vector<TerrainSample> candidate;
    candidate.reserve (raw.size ());
    for (const auto& item : raw) {
        TerrainSample sample;
        sample.onSurface = (item.flags & TAPIOCA_SAMPLE_ON_SURFACE) != 0;
        if (sample.onSurface) {
            sample.height = item.height;
            sample.face = item.face;
            sample.normal = { item.normal[0], item.normal[1], item.normal[2] };
            sample.gradient = { item.gradient[0], item.gradient[1] };
            sample.method = { static_cast<TerrainGradientMethod> ((item.flags >> 2) & 3U),
                              static_cast<TerrainGradientMethod> ((item.flags >> 4) & 3U) };
            sample.slope = item.slope;
            if ((item.flags & TAPIOCA_SAMPLE_HAS_ASPECT) != 0)
                sample.aspect = item.aspect;
        }
        candidate.push_back (sample);
    }
    samples = std::move (candidate);
    summary = { { field.min[0], field.min[1], field.min[2] },
                { field.max[0], field.max[1], field.max[2] },
                field.area,
                field.faces,
                field.excluded };
    error.clear ();
    return true;
}

bool EditTerrain (const TerrainEditRequest& request, TerrainEditResult& result, std::string& error)
{
    std::vector<double> pool;
    std::vector<TapiocaTerrainEdit> edits;
    edits.reserve (request.edits.size ());
    for (const auto& edit : request.edits) {
        TapiocaTerrainEdit raw {};
        raw.position[0] = edit.position[0];
        raw.position[1] = edit.position[1];
        raw.position[2] = edit.position[2];
        raw.width = edit.width;
        raw.level = edit.level.value_or (0);
        raw.blur = edit.blur;
        raw.maxDistance = edit.maxDistance;
        raw.strength = edit.strength;
        raw.cut = Daylight (edit.cut);
        raw.fill = Daylight (edit.fill);
        raw.target = static_cast<uint32_t> (edit.target);
        raw.mode = static_cast<uint32_t> (edit.mode);
        raw.effect = static_cast<uint32_t> (edit.effect);
        raw.transition = static_cast<uint32_t> (edit.transition);
        raw.levelKind = edit.level ? 1U : 2U;
        if (edit.crossfall) {
            raw.crossfall = Slope (edit.crossfall->slope);
            raw.crossfallHand = static_cast<uint32_t> (edit.crossfall->fallsTo);
        }
        raw.surface = edit.surface;
        TapiocaTerrainRing range {};
        if (!Append (pool, edit.geometry, range, error))
            return false;
        raw.geometryOffset = range.offset;
        raw.geometryCount = range.count;
        edits.push_back (raw);
    }
    std::vector<TapiocaTerrainRing> rings;
    for (const auto& ring : request.protectedRings) {
        TapiocaTerrainRing range {};
        if (!Append (pool, ring, range, error))
            return false;
        rings.push_back (range);
    }
    TapiocaTerrainRing clip {};
    if (!request.clip.empty () && !Append (pool, request.clip, clip, error))
        return false;
    std::vector<TapiocaTerrainSurfaceView> surfaces;
    for (const auto& surface : request.surfaces)
        surfaces.push_back ({ { surface.points.data (), surface.points.size () },
                              { surface.triangles.data (), surface.triangles.size () } });

    detail::AbiProvenanceStorage storage;
    TapiocaTerrainEditRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.mesh.points.data (), request.mesh.points.size () };
    input.triangles = { request.mesh.triangles.data (), request.mesh.triangles.size () };
    input.edits = { edits.data (), edits.size () };
    input.geometry = { pool.data (), pool.size () };
    input.surfaces = { surfaces.data (), surfaces.size () };
    input.protectedRings = { rings.data (), rings.size () };
    input.clip = clip;
    input.hasClip = request.clip.empty () ? 0U : 1U;
    input.hasSmoothing = request.smoothing ? 1U : 0U;
    input.smoothing = request.smoothing.value_or (0);

    const size_t vertices = request.mesh.points.size () / 3;
    std::vector<double> heights (vertices);
    std::vector<uint32_t> owners (vertices);
    std::vector<TapiocaTerrainFaceOwner> faces ((request.mesh.triangles.size () / 3) * 3);
    std::vector<TapiocaTerrainEditReport> reports (edits.size ());
    size_t faceCount = 0;
    const TapiocaTerrainEditOutput output { &faceCount,     heights.data (), heights.size (),
                                            owners.data (), owners.size (),  faces.data (),
                                            faces.size (),  reports.data (), reports.size () };
    std::array<char, 256> buffer {};
    const auto status = tapioca_terrain_edit (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure ("terrain edit", status, buffer);
        return false;
    }
    if (faceCount > faces.size ()) {
        error = "TerrainOutputShape: face rows beyond capacity";
        return false;
    }
    TerrainEditResult candidate;
    candidate.heights = std::move (heights);
    candidate.owners.reserve (vertices);
    for (const uint32_t owner : owners)
        candidate.owners.push_back (owner == kNoOwner ? std::nullopt : std::optional<uint32_t> (owner));
    for (size_t row = 0; row < faceCount; ++row)
        candidate.faces.push_back ({ faces[row].face, faces[row].edit });
    size_t clipped = 0;
    for (const auto& report : reports) {
        candidate.reports.push_back ({ report.moved, report.extensionClipped, report.limitClipped });
        clipped += size_t (report.extensionClipped) + report.limitClipped;
    }
    // Located rows only when something was clipped; their count is known.
    if (clipped != 0) {
        std::vector<TapiocaTerrainClip> rows (clipped);
        size_t count = 0;
        const TapiocaTerrainClipOutput located { &count, rows.data (), rows.size (), 0, 0 };
        const auto clipStatus = tapioca_terrain_edit_clips (&input, &located, buffer.data (), buffer.size ());
        if (clipStatus != TAPIOCA_OK) {
            error = Failure ("terrain edit clips", clipStatus, buffer);
            return false;
        }
        if (count != clipped) {
            error = "TerrainOutputShape: clip rows disagree with the report counters";
            return false;
        }
        for (const auto& row : rows) {
            if (row.kind < 1 || row.kind > 2) {
                error = "TerrainOutputShape: unknown clip kind";
                return false;
            }
            candidate.clips.push_back (
                { row.edit, row.vertex, static_cast<TerrainClipKind> (row.kind), row.distance, row.withheld });
        }
    }
    candidate.inputProvenance = request.provenance;
    result = std::move (candidate);
    error.clear ();
    return true;
}

bool MeasureTerrainVolumes (const TerrainVolumeRequest& request, TerrainVolumeReport& report, std::string& error)
{
    std::vector<uint32_t> owners;
    owners.reserve (request.owners.size ());
    std::set<uint32_t> distinct;
    for (const auto& owner : request.owners) {
        owners.push_back (owner.value_or (kNoOwner));
        if (owner)
            distinct.insert (*owner);
    }
    TapiocaTerrainVolumeRequest input {};
    input.points = { request.existing.points.data (), request.existing.points.size () };
    input.triangles = { request.existing.triangles.data (), request.existing.triangles.size () };
    input.proposedPoints = { request.proposedMesh.points.data (), request.proposedMesh.points.size () };
    input.proposedTriangles = { request.proposedMesh.triangles.data (), request.proposedMesh.triangles.size () };
    input.heights = { request.heights.data (), request.heights.size () };
    input.owners = { owners.data (), owners.size () };
    input.property = { request.property.data (), request.property.size () };
    if (request.domain) {
        for (size_t index = 0; index < 4; ++index)
            input.domain[index] = (*request.domain)[index];
    }
    input.plane = request.plane;
    input.cell = request.cell;
    input.proposed = static_cast<uint32_t> (request.proposed);
    input.subdivisions = request.subdivisions;
    input.hasDomain = request.domain ? 1U : 0U;

    TapiocaTerrainVolumes site {}, context {};
    std::array<uint32_t, 2> cells {};
    size_t editCount = 0;
    std::vector<TapiocaTerrainEditVolumes> edits (distinct.size ());
    const TapiocaTerrainVolumeOutput output {
        &site, &context, cells.data (), &editCount, edits.data (), edits.size ()
    };
    std::array<char, 256> buffer {};
    const auto status = tapioca_terrain_volume (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure ("terrain volume", status, buffer);
        return false;
    }
    if (editCount > edits.size ()) {
        error = "TerrainOutputShape: edit rows beyond capacity";
        return false;
    }
    const auto convert = [] (const TapiocaTerrainVolumes& v) {
        return TerrainVolumes { v.cut, v.fill, v.net, v.area, v.samples, v.excluded };
    };
    TerrainVolumeReport candidate;
    candidate.site = convert (site);
    candidate.context = convert (context);
    candidate.cells = cells;
    for (size_t row = 0; row < editCount; ++row)
        candidate.edits.push_back (
            { edits[row].edit, edits[row].cut, edits[row].fill, edits[row].net, edits[row].area });
    report = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
