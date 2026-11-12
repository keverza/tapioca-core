#include "Geometry/GeometryRustUnifiedTerrain.hpp"

#include "GeometryRustFfi.h"
#include "GeometryRustIdentityAbi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
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
                       : "Tapioca Rust unified terrain call failed with status " + std::to_string (status);
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

// Call-scoped POD conversion storage; never copy/move once views are prepared.
struct Input {
    detail::AbiProvenanceStorage provenance;
    std::vector<double> geometry;
    std::vector<TapiocaTerrainRing> rings;
    std::vector<TapiocaUnifiedPart> parts;
    std::vector<uint32_t> partIndices;
    std::vector<TapiocaTerrainStation> stations;
    std::vector<uint32_t> surfaceIndices;
    std::vector<uint64_t> sources;
    std::vector<TapiocaUnifiedEntry> entries;
    std::vector<TapiocaUnifiedPath> paths;
    std::vector<TapiocaUnifiedMask> masks;
    std::vector<TapiocaUnifiedControl> controls;
    std::vector<TapiocaUnifiedOutline> outlines;
    std::vector<TapiocaTerrainRing> outlineLines;
    std::vector<double> outlineGrades;
    TapiocaUnifiedRequest raw {};
};

bool AppendGeometry (std::span<const double> points, size_t minimumVertices, std::vector<double>& geometry,
                     TapiocaTerrainRing& range, std::string& error)
{
    if (points.size () > UINT32_MAX || geometry.size () > UINT32_MAX - points.size () || points.size () % 3 != 0 ||
        points.size () < minimumVertices * 3) {
        error = "TerrainUnifiedGeometry: whole xyz triples and the required vertex count are required";
        return false;
    }
    range = { static_cast<uint32_t> (geometry.size ()), static_cast<uint32_t> (points.size ()) };
    geometry.insert (geometry.end (), points.begin (), points.end ());
    return true;
}

bool AppendRing (const std::vector<double>& ring, std::vector<TapiocaTerrainRing>& rings, std::vector<double>& geometry,
                 std::string& error)
{
    TapiocaTerrainRing range {};
    if (!AppendGeometry (ring, 3, geometry, range, error))
        return false;
    rings.push_back (range);
    return true;
}

bool AppendLine (std::span<const double> line, std::vector<TapiocaTerrainRing>& rings, std::vector<double>& geometry,
                 TapiocaTerrainRing& range, std::string& error)
{
    if (!AppendGeometry (line, 2, geometry, range, error))
        return false;
    rings.push_back (range);
    return true;
}

bool Convert (const UnifiedPlanRequest& request, Input& input, std::string& error)
{
    if (request.entries.size () > UINT32_MAX || request.paths.size () > UINT32_MAX ||
        request.masks.size () > UINT32_MAX || request.controls.size () > UINT32_MAX) {
        error = "TerrainCountOverflow: unified plan exceeds 32-bit ranges";
        return false;
    }
    input.raw.provenance = detail::ConvertProvenanceRequest (request.provenance, input.provenance);
    input.raw.points = { request.mesh.points.data (), request.mesh.points.size () };
    input.raw.triangles = { request.mesh.triangles.data (), request.mesh.triangles.size () };
    input.raw.property = { request.property.data (), request.property.size () };
    // Shared pools: every entry then mask part ring, then every control line.
    auto appendPart = [&] (const UnifiedDomainPart& part, uint32_t& firstRing, uint32_t& ringCount) -> bool {
        firstRing = static_cast<uint32_t> (input.rings.size ());
        ringCount = static_cast<uint32_t> (part.holes.size ()) + 1;
        if (!AppendRing ({ part.outer.begin (), part.outer.end () }, input.rings, input.geometry, error))
            return false;
        for (const auto& hole : part.holes)
            if (!AppendRing ({ hole.begin (), hole.end () }, input.rings, input.geometry, error))
                return false;
        return true;
    };
    input.entries.reserve (request.entries.size ());
    for (const auto& entry : request.entries) {
        if (input.partIndices.size () > UINT32_MAX - entry.parts.size ()) {
            error = "TerrainCountOverflow: part index pool exceeds 32-bit ranges";
            return false;
        }
        TapiocaUnifiedEntry raw {};
        raw.object = entry.object;
        raw.rootOperation = entry.rootOperation;
        raw.level = entry.surface.level;
        raw.falloff = entry.transition.falloff;
        raw.cut = Slope (entry.transition.cut);
        raw.fill = Slope (entry.transition.fill);
        raw.reach = entry.transition.reach;
        if (entry.surface.kind == UnifiedSurfaceKind::Triangulated) {
            if (!AppendGeometry (entry.surface.points, 3, input.geometry, raw.surfacePoints, error))
                return false;
            const auto& triangles = entry.surface.triangles;
            if (triangles.size () > UINT32_MAX || input.surfaceIndices.size () > UINT32_MAX - triangles.size () ||
                triangles.size () % 3 != 0 || triangles.empty ()) {
                error = "TerrainUnifiedSurface: target triangles need whole nonempty triples";
                return false;
            }
            raw.surfaceTriangles = { static_cast<uint32_t> (input.surfaceIndices.size ()),
                                     static_cast<uint32_t> (triangles.size ()) };
            input.surfaceIndices.insert (input.surfaceIndices.end (), triangles.begin (), triangles.end ());
        }
        else if (!entry.surface.points.empty () || !entry.surface.triangles.empty ()) {
            error = "TerrainUnifiedSurface: only a triangulated target may carry a target mesh";
            return false;
        }
        raw.order = entry.order;
        raw.surface = static_cast<uint32_t> (entry.surface.kind);
        raw.effect = static_cast<uint32_t> (entry.effect);
        raw.transition = static_cast<uint32_t> (entry.transition.kind);
        raw.firstPart = static_cast<uint32_t> (input.partIndices.size ());
        raw.partCount = static_cast<uint32_t> (entry.parts.size ());
        raw.firstSource = static_cast<uint32_t> (input.sources.size ());
        raw.sourceCount = static_cast<uint32_t> (entry.sources.size ());
        for (const auto& part : entry.parts) {
            TapiocaUnifiedPart pod {};
            if (!appendPart (part, pod.firstRing, pod.ringCount))
                return false;
            input.partIndices.push_back (static_cast<uint32_t> (input.parts.size ()));
            input.parts.push_back (pod);
        }
        input.sources.insert (input.sources.end (), entry.sources.begin (), entry.sources.end ());
        input.entries.push_back (raw);
    }
    input.masks.reserve (request.masks.size ());
    for (const auto& mask : request.masks) {
        TapiocaUnifiedMask raw {};
        raw.kind = static_cast<uint32_t> (mask.kind);
        raw.operation = mask.operation;
        raw.firstPart = static_cast<uint32_t> (input.partIndices.size ());
        raw.partCount = static_cast<uint32_t> (mask.parts.size ());
        raw.firstSource = static_cast<uint32_t> (input.sources.size ());
        raw.sourceCount = static_cast<uint32_t> (mask.sources.size ());
        for (const auto& part : mask.parts) {
            TapiocaUnifiedPart pod {};
            if (!appendPart (part, pod.firstRing, pod.ringCount))
                return false;
            input.partIndices.push_back (static_cast<uint32_t> (input.parts.size ()));
            input.parts.push_back (pod);
        }
        input.sources.insert (input.sources.end (), mask.sources.begin (), mask.sources.end ());
        input.masks.push_back (raw);
    }
    input.paths.reserve (request.paths.size ());
    for (const auto& path : request.paths) {
        if (path.stations.size () > UINT32_MAX || input.stations.size () > UINT32_MAX - path.stations.size ()) {
            error = "TerrainCountOverflow: station pool exceeds 32-bit ranges";
            return false;
        }
        TapiocaUnifiedPath raw {};
        raw.order = path.order;
        raw.effect = static_cast<uint32_t> (path.effect);
        raw.firstStation = static_cast<uint32_t> (input.stations.size ());
        raw.stationCount = static_cast<uint32_t> (path.stations.size ());
        raw.source = path.source;
        raw.operation = path.operation;
        raw.object = path.object;
        raw.tolerance = path.tolerance;
        std::transform (path.stations.begin (), path.stations.end (), std::back_inserter (input.stations), Station);
        input.paths.push_back (raw);
    }
    input.controls.reserve (request.controls.size ());
    for (const auto& control : request.controls) {
        TapiocaUnifiedControl raw {};
        raw.point = control.point ? 1 : 0;
        raw.source = control.source;
        raw.vertex = control.vertex;
        std::copy (control.pointPosition.begin (), control.pointPosition.end (), raw.position);
        if (control.point) {
            raw.line = { 0, 0 };
        }
        else if (!AppendLine (control.line, input.rings, input.geometry, raw.line, error)) {
            return false;
        }
        input.controls.push_back (raw);
    }
    // #149 T1: the paved outline's pools carry no entity rows (they follow every ring).
    if (request.outlines.size () > 1) {
        error = "BuildOutlineOrder: one paved outline per build, after every region entry";
        return false;
    }
    for (const auto& outline : request.outlines) {
        TapiocaUnifiedOutline raw {};
        raw.cut = Slope (outline.cut);
        raw.fill = Slope (outline.fill);
        raw.limit = outline.limit;
        raw.rounding = outline.rounding;
        raw.tolerance = outline.tolerance;
        raw.order = outline.order;
        raw.effect = static_cast<uint32_t> (outline.effect);
        if (!AppendGeometry (outline.surfacePoints, 3, input.geometry, raw.surfacePoints, error) ||
            !AppendGeometry (outline.spots, 0, input.geometry, raw.spots, error))
            return false;
        const auto& triangles = outline.surfaceTriangles;
        if (triangles.empty () || triangles.size () % 3 != 0 ||
            input.surfaceIndices.size () > UINT32_MAX - triangles.size () || outline.surfaceGrades.size () % 3 != 0 ||
            outline.surfaceGrades.size () > UINT32_MAX) {
            error = "TerrainUnifiedOutline: the paved surface needs whole triples";
            return false;
        }
        raw.surfaceTriangles = { static_cast<uint32_t> (input.surfaceIndices.size ()),
                                 static_cast<uint32_t> (triangles.size ()) };
        input.surfaceIndices.insert (input.surfaceIndices.end (), triangles.begin (), triangles.end ());
        raw.grades = { static_cast<uint32_t> (input.outlineGrades.size ()),
                       static_cast<uint32_t> (outline.surfaceGrades.size ()) };
        input.outlineGrades.insert (input.outlineGrades.end (), outline.surfaceGrades.begin (),
                                    outline.surfaceGrades.end ());
        raw.firstLine = static_cast<uint32_t> (input.outlineLines.size ());
        raw.lineCount = static_cast<uint32_t> (outline.lines.size ());
        for (const auto& line : outline.lines) {
            TapiocaTerrainRing range {};
            if (!AppendGeometry (line, 2, input.geometry, range, error))
                return false;
            input.outlineLines.push_back (range);
        }
        input.outlines.push_back (raw);
    }
    input.raw.outlines = { input.outlines.data (), input.outlines.size () };
    input.raw.outlineLines = { input.outlineLines.data (), input.outlineLines.size () };
    input.raw.outlineGrades = { input.outlineGrades.data (), input.outlineGrades.size () };
    input.raw.geometry = { input.geometry.data (), input.geometry.size () };
    input.raw.rings = { input.rings.data (), input.rings.size () };
    input.raw.parts = { input.parts.data (), input.parts.size () };
    input.raw.partIndices = { input.partIndices.data (), input.partIndices.size () };
    input.raw.stations = { input.stations.data (), input.stations.size () };
    input.raw.surfaceIndices = { input.surfaceIndices.data (), input.surfaceIndices.size () };
    input.raw.sources = { input.sources.data (), input.sources.size () };
    input.raw.entries = { input.entries.data (), input.entries.size () };
    input.raw.paths = { input.paths.data (), input.paths.size () };
    input.raw.masks = { input.masks.data (), input.masks.size () };
    input.raw.controls = { input.controls.data (), input.controls.size () };
    input.raw.controlTolerance = request.controlTolerance;
    input.raw.revision = request.revision;
    input.raw.planHash = request.planHash;
    return true;
}

HybridVolumes Volumes (const TapiocaTerrainHybridVolumes& raw)
{
    return { raw.cut, raw.fill, raw.net, raw.area, raw.triangles };
}

bool DecodeLines (const std::vector<TapiocaUnifiedLine>& rows, std::vector<UnifiedLine>& lines, std::string& error)
{
    lines.reserve (rows.size ());
    for (const auto& row : rows) {
        if (row.domain < 1 || row.domain > 5) {
            error = "TerrainOutputShape: unknown unified line domain";
            return false;
        }
        UnifiedLine line {};
        line.domain = static_cast<UnifiedLineDomain> (row.domain);
        line.index = row.index;
        line.part = row.part;
        line.ring = row.ring;
        line.segment = row.segment;
        line.root = row.root;
        if (row.domain == 2) {
            const bool sided = row.kind <= 3 || row.kind == 8;
            if (row.kind < 1 || row.kind > 8 || row.kind == 6 || row.side > 2 || sided == (row.side == 0)) {
                error = "TerrainOutputShape: unknown unified path line kind";
                return false;
            }
            line.kind = static_cast<HybridLineKind> (row.kind);
            if (row.side != 0)
                line.side = static_cast<TerrainHand> (row.side);
        }
        else if (row.kind != 0 || row.side != 0 || row.root != 0) {
            error = "TerrainOutputShape: path fields on a non-path line";
            return false;
        }
        lines.push_back (line);
    }
    return true;
}

// #149 T4: every row is checked before any is accepted; the kind fixes which index it may carry.
bool DecodeSteps (const std::vector<TapiocaUnifiedStep>& rows, size_t vertices, size_t ringSegments,
                  std::vector<UnifiedStepWitness>& steps, std::string& error)
{
    std::array<size_t, 4> perKind {};
    steps.reserve (rows.size ());
    for (const auto& row : rows) {
        const bool finite = std::isfinite (row.position[0]) && std::isfinite (row.position[1]) &&
                            std::isfinite (row.position[2]) && std::isfinite (row.design) &&
                            std::isfinite (row.residual);
        const bool indexed = (row.kind == 1 && row.index < vertices) || (row.kind == 2 && row.index < ringSegments) ||
                             (row.kind == 3 && row.index == UINT32_MAX);
        if (row.kind < 1 || row.kind > 3 || !finite || !indexed || ++perKind[row.kind] > kOutlineStepWitnessLimit) {
            error = "TerrainOutputShape: invalid unified step witness";
            return false;
        }
        UnifiedStepWitness step;
        step.kind = static_cast<UnifiedStepKind> (row.kind);
        if (row.kind != 3)
            step.index = row.index;
        step.position = { row.position[0], row.position[1], row.position[2] };
        step.design = row.design;
        step.residual = row.residual;
        steps.push_back (step);
    }
    return true;
}

} // namespace

bool BuildUnifiedTerrain (const UnifiedPlanRequest& request, UnifiedTerrainResult& result, std::string& error)
{
    Input converted;
    if (!Convert (request, converted, error))
        return false;

    std::array<char, 256> buffer {};
    TapiocaUnifiedCounts counts {};
    TapiocaUnifiedMeta meta {};
    TapiocaTerrainHybridVolumes site {}, context {};
    TapiocaUnifiedOutput output {};
    output.counts = &counts;
    output.meta = &meta;
    output.site = &site;
    output.context = &context;
    output.queryOnly = 1;
    auto status = tapioca_terrain_unified_build (&converted.raw, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    UnifiedTerrainResult candidate;
    std::vector<TapiocaTerrainMapping> mappings;
    std::vector<TapiocaUnifiedLine> lines;
    std::vector<TapiocaUnifiedLocked> locked;
    std::vector<TapiocaUnifiedStep> steps;
    std::vector<uint32_t> owners;
    if (counts.points % 3 != 0 || counts.triangles % 3 != 0 || counts.points > candidate.points.max_size () ||
        counts.triangles > candidate.triangles.max_size () || counts.mappings > mappings.max_size () ||
        counts.lines > lines.max_size () || counts.locked > locked.max_size () ||
        counts.steps > 3 * kOutlineStepWitnessLimit) {
        error = "TerrainOutputShape: invalid unified query counts";
        return false;
    }
    const auto expected = counts;
    candidate.points.resize (counts.points);
    candidate.base.resize (counts.points / 3);
    owners.resize (counts.points / 3);
    candidate.triangles.resize (counts.triangles);
    mappings.resize (counts.mappings);
    lines.resize (counts.lines);
    locked.resize (counts.locked);
    steps.resize (counts.steps);
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
    output.locked = locked.data ();
    output.lockedCapacity = locked.size ();
    output.steps = steps.data ();
    output.stepCapacity = steps.size ();
    output.queryOnly = 0;
    status = tapioca_terrain_unified_build (&converted.raw, &output, buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = Failure (status, buffer);
        return false;
    }
    if (counts.points != expected.points || counts.triangles != expected.triangles ||
        counts.mappings != expected.mappings || counts.lines != expected.lines || counts.locked != expected.locked ||
        counts.steps != expected.steps) {
        error = "TerrainOutputShape: unified query/fill counts disagree";
        return false;
    }
    for (const auto& mapping : mappings) {
        if (mapping.kind != 9 && (mapping.kind < 12 || mapping.kind > 17) && mapping.kind != 20 && mapping.kind != 21 &&
            mapping.kind != 23) {
            error = "TerrainOutputShape: unknown unified support kind";
            return false;
        }
        TerrainMapping row { static_cast<TerrainMappingKind> (mapping.kind), mapping.index, {} };
        std::copy (std::begin (mapping.values), std::end (mapping.values), row.values.begin ());
        candidate.mappings.push_back (row);
    }
    if (!DecodeLines (lines, candidate.lines, error))
        return false;
    const size_t ringSegments = static_cast<size_t> (
        std::count_if (mappings.begin (), mappings.end (), [] (const auto& row) { return row.kind == 17; }));
    if (!DecodeSteps (steps, counts.points / 3, ringSegments, candidate.outlineSteps, error))
        return false;
    candidate.locked.reserve (locked.size ());
    for (const auto& row : locked)
        candidate.locked.push_back ({ row.control, row.vertex, row.height });
    candidate.owners.reserve (owners.size ());
    for (uint32_t owner : owners)
        candidate.owners.push_back (owner == kNoOwner ? std::nullopt : std::optional<uint32_t> (owner));
    candidate.site = Volumes (site);
    candidate.context = Volumes (context);
    candidate.revision = meta.revision;
    candidate.planHash = meta.planHash;
    candidate.outlineUnreached = meta.outlineUnreached;
    candidate.outlineClipped = meta.outlineClipped;
    candidate.inputProvenance = request.provenance;
    candidate.inputPointCount = request.mesh.points.size () / 3;
    candidate.inputPropertyCount = request.property.size () / 3;
    result = std::move (candidate);
    error.clear ();
    return true;
}

bool PreviewUnifiedTerrain (const UnifiedPlanRequest& request, const UnifiedPreviewAsk& ask,
                            UnifiedPreviewResult& result, std::string& error)
{
    Input converted;
    if (!Convert (request, converted, error))
        return false;
    std::array<char, 256> buffer {};
    TapiocaPreviewCounts counts {};
    TapiocaPreviewSummary summary {};
    std::vector<double> points, linePoints;
    std::vector<uint32_t> triangles;
    std::vector<TapiocaPreviewLine> lines;
    TapiocaPreviewOutput output {};
    output.step = ask.step;
    output.interval = ask.interval;
    output.maxReach = ask.maxReach;
    output.counts = &counts;
    output.summary = &summary;
    size_t want[4] = { ask.points, ask.triangles, ask.lines, ask.linePoints };
    // The first call tries the capacities asked; a second, only when one was too small, the
    // exact counts the first reported.
    for (int attempt = 0; attempt < 2; ++attempt) {
        points.resize (want[0]);
        triangles.resize (want[1]);
        lines.resize (want[2]);
        linePoints.resize (want[3]);
        output.points = points.data ();
        output.pointCapacity = points.size ();
        output.triangles = triangles.data ();
        output.triangleCapacity = triangles.size ();
        output.lines = lines.data ();
        output.lineCapacity = lines.size ();
        output.linePoints = linePoints.data ();
        output.linePointCapacity = linePoints.size ();
        const auto status = tapioca_terrain_unified_preview (&converted.raw, &output, buffer.data (), buffer.size ());
        if (status != TAPIOCA_OK) {
            error = Failure (status, buffer);
            return false;
        }
        if (counts.complete == 1)
            break;
        want[0] = counts.points;
        want[1] = counts.triangles;
        want[2] = counts.lines;
        want[3] = counts.linePoints;
    }
    if (counts.complete != 1 || counts.points % 3 != 0 || counts.triangles % 3 != 0 || counts.linePoints % 3 != 0) {
        error = "TerrainOutputShape: invalid field preview counts";
        return false;
    }
    UnifiedPreviewResult candidate;
    points.resize (counts.points);
    triangles.resize (counts.triangles);
    candidate.points = std::move (points);
    candidate.triangles = std::move (triangles);
    for (size_t i = 0; i < counts.lines; ++i) {
        const auto& row = lines[i];
        if (row.first > counts.linePoints / 3 || row.count > counts.linePoints / 3 - row.first) {
            error = "TerrainOutputShape: field preview line outside its pool";
            return false;
        }
        UnifiedPreviewLine line;
        line.level = row.level;
        line.closed = row.closed != 0;
        line.points.assign (linePoints.begin () + ptrdiff_t (row.first * 3),
                            linePoints.begin () + ptrdiff_t ((row.first + row.count) * 3));
        candidate.lines.push_back (std::move (line));
    }
    candidate.interval = summary.interval;
    candidate.step = summary.step;
    candidate.surfaceStep = summary.surfaceStep;
    candidate.cut = summary.cut;
    candidate.fill = summary.fill;
    candidate.low = summary.range[0];
    candidate.high = summary.range[1];
    candidate.reach = summary.reach;
    candidate.unreached = summary.unreached;
    candidate.clipped = summary.clipped;
    result = std::move (candidate);
    error.clear ();
    return true;
}

} // namespace tapioca::geometry
