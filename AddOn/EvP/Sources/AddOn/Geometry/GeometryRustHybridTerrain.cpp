#include "Geometry/GeometryRustHybridTerrain.hpp"

#include "GeometryRustFfi.h"
#include "GeometryRustIdentityAbi.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace tapioca::geometry {
namespace {

constexpr uint32_t kNoOwner = std::numeric_limits<uint32_t>::max ();

std::string Failure (TapiocaStatus status, const std::array<char, 256>& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    return length != 0 ? std::string (buffer.data (), length)
                       : "Tapioca Rust hybrid terrain call failed with status " + std::to_string (status);
}

TapiocaTerrainSlope Slope (const TerrainSlope& slope)
{
    return { slope.a, slope.b, static_cast<uint32_t> (slope.unit), 0 };
}

TapiocaTerrainStation Station (const TerrainStation& station)
{
    TapiocaTerrainStation raw {};
    std::copy (station.position.begin (), station.position.end (), raw.position);
    raw.halfLeft = station.halfLeft;
    raw.halfRight = station.halfRight;
    raw.shoulderLeft = station.shoulderLeft;
    raw.shoulderRight = station.shoulderRight;
    raw.cutLimit = station.cutLimit;
    raw.fillLimit = station.fillLimit;
    raw.cut = Slope (station.cut);
    raw.fill = Slope (station.fill);
    if (station.crossfall) {
        raw.crossfall = Slope (station.crossfall->slope);
        raw.crossfallHand = static_cast<uint32_t> (station.crossfall->fallsTo);
    }
    else if (station.crown) {
        raw.crossfall = Slope (*station.crown);
        raw.crossfallHand = 3;
    }
    return raw;
}

HybridVolumes Volumes (const TapiocaTerrainHybridVolumes& raw)
{
    return { raw.cut, raw.fill, raw.net, raw.area, raw.triangles };
}

} // namespace

bool BuildHybridTerrain (const HybridTerrainRequest& request, HybridTerrainResult& result, std::string& error)
{
    detail::AbiProvenanceStorage storage;
    TapiocaTerrainHybridRequest input {};
    input.provenance = detail::ConvertProvenanceRequest (request.provenance, storage);
    input.points = { request.mesh.points.data (), request.mesh.points.size () };
    input.triangles = { request.mesh.triangles.data (), request.mesh.triangles.size () };
    input.region = { request.region.data (), request.region.size () };
    std::vector<TapiocaTerrainStation> stations;
    stations.reserve (request.stations.size ());
    for (const auto& station : request.stations) {
        if (station.crossfall && station.crown) {
            error = "TerrainHybridCamber: a station has either a one-way crossfall or a crown";
            return false;
        }
        stations.push_back (Station (station));
    }
    input.stations = { stations.data (), stations.size () };
    input.tolerance = request.tolerance;
    input.effect = static_cast<uint32_t> (request.effect);

    std::array<char, 256> buffer {};
    TapiocaTerrainHybridCounts counts {};
    TapiocaTerrainHybridVolumes site {}, context {};
    TapiocaTerrainHybridOutput output {};
    output.counts = &counts;
    output.site = &site;
    output.context = &context;
    output.queryOnly = 1;
    auto status = tapioca_terrain_hybrid_build (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    HybridTerrainResult candidate;
    std::vector<TapiocaTerrainMapping> mappings;
    std::vector<TapiocaTerrainHybridLine> lines;
    std::vector<uint32_t> owners;
    if (counts.points % 3 != 0 || counts.triangles % 3 != 0 || counts.points > candidate.points.max_size () ||
        counts.triangles > candidate.triangles.max_size () || counts.mappings > mappings.max_size () ||
        counts.lines > lines.max_size ()) {
        error = "TerrainOutputShape: invalid hybrid query counts";
        return false;
    }
    const auto expected = counts;
    candidate.points.resize (counts.points);
    candidate.base.resize (counts.points / 3);
    owners.resize (counts.points / 3);
    candidate.triangles.resize (counts.triangles);
    mappings.resize (counts.mappings);
    lines.resize (counts.lines);
    std::vector<TapiocaTerrainHybridSide> sides (stations.size () * 2);
    output.points = candidate.points.data ();
    output.pointCapacity = candidate.points.size ();
    output.base = candidate.base.data ();
    output.baseCapacity = candidate.base.size ();
    output.owners = owners.data ();
    output.ownerCapacity = owners.size ();
    output.triangles = candidate.triangles.data ();
    output.triangleCapacity = candidate.triangles.size ();
    output.mappings = mappings.data ();
    output.mappingCapacity = mappings.size ();
    output.lines = lines.data ();
    output.lineCapacity = lines.size ();
    output.sides = sides.data ();
    output.sideCapacity = sides.size ();
    output.queryOnly = 0;
    status = tapioca_terrain_hybrid_build (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    if (counts.points != expected.points || counts.triangles != expected.triangles ||
        counts.mappings != expected.mappings || counts.lines != expected.lines) {
        error = "TerrainOutputShape: hybrid query/fill counts disagree";
        return false;
    }
    for (const auto& mapping : mappings) {
        if (mapping.kind != 9 && (mapping.kind < 12 || mapping.kind > 17)) {
            error = "TerrainOutputShape: unknown hybrid support kind";
            return false;
        }
        TerrainMapping row { static_cast<TerrainMappingKind> (mapping.kind), mapping.index, {} };
        std::copy (std::begin (mapping.values), std::end (mapping.values), row.values.begin ());
        candidate.mappings.push_back (row);
    }
    for (const auto& line : lines) {
        if (line.kind < 1 || line.kind > 5 || line.side > 2 || (line.kind >= 4) != (line.side == 0)) {
            error = "TerrainOutputShape: unknown hybrid line kind";
            return false;
        }
        HybridLine value { static_cast<HybridLineKind> (line.kind), std::nullopt, line.root };
        if (line.side != 0)
            value.side = static_cast<TerrainHand> (line.side);
        candidate.lines.push_back (value);
    }
    for (size_t i = 0; i + 1 < sides.size (); i += 2) {
        candidate.sides.push_back ({ HybridSide { sides[i].roots, sides[i].first, sides[i].last },
                                     HybridSide { sides[i + 1].roots, sides[i + 1].first, sides[i + 1].last } });
    }
    candidate.owners.reserve (owners.size ());
    for (uint32_t owner : owners)
        candidate.owners.push_back (owner == kNoOwner ? std::nullopt : std::optional<uint32_t> (owner));
    candidate.site = Volumes (site);
    candidate.context = Volumes (context);
    candidate.inputProvenance = request.provenance;
    candidate.inputPointCount = request.mesh.points.size () / 3;
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
