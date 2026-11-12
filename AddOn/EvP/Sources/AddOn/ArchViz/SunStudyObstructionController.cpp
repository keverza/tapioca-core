#include "ArchViz/SunStudyObstructionController.hpp"

#include "ArchViz/SunStudyObstructionGeometry.hpp"
#include "Geometry/Aabb.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyPointProfile.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudySolarCut.hpp"
#include "SunStudy/SunStudySolarCutCells.hpp"
#include "SunStudy/SunStudyTaskWorker.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <mutex>
#include <utility>

namespace geomsrv::archviz {

namespace {

struct RequestKey {
    std::string studyId;
    uint64_t revision = 0, snapshotId = 0, captureStamp = 0;
    std::vector<size_t> samples;
    SunObstructionMode mode = SunObstructionMode::Off;
    int step = -1;
    int fromMinute = 0, toMinute = 1440;
    bool lifted = false;
    double liftedPoint[3] = {};
    double radius = 0.0;

    bool operator== (const RequestKey& other) const = default;
};

struct PendingRequest {
    RequestKey key;
    uint64_t generation = 0;
    std::shared_ptr<const evp::sunstudy::StudyRecord> record;
};

// The cut clears the rays by 5 cm (SunStudySolarCut.hpp); a ray grazing the
// removed part's box by less still counts as passing through it.
constexpr double kRemovedBoxPad = 0.1;

// Slab test: does the ray from `origin` along unit `direction` meet `box`
// grown by `pad`? An invalid box meets nothing.
bool RayMeetsBox (const double origin[3], const double direction[3], const geomsrv::Aabb& box, double pad)
{
    if (!box.Valid ())
        return false;
    double enter = 0.0, leave = INFINITY;
    for (int axis = 0; axis < 3; ++axis) {
        const double lo = box.mn[axis] - pad, hi = box.mx[axis] + pad;
        if (std::abs (direction[axis]) < 1.0e-15) {
            if (origin[axis] < lo || origin[axis] > hi)
                return false;
            continue;
        }
        double t0 = (lo - origin[axis]) / direction[axis], t1 = (hi - origin[axis]) / direction[axis];
        if (t0 > t1)
            std::swap (t0, t1);
        enter = (std::max) (enter, t0);
        leave = (std::min) (leave, t1);
        if (enter > leave)
            return false;
    }
    return true;
}

constexpr size_t kExposureWorkLimit = 20000000;

// Where a study cell lies, as the display draws it: its square on its own
// lattice, and the surface whose triangles bound it. Patch cells merge per
// patch; legacy triangle cells per mesh plane, so a surface's triangle seams
// do not show in the merged outline. False for a cell with no lattice.
struct CellPlacer {
    const evp::sunstudy::StudyRecord& record;
    const geomsrv::Snapshot& snapshot;
    std::vector<size_t> firstFace; // per mesh, its first global face

    CellPlacer (const evp::sunstudy::StudyRecord& r, const geomsrv::Snapshot& s) : record (r), snapshot (s)
    {
        size_t face = 0;
        for (const auto& mesh : snapshot.meshes) {
            firstFace.push_back (face);
            face += mesh.TriangleCount ();
        }
    }

    bool FaceTriangle (size_t global, evp::sunstudy::SolarCutTriangle& out) const
    {
        const auto at = std::upper_bound (firstFace.begin (), firstFace.end (), global);
        if (at == firstFace.begin ())
            return false;
        const size_t m = size_t (at - firstFace.begin ()) - 1, t = global - firstFace[m];
        const auto& mesh = snapshot.meshes[m];
        if (t >= mesh.TriangleCount ())
            return false;
        out.mesh = uint32_t (m);
        for (size_t c = 0; c < 3; ++c) {
            const size_t v = 3ull * mesh.triangles[3 * t + c];
            if (v + 2 >= mesh.vertices.size ())
                return false;
            std::copy_n (&mesh.vertices[v], 3, out.v[c]);
        }
        return true;
    }

    static void Square (const double origin[3], const double u[3], const double v[3], double spacing, long i, long j,
                        double corners[4][3])
    {
        const long du[4] = { 0, 1, 1, 0 }, dv[4] = { 0, 0, 1, 1 };
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a)
                corners[k][a] = origin[a] + u[a] * double (i + du[k]) * spacing + v[a] * double (j + dv[k]) * spacing;
    }

    // The cell, its surface key, and the global faces bounding that surface
    // (all of a patch's at once; a legacy cell brings only its own face).
    bool Place (size_t sample, evp::sunstudy::SolarCutCell& cell, std::vector<size_t>& faces) const
    {
        const double spacing = record.gridSpacing;
        if (!(spacing > 0.0))
            return false;
        if (record.IsPatchDomain ()) {
            const auto& grid = record.patchGrid;
            if (sample >= grid.spanOf.size () || sample >= grid.cellColumns.size () || sample >= grid.cellRows.size ())
                return false;
            const uint32_t span = grid.spanOf[sample];
            if (span >= grid.spans.size ())
                return false;
            const auto& patch = grid.spans[span];
            Square (patch.origin, patch.uAxis, patch.vAxis, spacing, long (grid.cellColumns[sample]),
                    long (grid.cellRows[sample]), cell.corners);
            cell.surface = (uint64_t (1) << 63) | span;
            return true; // the patch's faces are gathered once, by the caller
        }
        const auto& grid = record.sampleGrid;
        if (sample >= grid.faces.size () || sample >= grid.cellColumns.size () || sample >= grid.cellRows.size ())
            return false;
        const uint32_t face = grid.faces[sample];
        if (face >= grid.layouts.size () || !grid.layouts[face].gridded)
            return false;
        const auto& layout = grid.layouts[face];
        Square (layout.origin, layout.uAxis, layout.vAxis, spacing, layout.uStart + long (grid.cellColumns[sample]),
                layout.vStart + long (grid.cellRows[sample]), cell.corners);
        evp::sunstudy::SolarCutTriangle triangle;
        if (!FaceTriangle (face, triangle))
            return false;
        // The plane, quantised: coplanar faces of one mesh share a key.
        double e1[3], e2[3], n[3];
        for (int a = 0; a < 3; ++a)
            e1[a] = triangle.v[1][a] - triangle.v[0][a], e2[a] = triangle.v[2][a] - triangle.v[0][a];
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        const double length = std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(length > 0.0))
            return false;
        uint64_t key = 1469598103934665603ull;
        const auto mix = [&key] (int64_t value) {
            key ^= uint64_t (value);
            key *= 1099511628211ull;
        };
        mix (int64_t (triangle.mesh));
        for (int a = 0; a < 3; ++a)
            mix (std::llround (n[a] / length * 1.0e4));
        mix (std::llround ((n[0] * triangle.v[0][0] + n[1] * triangle.v[0][1] + n[2] * triangle.v[0][2]) / length *
                           1.0e3));
        cell.surface = key & ~(uint64_t (1) << 63);
        faces.push_back (face);
        return true;
    }
};

} // namespace

const char* SunSolarCutPhaseName (SunSolarCutPhase phase)
{
    switch (phase) {
        case SunSolarCutPhase::Surface:
            return "Cutting surface";
        case SunSolarCutPhase::Cut:
            return "Clipping the blockers";
        case SunSolarCutPhase::Cells:
            return "Re-tracing study cells";
        case SunSolarCutPhase::LitSurface:
            return "Tracing the won-lit surface";
        case SunSolarCutPhase::CellHours:
            return "Drawing the re-counted cells";
        case SunSolarCutPhase::Done:
            return "Done";
    }
    return "";
}

// The solar cut for one request, on the worker: the point, the slice's suns,
// which elements may block, then the cut and its preview. Moving the point
// must answer at once, so the frame is published as it grows: the cutting
// surface (a few rays), then the cut (every blocker clipped), then the shadows
// re-traced against what remains (the scene's occluders rebuilt).
static std::shared_ptr<SunObstructionFrame>
SolarCutFrame (const PendingRequest& job, const std::atomic<bool>& cancelled,
               const std::function<void (const SunObstructionFrame&)>& publish,
               const std::shared_ptr<SunSolarCutProgress>& progress)
{
    const auto& record = *job.record;
    auto frame = std::make_shared<SunObstructionFrame> ();
    frame->progress = progress;
    // Every inner loop's cancellation poll doubles as a heartbeat.
    const auto stop = [&] {
        progress->Tick ();
        return cancelled.load ();
    };
    frame->generation = job.generation;
    frame->studyId = job.key.studyId;
    frame->snapshotId = job.key.snapshotId;
    frame->captureStamp = job.key.captureStamp;
    frame->mode = SunObstructionMode::SolarCut;
    frame->fromMinute = job.key.fromMinute;
    frame->toMinute = job.key.toMinute;
    frame->windowFrom = record.hourFrom * 60;
    frame->windowTo = record.hourTo * 60;
    frame->timestepMinutes = record.timestepMinutes;
    frame->year = record.year;
    frame->month = record.month;
    frame->day = record.day;
    frame->cells = job.key.samples.size ();
    if (!record.defaultRayBounds) {
        frame->error = "Solar cut cannot reproduce this study's custom ray bounds; run with default ray bounds";
        return frame;
    }
    const size_t sample = job.key.samples.front ();
    if (!job.key.lifted && sample >= record.positions.size () / 3) {
        frame->error = "the study point's cell is outside the study";
        return frame;
    }
    evp::sunstudy::SolarCutInput input;
    if (job.key.lifted)
        std::copy (job.key.liftedPoint, job.key.liftedPoint + 3, input.point);
    else {
        std::copy (&record.positions[3 * sample], &record.positions[3 * sample] + 3, input.point);
        if (record.normals.size () == record.positions.size ())
            input.normal = &record.normals[3 * sample];
        if (sample < record.sampleMeshes.size ())
            input.ownMesh = record.sampleMeshes[sample];
    }
    for (const auto& step : record.series.Steps ()) {
        const int minute = step.time.hour * 60 + step.time.minute;
        if (minute >= job.key.fromMinute && minute < job.key.toMinute)
            input.steps.push_back (step);
    }
    input.hoursPerStep = double (record.timestepMinutes) / 60.0;
    input.radius = job.key.radius;
    const auto& snapshot = *record.snapshot;
    input.blocks.assign (snapshot.meshes.size (), 1);
    if (record.elementRoles.size () == snapshot.meshes.size ())
        for (size_t m = 0; m < snapshot.meshes.size (); ++m)
            input.blocks[m] = record.elementRoles[m] != uint8_t (evp::sunstudy::ElementRole::Ignored) ? 1 : 0;
    // Already lit: a clear ray to that sun now, through the study's occluders.
    const auto lit = [&record, &input] (const double direction[3]) {
        if (record.occluders == nullptr)
            return true;
        const double origin[3] = { input.point[0] + 0.001 * direction[0], input.point[1] + 0.001 * direction[1],
                                   input.point[2] + 0.001 * direction[2] };
        for (const auto& engine : { record.occluders->analysis, record.occluders->context })
            if (engine != nullptr && engine->Raycast (origin, direction, 0.0).hit)
                return false;
        return true;
    };
    frame->attributedSteps = input.steps.size ();
    if (!input.steps.empty ()) {
        frame->firstMinute = input.steps.front ().time.hour * 60 + input.steps.front ().time.minute;
        frame->lastMinute = input.steps.back ().time.hour * 60 + input.steps.back ().time.minute;
    }
    // Stage one: the cutting surface, as far as the farthest blocker the
    // slice's rays meet now (the full cut later trims it to what it cuts).
    {
        double reach = 0.0;
        if (record.occluders != nullptr)
            for (const auto& step : input.steps) {
                double direction[3] = { step.direction[0], step.direction[1], step.direction[2] };
                const double length =
                    std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
                if (!(length > 0.0) || direction[2] <= 0.0)
                    continue;
                for (double& c : direction)
                    c /= length;
                for (const auto& engine : { record.occluders->analysis, record.occluders->context })
                    if (engine != nullptr) {
                        const auto hit = engine->Raycast (input.point, direction, 0.0);
                        if (hit.hit)
                            reach = (std::max) (reach, hit.t);
                    }
            }
        evp::sunstudy::SolarCutResult surface;
        if (evp::sunstudy::ComputeSolarCutSurface (input, surface, lit, reach > 0.0 ? reach * 1.05 + 1.0 : 20.0) &&
            !surface.envelope.empty () && publish) {
            if (cancelled.load ())
                return nullptr;
            frame->facingHours = surface.facingHours;
            frame->litHours = surface.litHours;
            frame->sectors = surface.sectors;
            frame->cutStage = SunSolarCutStage::Surface;
            BuildSolarCutGeometry (surface, input.point, frame->drawables);
            publish (*frame);
            frame->drawables = GhPreviewDrawables ();
        }
    }
    progress->Enter (SunSolarCutPhase::Cut);
    evp::sunstudy::SolarCutResult cut;
    if (!evp::sunstudy::ComputeSolarCut (snapshot, input, cut, lit, stop)) {
        if (cancelled.load ())
            return nullptr;
        frame->error = cut.error;
        frame->cutStage = SunSolarCutStage::Done;
        return frame;
    }
    frame->removedVolume = cut.removedVolume;
    frame->facingHours = cut.facingHours;
    frame->litHours = cut.litHours;
    frame->sectors = cut.sectors;
    std::set<std::string> cutGuids;
    for (const auto& object : cut.objects) {
        frame->cutObjects.push_back ({ object.guid, object.elemType, object.removed, object.total, object.closed });
        frame->blockerGuids.push_back (object.guid);
        cutGuids.insert (evp::sunstudy::CanonicalGuid (object.guid));
    }
    frame->blockerObjects = cut.objects.size ();
    BuildSolarCutGeometry (cut, input.point, frame->drawables);
    frame->cutStage = SunSolarCutStage::Done;
    if (cut.kept.empty ())
        return frame;
    // Stage two: the cut, while its shadows are re-traced.
    if (publish) {
        frame->cutStage = SunSolarCutStage::Cut;
        publish (*frame);
        frame->cutStage = SunSolarCutStage::Done;
    }

    // The shadows the cut removes: the scene with the cut elements as they would
    // remain, and every study cell whose ray toward a slice sun stopped on a cut
    // element and now runs clear.
    //
    // ⚠️ NO COPY OF THE SCENE, NO NEW INDEX (#168). This stage once copied the
    // whole snapshot and rebuilt both occluder BVHs on every point move. The
    // after scene is instead the study's own BVHs with the cut elements masked
    // out, plus the few cut elements as they remain, tested directly.
    if (record.occluders == nullptr) {
        frame->exposure.error = "The study kept no occluders, so the shadows cannot be re-traced; run the study again";
        frame->cutCells.error = frame->exposure.error;
        progress->Enter (SunSolarCutPhase::Done);
        return frame;
    }
    std::set<std::string> keptGuids;
    geomsrv::Snapshot keptScene;
    for (const auto& [mesh, kept] : cut.kept)
        if (mesh < snapshot.meshes.size ()) {
            keptGuids.insert (evp::sunstudy::CanonicalGuid (snapshot.meshes[mesh].guid));
            keptScene.meshes.push_back (kept);
            auto& bounds = keptScene.meshes.back ().bounds;
            bounds = geomsrv::Aabb ();
            for (size_t v = 0; v + 2 < kept.vertices.size (); v += 3)
                bounds.Expand (kept.vertices[v], kept.vertices[v + 1], kept.vertices[v + 2]);
        }
    std::vector<std::pair<std::shared_ptr<const geomsrv::QueryEngine>, std::vector<uint8_t>>> masked;
    for (const auto& engine : { record.occluders->analysis, record.occluders->context }) {
        if (engine == nullptr || engine->SourceSnapshot () == nullptr)
            continue;
        const auto& meshes = engine->SourceSnapshot ()->meshes;
        std::vector<uint8_t> mask (meshes.size (), 1);
        for (size_t m = 0; m < meshes.size (); ++m)
            if (keptGuids.count (evp::sunstudy::CanonicalGuid (meshes[m].guid)) != 0)
                mask[m] = 0;
        masked.emplace_back (engine, std::move (mask));
    }
    const auto clear = [&masked, &keptScene] (const double origin[3], const double direction[3]) {
        for (const auto& [engine, mask] : masked)
            if (engine->RaycastMasked (origin, direction, 0.0, mask).hit)
                return false;
        for (size_t m = 0; m < keptScene.meshes.size (); ++m)
            if (RayMeetsBox (origin, direction, keptScene.meshes[m].bounds, 0.0) &&
                geomsrv::QueryEngine::RaycastMesh (keptScene, m, origin, direction, 0.0).hit)
                return false;
        return true;
    };
    // A ray that never passes through what goes meets the cut elements only
    // where they remain: still blocked. Only rays through the removed part's
    // box (padded) can be won, so only they are traced.
    geomsrv::Aabb removedBox;
    for (const auto& triangle : cut.removed)
        for (const auto& corner : triangle.v)
            removedBox.Expand (corner[0], corner[1], corner[2]);
    // The point itself, measured rather than assumed.
    for (const auto& sun : cut.facingSteps) {
        const double origin[3] = { input.point[0] + 0.001 * sun.direction[0], input.point[1] + 0.001 * sun.direction[1],
                                   input.point[2] + 0.001 * sun.direction[2] };
        if (clear (origin, sun.direction))
            frame->litAfterHours += input.hoursPerStep;
    }
    // The study's own cells: every ray of the WHOLE DAY that passes through
    // what goes, from a cell the study read as unlit then, re-traced past
    // what remains. Clear now, that step is won. The slice's share is the
    // count the summary gives; the whole day's is what the cell would read.
    struct Sun {
        double direction[3] = {};
        bool up = false, inSlice = false;
    };
    const auto& day = record.series.Steps ();
    std::vector<Sun> suns (day.size ());
    for (size_t i = 0; i < day.size (); ++i) {
        const double length =
            std::sqrt (day[i].direction[0] * day[i].direction[0] + day[i].direction[1] * day[i].direction[1] +
                       day[i].direction[2] * day[i].direction[2]);
        if (!(length > 0.0) || day[i].direction[2] <= 0.0)
            continue;
        for (int a = 0; a < 3; ++a)
            suns[i].direction[a] = day[i].direction[a] / length;
        suns[i].up = true;
        const int minute = day[i].time.hour * 60 + day[i].time.minute;
        suns[i].inSlice = minute >= job.key.fromMinute && minute < job.key.toMinute;
    }
    const size_t samples = record.positions.size () / 3;
    const bool haveNormals = record.normals.size () == record.positions.size ();
    progress->Enter (SunSolarCutPhase::Cells, samples);
    std::vector<size_t> candidates;
    std::vector<std::vector<uint16_t>> through; // per candidate, the steps whose ray meets what goes
    for (size_t s = 0; s < samples; ++s) {
        if ((s & 255) == 0) {
            if (cancelled.load ())
                return nullptr;
            progress->Advance (s);
        }
        const double* position = &record.positions[3 * s];
        const double* normal = haveNormals ? &record.normals[3 * s] : nullptr;
        std::vector<uint16_t> steps;
        for (size_t i = 0; i < suns.size (); ++i) {
            if (!suns[i].up || !evp::sunstudy::FacesSun (normal, suns[i].direction))
                continue;
            const double* d = suns[i].direction;
            const double origin[3] = { position[0] + 0.001 * d[0], position[1] + 0.001 * d[1],
                                       position[2] + 0.001 * d[2] };
            if (RayMeetsBox (origin, d, removedBox, kRemovedBoxPad))
                steps.push_back (uint16_t (i));
        }
        if (!steps.empty ()) {
            candidates.push_back (s);
            through.push_back (std::move (steps));
        }
    }
    // What the study read for them, step by step: the cells are re-counted
    // from the study's own result, so an unchanged step stays exactly as the
    // map shows it. Read in batches, so the store's lock is never held long.
    std::vector<std::vector<uint8_t>> stored;
    std::string cellsError;
    for (size_t first = 0; first < candidates.size () && cellsError.empty (); first += 512) {
        if (cancelled.load ())
            return nullptr;
        const std::vector<size_t> batch (candidates.begin () + std::ptrdiff_t (first),
                                         candidates.begin () +
                                             std::ptrdiff_t ((std::min) (first + 512, candidates.size ())));
        std::vector<std::vector<uint8_t>> lit;
        std::string error;
        if (!evp::sunstudy::SunStudyStore::Get ().SampleLitSteps (job.key.studyId, batch, lit, error))
            cellsError = "Cells not re-counted: " + error;
        else
            stored.insert (stored.end (), std::make_move_iterator (lit.begin ()), std::make_move_iterator (lit.end ()));
    }
    progress->total.store (samples + candidates.size ());
    const CellPlacer placer (record, snapshot);
    std::vector<evp::sunstudy::SolarCutCell> changed;
    std::vector<size_t> legacyFaces;
    std::vector<std::pair<size_t, uint64_t>> legacySurfaces;
    std::set<uint32_t> patches;
    size_t unplaced = 0;
    const double hoursPerStep = double (record.timestepMinutes) / 60.0;
    for (size_t c = 0; c < candidates.size (); ++c) {
        if ((c & 63) == 0) {
            if (cancelled.load ())
                return nullptr;
            progress->Advance (samples + c);
        }
        const size_t s = candidates[c];
        const double* position = &record.positions[3 * s];
        const bool known = cellsError.empty () && c < stored.size ();
        size_t won = 0;
        bool wonInSlice = false;
        for (const uint16_t i : through[c]) {
            const double* d = suns[i].direction;
            const double origin[3] = { position[0] + 0.001 * d[0], position[1] + 0.001 * d[1],
                                       position[2] + 0.001 * d[2] };
            if (known) {
                if (i < stored[c].size () && stored[c][i] != 0)
                    continue; // lit already
            }
            else {
                // Without the study's own reading: blocked now, and first by an element the cut changes?
                double nearest = -1.0;
                std::string by;
                for (const auto& engine : { record.occluders->analysis, record.occluders->context }) {
                    if (engine == nullptr)
                        continue;
                    const auto hit = engine->Raycast (origin, d, 0.0);
                    if (hit.hit && (nearest < 0.0 || hit.t < nearest)) {
                        nearest = hit.t;
                        by = engine->SourceSnapshot ()->meshes[hit.meshIndex].guid;
                    }
                }
                if (nearest < 0.0 || cutGuids.count (evp::sunstudy::CanonicalGuid (by)) == 0)
                    continue;
            }
            if (!clear (origin, d))
                continue;
            ++won;
            wonInSlice = wonInSlice || suns[i].inSlice;
        }
        if (wonInSlice)
            ++frame->wonCells; // the slice's count; the cells below carry the whole day
        if (won == 0 || !known)
            continue;
        evp::sunstudy::SolarCutCell cell;
        cell.sample = uint32_t (s);
        size_t litSteps = 0;
        for (const uint8_t flag : stored[c])
            litSteps += flag != 0 ? 1 : 0;
        cell.before = float (double (litSteps) * hoursPerStep);
        cell.after = float (double (litSteps + won) * hoursPerStep);
        std::vector<size_t> faces;
        if (!placer.Place (s, cell, faces)) {
            ++unplaced;
            continue;
        }
        for (const size_t face : faces) {
            legacyFaces.push_back (face);
            legacySurfaces.emplace_back (face, cell.surface);
        }
        changed.push_back (cell);
    }
    std::vector<uint32_t> receivers;
    if (record.IsPatchDomain ()) {
        const std::set<uint32_t> measured (record.patchGrid.spanOf.begin (), record.patchGrid.spanOf.end ());
        for (size_t face = 0; face < record.patchGrid.patchOfTriangle.size (); ++face)
            if (measured.count (record.patchGrid.patchOfTriangle[face]) != 0)
                receivers.push_back (uint32_t (face));
    }
    else
        receivers = record.sampleGrid.faces;
    if (receivers.empty ())
        frame->exposure.error = "Won-lit surfaces need retained triangle/patch receiver geometry";
    else {
        progress->Enter (SunSolarCutPhase::LitSurface);
        frame->cutDrawables = frame->drawables;
        if (evp::sunstudy::ComputeSolarExposure (snapshot, cut, input.blocks, receivers, input.steps, frame->exposure,
                                                 stop, kExposureWorkLimit, [&] (size_t done, size_t of) {
                                                     progress->total.store (of);
                                                     progress->Advance (done);
                                                 })) {
            AppendSolarExposure (frame->exposure, frame->drawables);
        }
    }
    if (cancelled.load ())
        return nullptr;

    // The study's cells, re-counted: drawn in the hours they would read.
    progress->Enter (SunSolarCutPhase::CellHours, changed.size ());
    if (!cellsError.empty ())
        frame->cutCells.error = cellsError;
    else if (!changed.empty ()) {
        std::map<uint64_t, std::vector<evp::sunstudy::SolarCutTriangle>> surfaces;
        std::set<size_t> faces (legacyFaces.begin (), legacyFaces.end ());
        for (const auto& cell : changed)
            if (cell.surface >> 63)
                patches.insert (uint32_t (cell.surface));
        if (!patches.empty ())
            for (size_t face = 0; face < record.patchGrid.patchOfTriangle.size (); ++face)
                if (patches.count (record.patchGrid.patchOfTriangle[face]) != 0)
                    faces.insert (face);
        std::map<size_t, uint64_t> surfaceOf; // a legacy face's plane key, from its cells
        for (const auto& [face, key] : legacySurfaces)
            surfaceOf[face] = key;
        for (const size_t face : faces) {
            evp::sunstudy::SolarCutTriangle triangle;
            if (!placer.FaceTriangle (face, triangle))
                continue;
            uint64_t key = 0;
            if (record.IsPatchDomain ())
                key = (uint64_t (1) << 63) | record.patchGrid.patchOfTriangle[face];
            else if (const auto found = surfaceOf.find (face); found != surfaceOf.end ())
                key = found->second;
            else
                continue;
            surfaces[key].push_back (triangle);
        }
        if (!evp::sunstudy::BuildSolarCutCells (changed, surfaces, frame->cutCells, stop) && cancelled.load ())
            return nullptr;
    }
    frame->cutCells.unplaced += unplaced;
    progress->Enter (SunSolarCutPhase::Done);
    return frame;
}

struct SunObstructionController::Impl {
    mutable std::mutex mutex;
    evp::sunstudy::SunStudyTaskWorker worker;
    std::optional<RequestKey> current;
    std::optional<PendingRequest> pending;
    std::shared_ptr<const SunObstructionFrame> latest;
    uint64_t generation = 0;
    bool stopping = false;

    void Clear ()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (!current && !pending && !latest && !worker.Busy ())
                return; // the viewport calls Off every frame; an empty clear is free
        }
        worker.Cancel ();
        std::lock_guard<std::mutex> lock (mutex);
        current.reset ();
        pending.reset ();
        latest.reset ();
    }

    void Pump ()
    {
        evp::sunstudy::StudyTaskCompletion completion;
        if (worker.Poll (completion) && !completion.error.empty ()) {
            std::lock_guard<std::mutex> lock (mutex);
            auto frame = std::make_shared<SunObstructionFrame> ();
            frame->generation = completion.runGeneration;
            frame->error = completion.error;
            if (current && generation == frame->generation)
                latest = std::move (frame);
        }
        if (worker.Busy ())
            return;
        std::optional<PendingRequest> request;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (stopping || !pending)
                return;
            request = std::move (pending);
            pending.reset ();
        }
        const auto job = std::make_shared<PendingRequest> (std::move (*request));
        evp::sunstudy::StudyTaskRequest task;
        task.runGeneration = job->generation;
        task.execute = [this, job] (const std::atomic<bool>& cancelled) {
            if (job->key.mode == SunObstructionMode::SolarCut) {
                const auto publish = [this, job, &cancelled] (const SunObstructionFrame& stage) {
                    auto shown = std::make_shared<SunObstructionFrame> (stage);
                    std::lock_guard<std::mutex> lock (mutex);
                    if (!cancelled.load () && current == job->key && generation == job->generation)
                        latest = std::move (shown);
                };
                auto frame = SolarCutFrame (*job, cancelled, publish, std::make_shared<SunSolarCutProgress> ());
                std::lock_guard<std::mutex> lock (mutex);
                if (frame != nullptr && !cancelled.load () && current == job->key && generation == job->generation)
                    latest = std::move (frame);
                return;
            }
            evp::sunstudy::SunObstructionDayResult day;
            bool success = true;
            const int divisions = evp::sunstudy::SunObstructionFootprintDivisions (job->key.samples.size ());
            if (job->key.mode == SunObstructionMode::WholeDay)
                success = evp::sunstudy::AttributeSunObstructionDay (
                    *job->record, job->key.samples, day, [&] { return cancelled.load (); }, divisions,
                    job->key.fromMinute, job->key.toMinute);
            else {
                day.steps.emplace_back ();
                success = evp::sunstudy::AttributeSunObstruction (*job->record, job->key.samples,
                                                                  job->record->series.Steps ()[size_t (job->key.step)],
                                                                  day.steps[0], divisions);
            }
            if (cancelled.load ())
                return;
            if (!success && day.error.empty () && !day.steps.empty ())
                day.error = day.steps[0].error;
            if (!success && day.error.empty ())
                return;
            auto frame = std::make_shared<SunObstructionFrame> ();
            frame->generation = job->generation;
            frame->studyId = job->key.studyId;
            frame->snapshotId = job->key.snapshotId;
            frame->captureStamp = job->key.captureStamp;
            frame->mode = job->key.mode;
            frame->error = day.error;
            const auto& record = *job->record;
            frame->cells = job->key.samples.size ();
            frame->fromMinute = job->key.fromMinute;
            frame->toMinute = job->key.toMinute;
            frame->windowFrom = record.hourFrom * 60;
            frame->windowTo = record.hourTo * 60;
            frame->timestepMinutes = record.timestepMinutes;
            frame->year = record.year;
            frame->month = record.month;
            frame->day = record.day;
            frame->footprintDivisions = divisions;
            frame->attributedSteps = day.steps.size ();
            if (!day.steps.empty ()) {
                frame->firstMinute = day.steps.front ().minute;
                frame->lastMinute = day.steps.back ().minute;
            }
            if (success) {
                SunObstructionGeometry geometry;
                success = BuildSunObstructionGeometry (*record.snapshot, day.steps, record.timestepMinutes, geometry);
                frame->error = geometry.error;
                if (success) {
                    frame->drawables = std::move (geometry.drawables);
                    frame->blockerObjects = geometry.blockerObjects;
                    frame->footprints = geometry.footprints;
                    frame->lit = geometry.lit;
                    frame->blocked = geometry.blocked;
                    frame->backFacing = geometry.backFacing;
                    frame->unavailable = geometry.unavailable;
                    for (const auto& blocker : geometry.blockers)
                        frame->blockerGuids.push_back (blocker.guid);
                    frame->blockers = std::move (geometry.blockers);
                    if (job->key.mode == SunObstructionMode::TimeStep && !day.steps.empty ())
                        frame->minute = day.steps[0].minute;
                }
            }
            std::lock_guard<std::mutex> lock (mutex);
            if (!cancelled.load () && current == job->key && generation == job->generation)
                latest = std::move (frame);
        };
        std::string error;
        if (!worker.Submit (std::move (task), error)) {
            std::lock_guard<std::mutex> lock (mutex);
            auto frame = std::make_shared<SunObstructionFrame> ();
            frame->generation = job->generation;
            frame->studyId = job->key.studyId;
            frame->error = error;
            if (current == job->key && generation == job->generation)
                latest = std::move (frame);
        }
    }
};

SunObstructionController::SunObstructionController () : impl_ (std::make_unique<Impl> ())
{
}

SunObstructionController::~SunObstructionController ()
{
    Shutdown ();
}

void SunObstructionController::Request (const SunObstructionRequest& request)
{
    if (request.mode == SunObstructionMode::Off || request.studyId.empty () || request.samples.empty ()) {
        impl_->Clear ();
        return;
    }
    const auto record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (request.studyId);
    if (record == nullptr || record->snapshot == nullptr) {
        impl_->Clear ();
        return;
    }
    const auto& steps = record->series.Steps ();
    if (request.mode == SunObstructionMode::TimeStep && (request.step < 0 || size_t (request.step) >= steps.size ())) {
        impl_->Clear ();
        return;
    }
    RequestKey key {
        request.studyId,
        record->storeRevision,
        record->snapshotId != 0 ? record->snapshotId : record->snapshot->id,
        record->snapshot->captureStamp,
        request.samples,
        request.mode,
        request.mode == SunObstructionMode::TimeStep ? request.step : -1,
        request.mode != SunObstructionMode::TimeStep ? request.fromMinute : 0,
        request.mode != SunObstructionMode::TimeStep ? request.toMinute : 1440,
        request.mode == SunObstructionMode::SolarCut && request.lifted,
        { request.liftedPoint[0], request.liftedPoint[1], request.liftedPoint[2] },
        request.mode == SunObstructionMode::SolarCut ? (std::max) (0.0, request.radius) : 0.0,
    };
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        if (impl_->stopping || (impl_->current == key && (impl_->pending || impl_->latest || impl_->worker.Busy ())))
            return;
        impl_->worker.Cancel ();
        impl_->latest.reset ();
        impl_->current = key;
        impl_->pending = PendingRequest { key, ++impl_->generation, record };
    }
    impl_->Pump ();
}

std::shared_ptr<const SunObstructionFrame> SunObstructionController::Latest ()
{
    impl_->Pump ();
    std::lock_guard<std::mutex> lock (impl_->mutex);
    return impl_->latest;
}

bool SunObstructionController::Busy () const
{
    std::lock_guard<std::mutex> lock (impl_->mutex);
    return impl_->pending.has_value () || impl_->worker.Busy ();
}

void SunObstructionController::Cancel ()
{
    impl_->Clear ();
}

void SunObstructionController::Shutdown ()
{
    {
        std::lock_guard<std::mutex> lock (impl_->mutex);
        if (impl_->stopping)
            return;
        impl_->stopping = true;
        impl_->pending.reset ();
        impl_->current.reset ();
        impl_->latest.reset ();
    }
    impl_->worker.Shutdown ();
}

SunObstructionController& SunObstructionTasks ()
{
    static SunObstructionController controller;
    return controller;
}

void ShutdownSunObstructionTasks ()
{
    SunObstructionTasks ().Shutdown ();
}

void ResetSunObstructionTasks ()
{
    SunObstructionTasks ().Cancel ();
}

bool SunObstructionSnapshotCurrent (const SunObstructionFrame& frame,
                                    const std::shared_ptr<const geomsrv::Snapshot>& shared, uint64_t captureStamp)
{
    return shared != nullptr && frame.snapshotId != 0 && shared->id == frame.snapshotId &&
           shared->captureStamp == frame.captureStamp && captureStamp == frame.captureStamp;
}

bool SunObstructionFrameCurrent (const std::shared_ptr<const SunObstructionFrame>& frame,
                                 const std::string& shownStudyId)
{
    if (frame == nullptr || frame->studyId != shownStudyId)
        return false;
    return SunObstructionSnapshotCurrent (*frame, MeshStore::Get ().Shared (), MeshStore::Get ().CaptureStamp ());
}

} // namespace geomsrv::archviz
