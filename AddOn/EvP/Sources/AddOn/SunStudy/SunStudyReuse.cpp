#include "SunStudy/SunStudyReuse.hpp"

#include "SunStudy/SunStudyRoles.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>

namespace evp::sunstudy {
namespace {

using MeshMap = std::map<std::string, size_t>;

bool IndexMeshes (const geomsrv::Snapshot& snapshot, MeshMap& index)
{
    for (size_t mesh = 0; mesh < snapshot.meshes.size (); ++mesh) {
        if (snapshot.meshes[mesh].vertices.size () % 3 != 0)
            return false;
        for (const double coordinate : snapshot.meshes[mesh].vertices) {
            if (!std::isfinite (coordinate))
                return false;
        }
        const auto guid = CanonicalGuid (snapshot.meshes[mesh].guid);
        if (guid.empty () || !index.emplace (guid, mesh).second)
            return false; // ambiguous identity: recompute, never guess
    }
    return true;
}

ChangedBounds Bounds (const geomsrv::Mesh& mesh)
{
    ChangedBounds bounds;
    for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3)
        bounds.Add (&mesh.vertices[v]);
    return bounds;
}

bool SameMesh (const geomsrv::Mesh& a, const geomsrv::Mesh& b)
{
    return a.vertices == b.vertices && a.triangles == b.triangles;
}

bool SameSun (const SunSeries& a, const SunSeries& b)
{
    if (a.Version () != b.Version () || a.StepCount () != b.StepCount () ||
        a.TimestepMinutes () != b.TimestepMinutes ())
        return false;
    for (size_t i = 0; i < a.StepCount (); ++i) {
        const auto& x = a.Step (i);
        const auto& y = b.Step (i);
        if (x.time.hour != y.time.hour || x.time.minute != y.time.minute ||
            !std::equal (x.direction, x.direction + 3, y.direction))
            return false;
    }
    return true;
}

struct ShadowZone {
    ChangedBounds bounds;
    ChangedBounds envelope;
};

bool RayHitsBounds (const double* point, const double* direction, const ChangedBounds& bounds)
{
    double entry = 0.0, exit = std::numeric_limits<double>::infinity ();
    for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0) {
            if (point[axis] < bounds.min[axis] || point[axis] > bounds.max[axis])
                return false;
            continue;
        }
        double nearDistance = (bounds.min[axis] - point[axis]) / direction[axis];
        double farDistance = (bounds.max[axis] - point[axis]) / direction[axis];
        if (nearDistance > farDistance)
            std::swap (nearDistance, farDistance);
        entry = std::max (entry, nearDistance);
        exit = std::min (exit, farDistance);
        if (entry > exit)
            return false;
    }
    return true;
}

bool InShadowZone (const double* point, const std::vector<ShadowZone>& zones, const SunSeries& sun)
{
    for (const auto& zone : zones) {
        if (!zone.envelope.Intersects (point, point))
            continue;
        // The day's union AABB is only a broad-phase filter. A receiver must
        // actually cast a sunward ray through an old/new changed box to be dirty;
        // otherwise east+west suns would invalidate almost the entire site.
        for (const auto& step : sun.Steps ()) {
            if (RayHitsBounds (point, step.direction, zone.bounds))
                return true;
        }
    }
    return false;
}

} // namespace

void SetSampleMeshes (StudyRecord& record)
{
    if (!record.IsPatchDomain ()) {
        record.sampleMeshes = record.sampleGrid.groups;
        return;
    }
    if (record.snapshot == nullptr)
        return;
    MeshMap meshes;
    if (!IndexMeshes (*record.snapshot, meshes))
        return;
    record.sampleMeshes.assign (record.positions.size () / 3, UINT32_MAX);
    for (const auto& span : record.patchGrid.spans) {
        const auto mesh = meshes.find (CanonicalGuid (span.key.element));
        if (mesh == meshes.end () || span.first > record.sampleMeshes.size () ||
            span.count > record.sampleMeshes.size () - span.first) {
            record.sampleMeshes.clear ();
            return;
        }
        std::fill_n (record.sampleMeshes.begin () + span.first, span.count, static_cast<uint32_t> (mesh->second));
    }
}

size_t ReuseUnaffectedSamples (const StudyRecord& source, StudyRecord& target, const std::atomic<bool>& cancelled)
{
    if (source.snapshot == nullptr || target.snapshot == nullptr || !source.defaultRayBounds ||
        !source.session.Progress ().converged || !SameSun (source.series, target.series) ||
        source.positions.size () % 3 != 0 || target.positions.size () % 3 != 0 ||
        source.gridSpacing != target.gridSpacing || source.domain != target.domain ||
        source.sampleMeshes.size () != source.positions.size () / 3 ||
        target.sampleMeshes.size () != target.positions.size () / 3 ||
        source.normals.size () != source.positions.size () || target.normals.size () != target.positions.size () ||
        source.elementRoles.size () != source.snapshot->meshes.size () ||
        target.elementRoles.size () != target.snapshot->meshes.size ())
        return 0;
    MeshMap before, after;
    if (!IndexMeshes (*source.snapshot, before) || !IndexMeshes (*target.snapshot, after))
        return 0;

    std::vector<GeometryChange> changes;
    std::vector<size_t> oldMesh (target.snapshot->meshes.size (), OcclusionAccumulator::kNoReuse);
    ChangedBounds scene;
    for (const auto* snapshot : { source.snapshot.get (), target.snapshot.get () }) {
        for (const auto& mesh : snapshot->meshes) {
            const auto bounds = Bounds (mesh);
            if (bounds.valid) {
                scene.Add (bounds.min);
                scene.Add (bounds.max);
            }
        }
    }
    // Samples are lifted off geometry, so include them in the travel bound too.
    for (size_t i = 0; i < target.positions.size (); i += 3)
        scene.Add (&target.positions[i]);
    double diagonalSquared = 0.0;
    for (int axis = 0; axis < 3; ++axis)
        diagonalSquared += (scene.max[axis] - scene.min[axis]) * (scene.max[axis] - scene.min[axis]);

    for (const auto& entry : before) {
        if (cancelled.load ())
            return 0;
        const auto found = after.find (entry.first);
        const auto& mesh = source.snapshot->meshes[entry.second];
        if (found == after.end ())
            changes.push_back ({ entry.first, Bounds (mesh), {} });
        else {
            const size_t next = found->second;
            if (SameMesh (mesh, target.snapshot->meshes[next]) &&
                source.elementRoles[entry.second] == target.elementRoles[next])
                oldMesh[next] = entry.second;
            else
                changes.push_back ({ entry.first, Bounds (mesh), Bounds (target.snapshot->meshes[next]) });
        }
    }
    for (const auto& entry : after) {
        if (before.count (entry.first) == 0)
            changes.push_back ({ entry.first, {}, Bounds (target.snapshot->meshes[entry.second]) });
    }
    // Bulk replacement is not a local edit. Bound classifier work rather than
    // doing samples x thousands of changed boxes x all timesteps before tracing.
    if (changes.size () > 256)
        return 0;
    std::vector<double> directions;
    for (size_t step = 0; step < target.series.StepCount (); ++step) {
        const double* direction = target.series.Step (step).direction;
        directions.insert (directions.end (), direction, direction + 3);
    }
    std::vector<ShadowZone> zones;
    const double extent = std::sqrt (diagonalSquared) + 0.001;
    for (const auto& change : changes) {
        for (const auto* bounds : { &change.oldBounds, &change.newBounds }) {
            if (bounds->valid) {
                ChangedBounds padded = *bounds;
                for (int axis = 0; axis < 3; ++axis) {
                    // Conservative roundoff margin, including survey coordinates.
                    const double pad =
                        1.0e-6 + std::max (std::abs (padded.min[axis]), std::abs (padded.max[axis])) * 1.0e-12;
                    padded.min[axis] -= pad;
                    padded.max[axis] += pad;
                }
                zones.push_back ({ padded, ShadowInfluenceEnvelope (padded, directions.data (),
                                                                    target.series.StepCount (), extent) });
            }
        }
    }

    std::vector<std::vector<size_t>> samplesBefore (source.snapshot->meshes.size ());
    for (size_t sample = 0; sample < source.sampleMeshes.size (); ++sample) {
        const uint32_t mesh = source.sampleMeshes[sample];
        if (mesh >= samplesBefore.size ())
            return 0;
        samplesBefore[mesh].push_back (sample);
    }
    std::vector<size_t> cursors (oldMesh.size (), 0);
    std::vector<size_t> reuse (target.sampleMeshes.size (), OcclusionAccumulator::kNoReuse);
    size_t reused = 0;
    for (size_t sample = 0; sample < target.sampleMeshes.size (); ++sample) {
        if (cancelled.load ())
            return 0;
        const uint32_t mesh = target.sampleMeshes[sample];
        if (mesh >= oldMesh.size ())
            return 0;
        const size_t old = oldMesh[mesh];
        const size_t index = cursors[mesh]++;
        if (old == OcclusionAccumulator::kNoReuse || index >= samplesBefore[old].size ())
            continue;
        const size_t from = samplesBefore[old][index];
        const double* point = &target.positions[sample * 3];
        if (!std::equal (point, point + 3, &source.positions[from * 3]) ||
            !std::equal (&target.normals[sample * 3], &target.normals[sample * 3 + 3], &source.normals[from * 3]) ||
            InShadowZone (point, zones, target.series))
            continue;
        reuse[sample] = from;
        ++reused;
    }
    if (reused == 0 || cancelled.load () || !target.session.SeedReusable (source.session.Accumulator (), reuse))
        return 0;
    target.reusedSamples = reused;
    return reused;
}

} // namespace evp::sunstudy
