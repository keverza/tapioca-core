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
    raw.rounding = station.rounding;
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

struct Input {
    detail::AbiProvenanceStorage storage;
    std::vector<TapiocaTerrainStation> stations;
    TapiocaTerrainHybridRequest raw {};
    std::vector<double> controlGeometry;
    std::vector<TapiocaTerrainRing> controlLines;
    TapiocaTerrainHybridLockedRequest fixed {};
    bool locked = false;
};

bool Convert (const HybridTerrainRequest& request, Input& input, std::string& error)
{
    input.raw.provenance = detail::ConvertProvenanceRequest (request.provenance, input.storage);
    input.raw.points = { request.mesh.points.data (), request.mesh.points.size () };
    input.raw.triangles = { request.mesh.triangles.data (), request.mesh.triangles.size () };
    input.raw.region = { request.region.data (), request.region.size () };
    input.stations.reserve (request.stations.size ());
    for (const auto& station : request.stations) {
        if (station.crossfall && station.crown) {
            error = "TerrainHybridCamber: a station has either a one-way crossfall or a crown";
            return false;
        }
        input.stations.push_back (Station (station));
    }
    input.raw.stations = { input.stations.data (), input.stations.size () };
    input.raw.tolerance = request.tolerance;
    input.raw.effect = static_cast<uint32_t> (request.effect);
    input.locked = !request.controlLines.empty () || !request.controlPoints.empty ();
    for (const auto& line : request.controlLines) {
        if (line.size () > UINT32_MAX || input.controlGeometry.size () > UINT32_MAX - line.size ()) {
            error = "TerrainCountOverflow: control line pool exceeds 32-bit ranges";
            return false;
        }
        input.controlLines.push_back (
            { static_cast<uint32_t> (input.controlGeometry.size ()), static_cast<uint32_t> (line.size ()) });
        input.controlGeometry.insert (input.controlGeometry.end (), line.begin (), line.end ());
    }
    input.fixed.band = input.raw;
    input.fixed.linePoints = { input.controlGeometry.data (), input.controlGeometry.size () };
    input.fixed.lines = { input.controlLines.data (), input.controlLines.size () };
    input.fixed.points = { request.controlPoints.data (), request.controlPoints.size () };
    return true;
}

bool Control (const Input& input, std::array<uint32_t, 3> fields, std::optional<HybridControlRef>& value,
              std::string& error)
{
    const auto [kind, index, segment] = fields;
    if (kind == 0 && index == 0 && segment == 0) {
        value.reset ();
        return true;
    }
    if ((kind == 2 && segment == 0 && index < input.fixed.points.count / 3) ||
        (kind == 1 && index < input.controlLines.size () &&
         size_t (segment) + 1 < input.controlLines[index].count / 3)) {
        value = HybridControlRef { static_cast<HybridControlRef::Kind> (kind), index, segment };
        return true;
    }
    error = "TerrainOutputShape: unknown fixed-control support";
    return false;
}

TapiocaStatus Build (const Input& input, const TapiocaTerrainHybridOutput& output, std::array<char, 256>& error)
{
    return input.locked ? tapioca_terrain_hybrid_locked_build (&input.fixed, &output, error.data (), error.size ())
                        : tapioca_terrain_hybrid_build (&input.raw, &output, error.data (), error.size ());
}

bool Conflicts (const TapiocaTerrainHybridRequest& input, std::vector<HybridConflict>& result, std::string& error)
{
    std::array<char, 256> buffer {};
    size_t count = 0;
    TapiocaTerrainConflictOutput output { &count, nullptr, 0, 1, 0 };
    auto status = tapioca_terrain_hybrid_conflicts (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    std::vector<TapiocaTerrainConflict> rows (count);
    const size_t expected = count;
    output = { &count, rows.data (), rows.size (), 0, 0 };
    status = tapioca_terrain_hybrid_conflicts (&input, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    if (count != expected) {
        error = "TerrainOutputShape: conflict query/fill counts disagree";
        return false;
    }
    std::vector<HybridConflict> candidate;
    for (const auto& row : rows) {
        if (row.code < 1 || row.code > 5 || row.side < 1 || row.side > 2 || row.hard > 1) {
            error = "TerrainOutputShape: unknown conflict row";
            return false;
        }
        candidate.push_back ({ static_cast<HybridConflictCode> (row.code),
                               row.station,
                               static_cast<TerrainHand> (row.side),
                               { row.position[0], row.position[1], row.position[2] },
                               row.magnitude,
                               row.hard == 1 });
    }
    result = std::move (candidate);
    return true;
}

bool Conflicts (const Input& input, std::vector<HybridConflict>& result, std::string& error)
{
    if (!input.locked)
        return Conflicts (input.raw, result, error);
    std::array<char, 256> buffer {};
    size_t count = 0;
    TapiocaTerrainHybridDiagnosticOutput output { &count, nullptr, 0, 1, 0 };
    auto status = tapioca_terrain_hybrid_locked_assess (&input.fixed, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    std::vector<TapiocaTerrainHybridDiagnostic> rows (count);
    const auto expected = count;
    output = { &count, rows.data (), rows.size (), 0, 0 };
    status = tapioca_terrain_hybrid_locked_assess (&input.fixed, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK || count != expected) {
        error = status != TAPIOCA_OK ? Failure (status, buffer) : "TerrainOutputShape: diagnostic counts disagree";
        return false;
    }
    std::vector<HybridConflict> candidate;
    for (const auto& row : rows) {
        const bool fixed = row.code == 6;
        if (row.code < 1 || row.code > 6 || row.hard > 1 || row.reserved ||
            (fixed ? (row.station != UINT32_MAX || row.side != 0 || row.hard != 1 || row.controlKind == 0)
                   : (row.side < 1 || row.side > 2 || row.station >= input.stations.size () || row.controlKind != 0 ||
                      row.otherKind != 0 || row.vertex != UINT32_MAX))) {
            error = "TerrainOutputShape: unknown fixed-control diagnostic";
            return false;
        }
        HybridConflict value { static_cast<HybridConflictCode> (row.code),
                               row.station,
                               row.side == 2 ? TerrainHand::Right : TerrainHand::Left,
                               { row.position[0], row.position[1], row.position[2] },
                               row.magnitude,
                               row.hard == 1 };
        if (!Control (input, { row.controlKind, row.controlIndex, row.controlSegment }, value.control, error) ||
            !Control (input, { row.otherKind, row.otherIndex, row.otherSegment }, value.other, error))
            return false;
        value.vertex = row.vertex;
        candidate.push_back (std::move (value));
    }
    result = std::move (candidate);
    return true;
}

HybridVolumes Volumes (const TapiocaTerrainHybridVolumes& raw)
{
    return { raw.cut, raw.fill, raw.net, raw.area, raw.triangles };
}

} // namespace

const char* HybridConflictName (HybridConflictCode code)
{
    switch (code) {
        case HybridConflictCode::DaylightNotReached:
            return "DaylightNotReached";
        case HybridConflictCode::DaylightBeyondLimit:
            return "DaylightBeyondLimit";
        case HybridConflictCode::VerticalFace:
            return "VerticalFace";
        case HybridConflictCode::DaylightTopology:
            return "DaylightTopology";
        case HybridConflictCode::EdgeContradictsEffect:
            return "EdgeContradictsEffect";
        case HybridConflictCode::LockedConflict:
            return "LockedConflict";
    }
    return "UnknownConflict";
}

const char* HybridConflictSuggestion (HybridConflictCode code)
{
    switch (code) {
        case HybridConflictCode::DaylightNotReached:
            return "steepen the side slope, widen the limit or add a retaining wall";
        case HybridConflictCode::DaylightBeyondLimit:
            return "raise the side's limit to the reported reach";
        case HybridConflictCode::VerticalFace:
            return "use a separate wall surface or a finite side slope";
        case HybridConflictCode::DaylightTopology:
            return "split the band where the ground changes or adjust the profile so the side stays one kind";
        case HybridConflictCode::EdgeContradictsEffect:
            return "clear the cut/fill restriction or change the elevation";
        case HybridConflictCode::LockedConflict:
            return "narrow the band or align the fixed elevations";
    }
    return "";
}

bool AssessHybridTerrain (const HybridTerrainRequest& request, std::vector<HybridConflict>& conflicts,
                          std::string& error)
{
    Input input;
    std::vector<HybridConflict> candidate;
    if (!Convert (request, input, error) || !Conflicts (input, candidate, error))
        return false;
    conflicts = std::move (candidate);
    error.clear ();
    return true;
}

bool BuildHybridTerrain (const HybridTerrainRequest& request, HybridTerrainResult& result, std::string& error)
{
    Input converted;
    if (!Convert (request, converted, error))
        return false;
    const auto& stations = converted.stations;

    std::array<char, 256> buffer {};
    TapiocaTerrainHybridCounts counts {};
    TapiocaTerrainHybridVolumes site {}, context {};
    TapiocaTerrainHybridOutput output {};
    output.counts = &counts;
    output.site = &site;
    output.context = &context;
    output.queryOnly = 1;
    auto status = Build (converted, output, buffer);
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
    status = Build (converted, output, buffer);
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
        if (mapping.kind != 9 && (mapping.kind < 12 || mapping.kind > 17) &&
            !(converted.locked && mapping.kind >= 20 && mapping.kind <= 22)) {
            error = "TerrainOutputShape: unknown hybrid support kind";
            return false;
        }
        TerrainMapping row { static_cast<TerrainMappingKind> (mapping.kind), mapping.index, {} };
        std::copy (std::begin (mapping.values), std::end (mapping.values), row.values.begin ());
        candidate.mappings.push_back (row);
        if (mapping.kind == 22) {
            std::optional<HybridControlRef> control;
            if (mapping.index >= candidate.base.size () || owners[mapping.index] != kNoOwner ||
                !Control (converted, { mapping.values[0], mapping.values[1], mapping.values[2] }, control, error) ||
                !control) {
                error = "TerrainOutputShape: invalid fixed vertex support";
                return false;
            }
            if (candidate.locked.empty () || candidate.locked.back ().vertex != mapping.index) {
                if (!candidate.locked.empty () && candidate.locked.back ().vertex > mapping.index) {
                    error = "TerrainOutputShape: unordered fixed vertex support";
                    return false;
                }
                candidate.locked.push_back ({ mapping.index, {} });
            }
            candidate.locked.back ().supports.push_back (*control);
        }
    }
    for (const auto& line : lines) {
        const bool sided = line.kind <= 3 || line.kind == 8;
        if (line.kind < 1 || line.kind > 8 || line.side > 2 || sided == (line.side == 0) ||
            (line.kind == 6 && (!converted.locked || line.root >= request.controlLines.size ())) ||
            (line.kind == 7 && (line.root == 0 || size_t (line.root) + 1 >= request.stations.size ()))) {
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
    std::vector<HybridConflict> located;
    if (!Conflicts (converted, located, error))
        return false;
    for (const auto& conflict : located)
        if (!conflict.hard)
            candidate.conflicts.push_back (conflict);
    candidate.inputProvenance = request.provenance;
    candidate.inputPointCount = request.mesh.points.size () / 3;
    candidate.inputRegionCount = request.region.size () / 3;
    size_t at = candidate.inputPointCount + candidate.inputRegionCount;
    for (const auto& line : request.controlLines) {
        candidate.inputControlLineStarts.push_back (at);
        at += line.size () / 3;
    }
    candidate.inputControlPointStart = at;
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
