#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscapeInternal.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace tapioca::geometry {
namespace {
namespace edit = geomsrv::edit;

// FNV-1a over a canonical byte feed: integers and IEEE doubles by bit pattern,
// strings by content, containers in plan order. Plan-level determinism only;
// the worker compares hashes, never re-serializes geometry.
void HashBytes (uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*> (data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}
void HashNumber (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    static_assert (sizeof bits == sizeof value);
    std::memcpy (&bits, &value, sizeof bits);
    HashBytes (hash, &bits, sizeof bits);
}
void HashText (uint64_t& hash, const std::string& text)
{
    HashBytes (hash, text.data (), text.size ());
    const uint8_t zero = 0;
    HashBytes (hash, &zero, 1); // "a","bc" never collides with "ab","c".
}
void HashIdVector (uint64_t& hash, const std::vector<Id>& ids)
{
    const uint64_t count = ids.size ();
    HashBytes (hash, &count, sizeof count);
    for (Id id : ids)
        HashBytes (hash, &id, sizeof id);
}
void HashPoints (uint64_t& hash, const std::vector<Vec3>& points)
{
    const uint64_t count = points.size ();
    HashBytes (hash, &count, sizeof count);
    for (const auto& p : points) {
        HashNumber (hash, p.x);
        HashNumber (hash, p.y);
        HashNumber (hash, p.z);
    }
}
void HashDoubleVector (uint64_t& hash, const std::vector<double>& values)
{
    const uint64_t count = values.size ();
    HashBytes (hash, &count, sizeof count);
    for (double value : values)
        HashNumber (hash, value);
}
void HashU32Vector (uint64_t& hash, const std::vector<uint32_t>& values)
{
    const uint64_t count = values.size ();
    HashBytes (hash, &count, sizeof count);
    for (uint32_t value : values)
        HashBytes (hash, &value, sizeof value);
}
void HashMaterial (uint64_t& hash, const MaterialSpec& material)
{
    HashBytes (hash, &material.preset, sizeof material.preset);
    HashText (hash, material.fillGuid);
    HashText (hash, material.fillName);
    HashText (hash, material.surfaceGuid);
    HashText (hash, material.surfaceName);
    HashNumber (hash, material.hatchDirection);
    HashNumber (hash, material.hatchDensity);
}
void HashSlope (uint64_t& hash, const TerrainSlope& slope)
{
    HashBytes (hash, &slope.unit, sizeof slope.unit);
    HashNumber (hash, slope.a);
    HashNumber (hash, slope.b);
}
void HashRings (uint64_t& hash, const std::vector<std::vector<Vec3>>& rings)
{
    const uint64_t count = rings.size ();
    HashBytes (hash, &count, sizeof count);
    for (const auto& ring : rings) {
        const uint64_t points = ring.size ();
        HashBytes (hash, &points, sizeof points);
        for (const auto& p : ring) {
            HashNumber (hash, p.x);
            HashNumber (hash, p.y);
            HashNumber (hash, p.z);
        }
    }
}
void HashParts (uint64_t& hash, const std::vector<FieldPlanPart>& parts)
{
    const uint64_t count = parts.size ();
    HashBytes (hash, &count, sizeof count);
    for (const auto& part : parts)
        HashRings (hash, part.rings);
}
void HashSurface (uint64_t& hash, const RegionSurface& surface)
{
    HashBytes (hash, &surface.elevation, sizeof surface.elevation);
    HashBytes (hash, &surface.slope, sizeof surface.slope);
    HashNumber (hash, surface.first.x);
    HashNumber (hash, surface.first.y);
    HashNumber (hash, surface.first.z);
    HashNumber (hash, surface.second.x);
    HashNumber (hash, surface.second.y);
    HashNumber (hash, surface.second.z);
    HashSlope (hash, surface.grade);
    HashBytes (hash, &surface.fallsTo, sizeof surface.fallsTo);
}
void HashTerrain (uint64_t& hash, const RegionTerrain& terrain)
{
    HashBytes (hash, &terrain.effect, sizeof terrain.effect);
    HashBytes (hash, &terrain.transition, sizeof terrain.transition);
    HashNumber (hash, terrain.falloff);
    HashNumber (hash, terrain.reach);
    HashSlope (hash, terrain.cut);
    HashSlope (hash, terrain.fill);
}
uint64_t FieldPlanHash (const FieldPlan& plan)
{
    uint64_t hash = 14695981039346656037ull;
    HashBytes (hash, &plan.version, sizeof plan.version);
    HashBytes (hash, &plan.revision, sizeof plan.revision);
    HashBytes (hash, &plan.captureRevision, sizeof plan.captureRevision);
    HashBytes (hash, plan.session.data (), plan.session.size ());
    HashText (hash, plan.sourceTerrainGuid);
    HashPoints (hash, plan.propertyLine);
    HashBytes (hash, &plan.propertyLineRevision, sizeof plan.propertyLineRevision);
    HashBytes (hash, &plan.propertyLineSource, sizeof plan.propertyLineSource);
    for (const auto& entry : plan.entries) {
        HashBytes (hash, &entry.order, sizeof entry.order);
        HashBytes (hash, &entry.priority, sizeof entry.priority);
        HashBytes (hash, &entry.object, sizeof entry.object);
        HashBytes (hash, &entry.rootOperation, sizeof entry.rootOperation);
        HashBytes (hash, &entry.kind, sizeof entry.kind);
        HashText (hash, entry.name);
        HashMaterial (hash, entry.material);
        HashNumber (hash, entry.thickness);
        HashIdVector (hash, entry.sources);
        HashParts (hash, entry.parts);
        const uint8_t hasSurface = entry.surface ? 1 : 0;
        HashBytes (hash, &hasSurface, 1);
        if (entry.surface)
            HashSurface (hash, *entry.surface);
        HashDoubleVector (hash, entry.surfacePoints);
        HashU32Vector (hash, entry.surfaceTriangles);
        HashTerrain (hash, entry.terrain);
    }
    for (const auto& path : plan.paths) {
        HashBytes (hash, &path.order, sizeof path.order);
        HashBytes (hash, &path.source, sizeof path.source);
        HashBytes (hash, &path.operation, sizeof path.operation);
        HashBytes (hash, &path.object, sizeof path.object);
        HashBytes (hash, &path.path, sizeof path.path);
        HashNumber (hash, path.design.maxStep);
        HashNumber (hash, path.design.taper);
        HashNumber (hash, path.design.margin);
        HashNumber (hash, path.design.arcTolerance);
        HashNumber (hash, path.design.cutLimit);
        HashNumber (hash, path.design.fillLimit);
        HashBytes (hash, &path.design.effect, sizeof path.design.effect);
        HashNumber (hash, path.design.tolerance);
        const uint8_t outline = path.design.outline ? 1 : 0;
        HashBytes (hash, &outline, 1);
        const uint64_t count = path.stations.size ();
        HashBytes (hash, &count, sizeof count);
        for (const auto& station : path.stations) {
            HashNumber (hash, station.station);
            HashNumber (hash, station.centre.x);
            HashNumber (hash, station.centre.y);
            HashNumber (hash, station.centre.z);
            HashNumber (hash, station.grade);
            HashNumber (hash, station.leftEdge.x);
            HashNumber (hash, station.leftEdge.y);
            HashNumber (hash, station.leftEdge.z);
            HashNumber (hash, station.rightEdge.x);
            HashNumber (hash, station.rightEdge.y);
            HashNumber (hash, station.rightEdge.z);
            HashNumber (hash, station.section.widthLeft);
            HashNumber (hash, station.section.widthRight);
            HashNumber (hash, station.section.endLeft);
            HashNumber (hash, station.section.endRight);
            const uint8_t taper = station.section.taper ? 1 : 0;
            HashBytes (hash, &taper, 1);
            HashSlope (hash, station.section.crossfall.slope);
            const uint8_t negative = station.section.crossfall.negative ? 1 : 0;
            HashBytes (hash, &negative, 1);
            HashBytes (hash, &station.section.fall, sizeof station.section.fall);
            HashNumber (hash, station.section.shoulder);
            HashSlope (hash, station.section.cutSlope);
            HashSlope (hash, station.section.fillSlope);
            HashNumber (hash, station.section.rounding);
        }
        HashDoubleVector (hash, path.crossfallPercent);
    }
    for (const auto& mask : plan.masks) {
        HashBytes (hash, &mask.operation, sizeof mask.operation);
        HashBytes (hash, &mask.kind, sizeof mask.kind);
        HashIdVector (hash, mask.sources);
        HashParts (hash, mask.parts);
    }
    for (const auto& control : plan.controls) {
        const uint8_t point = control.point ? 1 : 0;
        HashBytes (hash, &point, 1);
        HashBytes (hash, &control.source, sizeof control.source);
        HashBytes (hash, &control.vertex, sizeof control.vertex);
        HashNumber (hash, control.pointPosition.x);
        HashNumber (hash, control.pointPosition.y);
        HashNumber (hash, control.pointPosition.z);
        const uint64_t count = control.line.size ();
        HashBytes (hash, &count, sizeof count);
        for (const auto& p : control.line) {
            HashNumber (hash, p.x);
            HashNumber (hash, p.y);
            HashNumber (hash, p.z);
        }
        const uint64_t segments = control.segments.size ();
        HashBytes (hash, &segments, sizeof segments);
        for (const auto& segment : control.segments) {
            HashBytes (hash, &segment.source, sizeof segment.source);
            HashBytes (hash, &segment.segment, sizeof segment.segment);
            HashBytes (hash, segment.vertices.data (), sizeof segment.vertices);
        }
    }
    for (const auto& diagnostic : plan.diagnostics) {
        HashBytes (hash, &diagnostic.stage, sizeof diagnostic.stage);
        HashBytes (hash, &diagnostic.code, sizeof diagnostic.code);
        HashBytes (hash, &diagnostic.object, sizeof diagnostic.object);
        HashBytes (hash, &diagnostic.operation, sizeof diagnostic.operation);
        HashBytes (hash, &diagnostic.source, sizeof diagnostic.source);
        HashBytes (hash, &diagnostic.ring, sizeof diagnostic.ring);
        HashBytes (hash, &diagnostic.part, sizeof diagnostic.part);
        HashText (hash, diagnostic.message);
    }
    return hash;
}

void Diagnose (FieldPlan& plan, FieldPlanStage stage, FieldPlanDiagnosticCode code, Id object, Id operation, Id source,
               const std::string& message)
{
    FieldPlanDiagnostic diagnostic;
    diagnostic.stage = stage;
    diagnostic.code = code;
    diagnostic.object = object;
    diagnostic.operation = operation;
    diagnostic.source = source;
    diagnostic.message = message;
    plan.diagnostics.push_back (std::move (diagnostic));
}

// Locked controls resolve from the document's authored refs alone, mirroring
// the captured CompileTerrainControls contract without a capture catalog:
// the plan carries references and positions, never provenance rows.
void CompileControls (const edit::Document& doc, FieldPlan& plan)
{
    for (const auto& control : doc.landscape->terrainControls) {
        if (!control.enabled)
            continue;
        const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                          [&] (const auto& s) { return s.id == control.source && !s.deleted; });
        const auto* entity = source == doc.landscape->sources.end () ? nullptr : edit::Find (doc, source->entityId);
        if (!entity || entity->kind != edit::Kind::Polyline) {
            Diagnose (plan, FieldPlanStage::Constraint, FieldPlanDiagnosticCode::ControlOrphaned, kNoId, kNoId,
                      control.source, "FieldPlanControl: an enabled control has no live guide");
            continue;
        }
        if (control.vertex) {
            const auto vertex = std::find_if (entity->vertices.begin (), entity->vertices.end (),
                                              [&] (const auto& v) { return v.id == control.vertex; });
            if (vertex == entity->vertices.end ()) {
                Diagnose (plan, FieldPlanStage::Constraint, FieldPlanDiagnosticCode::ControlOrphaned, kNoId, kNoId,
                          control.source, "FieldPlanControl: an enabled fixed vertex was deleted");
                continue;
            }
            FieldPlanControl resolved;
            resolved.point = true;
            resolved.source = control.source;
            resolved.vertex = control.vertex;
            resolved.pointPosition = vertex->p;
            plan.controls.push_back (std::move (resolved));
            continue;
        }
        if (entity->closed || entity->vertices.size () < 2 ||
            std::any_of (
                entity->vertices.begin (), entity->vertices.end (),
                [] (const auto& v) { return v.radius != 0 || v.in != edit::Vec3 {} || v.out != edit::Vec3 {}; }) ||
            std::any_of (source->arcAngles.begin (), source->arcAngles.end (), [] (double a) { return a != 0; })) {
            Diagnose (plan, FieldPlanStage::Constraint, FieldPlanDiagnosticCode::ControlUnsupported, kNoId, kNoId,
                      control.source, "FieldPlanControl: fixed lines require an open straight polyline");
            continue;
        }
        FieldPlanControl resolved;
        resolved.point = false;
        resolved.source = control.source;
        for (const auto& vertex : entity->vertices)
            resolved.line.push_back (vertex.p);
        for (size_t i = 0; i + 1 < entity->vertices.size (); ++i)
            resolved.segments.push_back (
                { control.source, entity->segments.at (i).id, { entity->vertices[i].id, entity->vertices[i + 1].id } });
        plan.controls.push_back (std::move (resolved));
    }
}

void CompilePath (const edit::Document& doc, const PathDesignRequest& request, FieldPlan& plan)
{
    const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                      [&] (const auto& s) { return s.id == request.source && !s.deleted; });
    const auto* path = source == doc.landscape->sources.end () || source->role != SourceRole::PathCenterline
                           ? nullptr
                           : edit::FindPath (doc, source->pathId);
    if (!path) {
        Diagnose (plan, FieldPlanStage::Path, FieldPlanDiagnosticCode::PathUnresolved, request.object,
                  request.operation, request.source, "FieldPlanPath: the design request names no live path centerline");
        return;
    }
    edit::AlignmentGeometry geometry;
    edit::PathPreparation preparation;
    std::string error;
    if (!edit::ValidatePath (*path, geometry, error) || !edit::PreparePath (*path, geometry, preparation, error)) {
        Diagnose (plan, FieldPlanStage::Path, FieldPlanDiagnosticCode::PathUnresolved, request.object,
                  request.operation, request.source, "FieldPlanPath: " + error);
        return;
    }
    FieldPlanPath compiled;
    compiled.order = uint32_t (plan.entries.size ());
    compiled.source = request.source;
    compiled.operation = request.operation;
    compiled.object = request.object;
    compiled.path = path->id;
    compiled.design = request;
    auto samples = edit::SampleStations (*path, geometry, request.maxStep);
    for (size_t i = 1; i < path->sections.size (); ++i) {
        const double before = path->sections[i].station - request.taper;
        if (before > 0)
            samples.push_back (before);
    }
    std::sort (samples.begin (), samples.end ());
    samples.erase (std::unique (samples.begin (), samples.end (), [] (double a, double b) { return b - a < 1.0e-6; }),
                   samples.end ());
    for (const double station : samples) {
        edit::StationFrame frame;
        if (!edit::Evaluate (*path, geometry, preparation, station, frame)) {
            Diagnose (plan, FieldPlanStage::Path, FieldPlanDiagnosticCode::PathUnresolved, request.object,
                      request.operation, request.source, "FieldPlanPath: station preparation failed");
            return;
        }
        double percent = 0;
        if (!edit::PreparedCrossfall (preparation, frame.section, percent)) {
            Diagnose (plan, FieldPlanStage::Path, FieldPlanDiagnosticCode::PathUnresolved, request.object,
                      request.operation, request.source, "FieldPlanPath: prepared crossfall could not be resolved");
            return;
        }
        compiled.crossfallPercent.push_back (percent);
        compiled.stations.push_back (std::move (frame));
    }
    plan.paths.push_back (std::move (compiled));
}
} // namespace

double ComposeFieldHeight (RegionTerrainEffect effect, double h, double t)
{
    switch (effect) {
        case RegionTerrainEffect::Grade:
            return t;
        case RegionTerrainEffect::Add:
            return std::max (h, t);
        case RegionTerrainEffect::Subtract:
            return std::min (h, t);
        case RegionTerrainEffect::None:
            break;
    }
    return h;
}

FieldPlan CompileLandscapeFieldPlan (const LandscapeSnapshot& snapshot)
{
    FieldPlan plan;
    plan.revision = snapshot.revision;
    plan.captureRevision = snapshot.captureRevision;
    plan.session = snapshot.session;
    plan.sourceTerrainGuid = snapshot.sourceTerrainGuid;
    const auto refuse = [&] (FieldPlanDiagnosticCode code, const std::string& message) {
        Diagnose (plan, FieldPlanStage::Snapshot, code, kNoId, kNoId, kNoId, message);
        plan.error = message;
        return plan;
    };
    if (!snapshot.doc || !snapshot.doc->landscape)
        return refuse (FieldPlanDiagnosticCode::SnapshotMismatch,
                       "FieldPlanSnapshot: an immutable landscape document snapshot is required");
    const auto& doc = *snapshot.doc;
    const auto& session = *doc.landscape;
    std::vector<Id> order;
    std::string error;
    if (!ValidateLandscapeDag (session, order, error))
        return refuse (FieldPlanDiagnosticCode::DagInvalid, "FieldPlanDag: " + error);
    // Stage 1 through the existing authority: evaluated domains per operation
    // (with PolyTree nesting intact) and the terrain constraint masks. Pure
    // evaluation; nothing is stored back into the session.
    LandscapeResult evaluated;
    std::map<Id, landscape_detail::Shape> values;
    if (!landscape_detail::EvaluateLandscapeOperations (doc, evaluated, &values))
        return refuse (FieldPlanDiagnosticCode::EvaluationRefused,
                       "FieldPlanDomain: " +
                           (evaluated.error.empty () ? "landscape evaluation refused" : evaluated.error));
    plan.propertyLine = evaluated.propertyLine;
    plan.propertyLineRevision = evaluated.propertyLineRevision;
    plan.propertyLineSource = session.propertyLineSourceId;
    std::map<Id, size_t> topoIndex;
    for (size_t i = 0; i < order.size (); ++i)
        topoIndex.emplace (order[i], i);
    const auto operation = [&] (Id id) -> const LandscapeOperation* {
        const auto found = std::find_if (session.operations.begin (), session.operations.end (),
                                         [=] (const auto& op) { return op.id == id; });
        return found == session.operations.end () ? nullptr : &*found;
    };
    // Every active owner contributes exactly one entry. Objects with no
    // configured effect are not field-stack members; a configured effect on a
    // dead root is a located diagnostic, not a silently skipped owner.
    std::vector<FieldPlanEntry> entries;
    for (const auto& object : session.objects) {
        if (object.terrain.effect == RegionTerrainEffect::None)
            continue;
        const auto* root = operation (object.footprintOperation);
        if (!root) {
            Diagnose (plan, FieldPlanStage::Domain, FieldPlanDiagnosticCode::OwnerRootMissing, object.id,
                      object.footprintOperation, kNoId,
                      "FieldPlanDomain: an active owner cites no live root operation");
            continue;
        }
        FieldPlanEntry entry;
        entry.priority = root->priority;
        entry.object = object.id;
        entry.rootOperation = object.footprintOperation;
        entry.kind = object.kind;
        entry.name = object.name;
        entry.material = object.baseMaterial;
        entry.thickness = object.thickness;
        entry.surface = object.surface;
        entry.terrain = object.terrain;
        for (const auto& region : evaluated.regions) {
            if (region.objectId != object.id)
                continue;
            entry.parts.push_back ({ region.rings });
            for (const Id source : region.sources)
                if (std::find (entry.sources.begin (), entry.sources.end (), source) == entry.sources.end ())
                    entry.sources.push_back (source);
        }
        if (object.surface) {
            PreparedRegionSurface prepared;
            std::string surfaceError;
            if (!PrepareRegionSurface (*object.surface, prepared, surfaceError))
                Diagnose (plan, FieldPlanStage::Surface, FieldPlanDiagnosticCode::SurfacePrepareFailed, object.id,
                          object.footprintOperation, kNoId, "FieldPlanSurface: " + surfaceError);
            if (object.surface->elevation == RegionElevationMode::Boundary ||
                object.surface->slope != RegionSlopeMode::None) {
                for (const auto& region : evaluated.regions) {
                    if (region.objectId != object.id)
                        continue;
                    LandscapePreviewMesh target;
                    if (!BuildRegionSurfaceMesh (doc, object, region, target, surfaceError)) {
                        Diagnose (plan, FieldPlanStage::Surface, FieldPlanDiagnosticCode::SurfacePrepareFailed,
                                  object.id, object.footprintOperation, kNoId,
                                  "FieldPlanSurfaceTarget: " + surfaceError);
                        break;
                    }
                    const auto offset = uint32_t (entry.surfacePoints.size () / 3);
                    for (const auto& point : target.vertices)
                        entry.surfacePoints.insert (entry.surfacePoints.end (), { point.x, point.y, point.z });
                    for (const auto triangle : target.triangles)
                        entry.surfaceTriangles.push_back (triangle + offset);
                }
            }
        }
        entries.push_back (std::move (entry));
    }
    std::stable_sort (entries.begin (), entries.end (), [&] (const FieldPlanEntry& a, const FieldPlanEntry& b) {
        if (a.priority != b.priority)
            return a.priority < b.priority;
        constexpr auto kNoTopo = std::numeric_limits<size_t>::max ();
        const auto indexA = topoIndex.count (a.rootOperation) ? topoIndex.at (a.rootOperation) : kNoTopo;
        const auto indexB = topoIndex.count (b.rootOperation) ? topoIndex.at (b.rootOperation) : kNoTopo;
        if (indexA != indexB)
            return indexA < indexB;
        return a.object < b.object; // Deterministic tie-break, never array order.
    });
    for (size_t i = 0; i < entries.size (); ++i)
        entries[i].order = uint32_t (i);
    plan.entries = std::move (entries);
    for (const auto& constraint : evaluated.terrainConstraints) {
        FieldPlanMask mask;
        mask.operation = constraint.operation;
        mask.kind = constraint.kind;
        for (const auto& region : constraint.masks) {
            mask.parts.push_back ({ region.rings });
            for (const Id source : region.sources)
                if (std::find (mask.sources.begin (), mask.sources.end (), source) == mask.sources.end ())
                    mask.sources.push_back (source);
        }
        plan.masks.push_back (std::move (mask));
    }
    CompileControls (doc, plan);
    if (snapshot.path)
        CompilePath (doc, *snapshot.path, plan);
    plan.hash = FieldPlanHash (plan);
    plan.ok = true;
    return plan;
}

bool ValidateLandscapeFieldPlanForBuild (const FieldPlan& plan, std::string& error)
{
    if (!plan.ok || plan.version != kFieldPlanVersion || plan.hash == 0) {
        error = "FieldPlanIncomplete: a current compiled plan with a content hash is required";
        return false;
    }
    if (!plan.diagnostics.empty ()) {
        const auto& diagnostic = plan.diagnostics.front ();
        error = "FieldPlanIncomplete: " + diagnostic.message;
        return false;
    }
    for (const auto& entry : plan.entries) {
        if (!entry.surface || entry.parts.empty ()) {
            error = "FieldPlanIncomplete: an active region has no surface target or evaluated domain";
            return false;
        }
        if (std::any_of (entry.parts.begin (), entry.parts.end (), [] (const FieldPlanPart& part) {
                return part.rings.empty () || part.rings.front ().size () < 3;
            })) {
            error = "FieldPlanIncomplete: an active region has an empty evaluated outer ring";
            return false;
        }
        const bool triangulated =
            entry.surface->elevation == RegionElevationMode::Boundary || entry.surface->slope != RegionSlopeMode::None;
        if (triangulated && (entry.surfacePoints.empty () || entry.surfaceTriangles.empty ())) {
            error = "FieldPlanIncomplete: a non-constant region has no triangulated target";
            return false;
        }
        if (!triangulated && (!entry.surfacePoints.empty () || !entry.surfaceTriangles.empty ())) {
            error = "FieldPlanIncomplete: a constant/existing target unexpectedly carries a target mesh";
            return false;
        }
    }
    for (const auto& path : plan.paths)
        if (path.stations.size () < 2 || path.crossfallPercent.size () != path.stations.size ()) {
            error = "FieldPlanIncomplete: a path has incomplete station preparation";
            return false;
        }
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
