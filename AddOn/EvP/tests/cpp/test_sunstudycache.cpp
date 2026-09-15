// Tests for SunStudy/SunStudyCache — which patches a change invalidates, and
// whether an incrementally updated cache equals a clean full recomputation.
//
// ⚠️ THE LAST TEST IN THIS FILE IS THE ONE THAT MATTERS. Incremental analysis
// fails PLAUSIBLY: a patch that should have been recomputed and was not keeps a
// shadow from a wall that has moved, in a picture that is complete, smooth and
// entirely finished-looking. Nothing downstream can detect it, no viewport shows
// it, and every reported total stays believable. The only thing that can catch
// it is running the whole analysis both ways and requiring the numbers to agree.

#include <cmath>
#include <string>
#include <vector>

#include <memory>

#include "Geometry/Mesh.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyCache.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"
#include "SunStudy/SurfacePatch.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// ---------------------------------------------------------------------------
// the fixture the task asked for: a receiver floor, an occluding wall, and an
// unrelated wall far enough away that no sun in the study can connect them.
// ---------------------------------------------------------------------------

struct Piece {
    std::string element;
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
};

// An axis-aligned quad as two triangles.
Piece Quad (const std::string& element, double x0, double y0, double z0, double x1, double y1, double z1)
{
    Piece piece;
    piece.element = element;
    // Degenerate in exactly one axis; expand the other two.
    if (std::fabs (z1 - z0) < 1e-9) { // horizontal
        piece.vertices = { x0, y0, z0, x1, y0, z0, x1, y1, z0, x0, y1, z0 };
    }
    else if (std::fabs (y1 - y0) < 1e-9) { // wall along x, standing in z
        piece.vertices = { x0, y0, z0, x1, y0, z0, x1, y1, z1, x0, y1, z1 };
    }
    else {
        piece.vertices = { x0, y0, z0, x0, y1, z0, x1, y1, z1, x1, y0, z1 };
    }
    piece.triangles = { 0, 1, 2, 0, 2, 3 };
    return piece;
}

struct Scene {
    std::vector<Piece> pieces;

    // Flattened, with one group id per triangle.
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
    std::vector<std::string> elementOf; // per group

    void Build ()
    {
        vertices.clear ();
        triangles.clear ();
        groups.clear ();
        elementOf.clear ();
        for (const Piece& piece : pieces) {
            const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
            const uint32_t group = static_cast<uint32_t> (elementOf.size ());
            elementOf.push_back (piece.element);
            vertices.insert (vertices.end (), piece.vertices.begin (), piece.vertices.end ());
            for (const uint32_t index : piece.triangles)
                triangles.push_back (base + index);
            groups.resize (triangles.size () / 3, group);
        }
    }

    double Extent () const
    {
        double lo[3] = { 1e30, 1e30, 1e30 };
        double hi[3] = { -1e30, -1e30, -1e30 };
        for (size_t v = 0; v + 2 < vertices.size (); v += 3) {
            for (int axis = 0; axis < 3; ++axis) {
                lo[axis] = std::min (lo[axis], vertices[v + axis]);
                hi[axis] = std::max (hi[axis], vertices[v + axis]);
            }
        }
        double diagonal = 0.0;
        for (int axis = 0; axis < 3; ++axis)
            diagonal += (hi[axis] - lo[axis]) * (hi[axis] - lo[axis]);
        return std::sqrt (diagonal) + 1.0;
    }
};

// wallA stands at x = wallAx and shades the near half of the floor.
Scene MakeScene (double wallAx)
{
    Scene scene;
    scene.pieces.push_back (Quad ("floor", 0, 0, 0, 20, 12, 0));
    scene.pieces.push_back (Quad ("wallA", wallAx, 0, 0, wallAx + 0.2, 12, 6));
    // Far away along +x, well outside anything the study's suns can sweep onto
    // the floor.
    scene.pieces.push_back (Quad ("wallB", 60, 0, 0, 60.2, 12, 6));
    scene.Build ();
    return scene;
}

// A handful of suns, all from +x and above, so shadows fall toward -x.
std::vector<double> SunDirections ()
{
    std::vector<double> dirs;
    for (int i = 0; i < 4; ++i) {
        const double altitude = 0.5 + i * 0.15; // radians
        dirs.push_back (std::cos (altitude));   // +x
        dirs.push_back (0.0);
        dirs.push_back (std::sin (altitude));
    }
    return dirs;
}

geomsrv::Snapshot SnapshotOf (const Scene& scene, uint64_t id)
{
    geomsrv::Snapshot snapshot;
    snapshot.id = id;
    for (size_t p = 0; p < scene.pieces.size (); ++p) {
        geomsrv::Mesh mesh;
        mesh.guid = scene.pieces[p].element;
        mesh.vertices = scene.pieces[p].vertices;
        mesh.triangles = scene.pieces[p].triangles;
        mesh.triMaterial.assign (mesh.triangles.size () / 3, 0);
        for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3)
            mesh.bounds.Expand (mesh.vertices[v], mesh.vertices[v + 1], mesh.vertices[v + 2]);
        snapshot.meshes.push_back (std::move (mesh));
    }
    return snapshot;
}

// Build the cache's patches and samples for a scene. Hours are left empty.
SunStudyCache BuildCache (const Scene& scene, double spacing)
{
    SunStudyCache cache;
    cache.spacing = spacing;
    cache.hoursPerStep = 1.0;

    const std::vector<SurfacePatch> patches =
        BuildSurfacePatches (scene.vertices.data (), scene.vertices.size () / 3, scene.triangles.data (),
                             scene.triangles.size () / 3, scene.groups.data ());
    for (const SurfacePatch& patch : patches) {
        const PatchGrid grid = BuildPatchGrid (patch, scene.vertices.data (), scene.triangles.data (), spacing);
        PatchAnalysis analysis;
        analysis.key = MakePatchKey (scene.elementOf[patch.group], patch);
        for (const PatchCell& cell : grid.cells) {
            // Lift off the surface, exactly as the sampler does: a ray that
            // starts ON a face hits it.
            for (int axis = 0; axis < 3; ++axis)
                analysis.positions.push_back (cell.centre[axis] + patch.normal[axis] * 0.01);
            for (int axis = 0; axis < 3; ++axis)
                analysis.normals.push_back (patch.normal[axis]);
            analysis.areas.push_back (cell.area);
        }
        // The patch's own AABB, from its corners.
        ChangedBounds bounds;
        for (const uint32_t face : patch.triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t index = scene.triangles[face * 3 + corner];
                const double p[3] = { scene.vertices[index * 3 + 0], scene.vertices[index * 3 + 1],
                                      scene.vertices[index * 3 + 2] };
                bounds.Add (p);
            }
        }
        for (int axis = 0; axis < 3; ++axis) {
            analysis.boundsMin[axis] = bounds.min[axis];
            analysis.boundsMax[axis] = bounds.max[axis];
        }
        analysis.state = PatchState::Dirty;
        cache.patches[analysis.key] = std::move (analysis);
    }
    return cache;
}

// Resolve `keys` against `scene`'s geometry. Everything else is untouched --
// which is the property the incremental path exists for.
size_t ResolvePatches (SunStudyCache& cache, const Scene& scene, const std::vector<PatchKey>& keys,
                       const std::vector<double>& sunDirections)
{
    // The engine takes the snapshot at construction; it is heavy and immutable
    // once built, which is exactly what lets it be shared by concurrent readers.
    auto snapshot = std::make_shared<const geomsrv::Snapshot> (SnapshotOf (scene, 1));
    auto engine = std::make_shared<const geomsrv::QueryEngine> (snapshot);
    const auto traversal = std::make_shared<CpuTraversal> (engine);

    const size_t steps = sunDirections.size () / 3;
    size_t resolved = 0;
    for (const PatchKey& key : keys) {
        auto found = cache.patches.find (key);
        if (found == cache.patches.end ())
            continue;
        PatchAnalysis& patch = found->second;
        if (patch.SampleCount () == 0) {
            patch.state = PatchState::Current;
            continue;
        }

        // ⚠️ A FRESH ACCUMULATOR PER PATCH. Its samples are a contiguous span, so
        // a subset run is the same code over a shorter SampleSet -- there is no
        // second engine here, which is the whole point.
        OcclusionAccumulator accumulator (patch.SampleCount (), steps);
        SampleSet samples;
        samples.positions = patch.positions.data ();
        samples.normals = patch.normals.data ();
        samples.count = patch.SampleCount ();
        for (size_t step = 0; step < steps; ++step)
            accumulator.AccumulateStep (*traversal, samples, step, &sunDirections[step * 3]);
        patch.hours = accumulator.SunHours (cache.hoursPerStep);
        patch.state = PatchState::Current;
        ++resolved;
    }
    return resolved;
}

std::vector<PatchKey> AllKeys (const SunStudyCache& cache)
{
    std::vector<PatchKey> keys;
    for (const auto& entry : cache.patches)
        keys.push_back (entry.first);
    return keys;
}

std::vector<GeometryChange> MovedWallA (const Scene& before, const Scene& after)
{
    GeometryChange change;
    change.element = "wallA";
    for (const Scene* scene : { &before, &after }) {
        ChangedBounds bounds;
        for (const Piece& piece : scene->pieces) {
            if (piece.element != "wallA")
                continue;
            for (size_t v = 0; v + 2 < piece.vertices.size (); v += 3) {
                const double p[3] = { piece.vertices[v], piece.vertices[v + 1], piece.vertices[v + 2] };
                bounds.Add (p);
            }
        }
        (scene == &before ? change.oldBounds : change.newBounds) = bounds;
    }
    return { change };
}

} // namespace

// ---------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------

TEST (SunStudyCache, APatchKeepsItsIdentityWhenSomethingElseChanges)
{
    // ⚠️ THE PROPERTY THE WHOLE CACHE RESTS ON. If an untouched wall's patches
    // were re-keyed by an edit elsewhere, every edit would invalidate everything
    // and the cache would be an expensive way to do exactly what the whole-study
    // follower already did.
    const Scene before = MakeScene (8.0);
    const Scene after = MakeScene (11.0); // wallA moved; floor and wallB did not

    const SunStudyCache a = BuildCache (before, 2.0);
    const SunStudyCache b = BuildCache (after, 2.0);

    size_t shared = 0;
    for (const auto& entry : a.patches) {
        if (entry.first.element == "wallA")
            continue;
        EXPECT_TRUE (b.patches.count (entry.first) == 1)
            << "a patch of '" << entry.first.element << "' lost its identity";
        ++shared;
    }
    EXPECT_GT (shared, 0u);

    // And the wall that moved did NOT keep its identity -- its samples are in
    // the wrong place now, and a key that survived would hand the new wall the
    // old wall's sunlight.
    bool wallAKeyReused = false;
    for (const auto& entry : a.patches) {
        if (entry.first.element == "wallA" && b.patches.count (entry.first) == 1)
            wallAKeyReused = true;
    }
    EXPECT_FALSE (wallAKeyReused);
}

TEST (SunStudyCache, TheTwoFacesOfAWallAreTwoIdentities)
{
    // Same plane, opposite normals. A key that made the normal absolute would
    // give them one identity and hand the outside's sunlight to the inside.
    SurfacePatch front;
    front.normal[0] = 0.0;
    front.normal[1] = 1.0;
    front.normal[2] = 0.0;
    SurfacePatch back = front;
    back.normal[1] = -1.0;
    EXPECT_FALSE (MakePatchKey ("w", front) == MakePatchKey ("w", back));
}

// ---------------------------------------------------------------------------
// the swept envelope
// ---------------------------------------------------------------------------

TEST (SunStudyCache, TheEnvelopeSweepsAwayFromTheSunNotTowardIt)
{
    // ⚠️ THE SIGN. A shadow falls AWAY from the sun; sweeping the other way
    // dirties the sunny side and leaves the shaded side stale, which is exactly
    // inverted and which produces a picture where the shadows that did not move
    // are the ones that were updated.
    ChangedBounds box;
    const double lo[3] = { 0.0, 0.0, 0.0 };
    const double hi[3] = { 1.0, 1.0, 1.0 };
    box.Add (lo);
    box.Add (hi);

    const double toSun[3] = { 1.0, 0.0, 0.0 }; // sun in the +x direction
    const ChangedBounds envelope = ShadowInfluenceEnvelope (box, toSun, 1, 10.0);
    ASSERT_TRUE (envelope.valid);
    EXPECT_NEAR (envelope.min[0], -10.0, 1e-9) << "the envelope must extend into -x, where the shadow falls";
    EXPECT_NEAR (envelope.max[0], 1.0, 1e-9) << "and must not extend past the occluder toward the sun";
}

TEST (SunStudyCache, ASunBelowTheHorizonSweepsNothing)
{
    ChangedBounds box;
    const double lo[3] = { 0.0, 0.0, 0.0 };
    const double hi[3] = { 1.0, 1.0, 1.0 };
    box.Add (lo);
    box.Add (hi);
    const double none[3] = { 0.0, 0.0, 0.0 }; // a zero vector, as a filtered step
    const ChangedBounds envelope = ShadowInfluenceEnvelope (box, none, 1, 10.0);
    ASSERT_TRUE (envelope.valid);
    // Just the occluder's own box: it can still shade itself.
    EXPECT_NEAR (envelope.max[0], 1.0, 1e-9);
    EXPECT_NEAR (envelope.min[0], 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// classification
// ---------------------------------------------------------------------------

TEST (SunStudyCache, MovingOneWallDirtiesItAndItsShadowButNotTheFarSideOfTheSite)
{
    const Scene before = MakeScene (8.0);
    const Scene after = MakeScene (11.0);
    const SunStudyCache cache = BuildCache (after, 2.0);
    const std::vector<double> suns = SunDirections ();

    const std::set<PatchKey> dirty =
        ClassifyDirtyPatches (cache, MovedWallA (before, after), suns.data (), suns.size () / 3, after.Extent ());

    size_t wallADirty = 0;
    size_t wallBDirty = 0;
    for (const PatchKey& key : dirty) {
        if (key.element == "wallA")
            ++wallADirty;
        if (key.element == "wallB")
            ++wallBDirty;
    }
    EXPECT_GT (wallADirty, 0u) << "the element that moved must always be dirty";
    // ⚠️ wallB IS 50 m AWAY ON THE SUNNY SIDE. Every sun in this study comes from
    // +x, so wallA's shadow travels toward -x and can never reach it. A
    // classifier that dirtied it anyway would be conservative but useless -- it
    // would recompute the whole model on every edit.
    EXPECT_EQ (wallBDirty, 0u) << "an element the shadow cannot reach must stay current";
    EXPECT_LT (dirty.size (), cache.patches.size ()) << "something must have survived, or nothing was gained";
}

TEST (SunStudyCache, NoChangesDirtyNothing)
{
    const Scene scene = MakeScene (8.0);
    const SunStudyCache cache = BuildCache (scene, 2.0);
    const std::vector<double> suns = SunDirections ();
    EXPECT_TRUE (ClassifyDirtyPatches (cache, {}, suns.data (), suns.size () / 3, scene.Extent ()).empty ());
}

// ---------------------------------------------------------------------------
// THE ORACLE
// ---------------------------------------------------------------------------

TEST (SunStudyCache, AnIncrementallyUpdatedCacheEqualsACleanFullRecomputation)
{
    // ⚠️ THE MOST IMPORTANT TEST IN THE SUN STUDY. Incremental analysis fails
    // PLAUSIBLY: a patch that should have been recomputed and was not keeps a
    // shadow from a wall that has moved, in a picture that is complete and
    // finished-looking, with every total still believable. Only running it both
    // ways and demanding the same numbers can catch that.
    const double spacing = 1.0;
    const std::vector<double> suns = SunDirections ();
    const Scene before = MakeScene (8.0);
    const Scene after = MakeScene (11.0);

    // ---- 1. a complete baseline against the model as it was ----------------
    SunStudyCache cache = BuildCache (before, spacing);
    ASSERT_GT (cache.patches.size (), 2u);
    ResolvePatches (cache, before, AllKeys (cache), suns);
    ASSERT_TRUE (cache.FullyCurrent ());
    const size_t baselineSamples = cache.SampleCount ();
    ASSERT_GT (baselineSamples, 50u);

    // ---- 2. the wall moves -------------------------------------------------
    const std::vector<GeometryChange> changes = MovedWallA (before, after);
    const std::set<PatchKey> dirty =
        ClassifyDirtyPatches (cache, changes, suns.data (), suns.size () / 3, after.Extent ());

    // Re-key against the new model: patches that did not move keep their keys and
    // their hours; the moved wall's are new and start Dirty.
    SunStudyCache incremental = BuildCache (after, spacing);
    size_t reused = 0;
    for (auto& entry : incremental.patches) {
        const auto previous = cache.patches.find (entry.first);
        if (previous == cache.patches.end ())
            continue; // a surface that did not exist before
        if (dirty.count (entry.first) != 0)
            continue; // classified as affected: it must be recomputed
        // ⚠️ THE HOURS ARE CARRIED OVER WITHOUT A SINGLE RAY. This is the entire
        // claim of the feature, and the assertion below is what makes it safe.
        entry.second.hours = previous->second.hours;
        entry.second.state = PatchState::Current;
        ++reused;
    }
    EXPECT_GT (reused, 0u) << "nothing was reused, so nothing was saved";

    const std::vector<PatchKey> toResolve = incremental.DirtyKeys ();
    EXPECT_LT (toResolve.size (), incremental.patches.size ()) << "everything was recomputed";
    ResolvePatches (incremental, after, toResolve, suns);
    ASSERT_TRUE (incremental.FullyCurrent ());

    // ---- 3. the oracle: throw it all away and start again ------------------
    SunStudyCache clean = BuildCache (after, spacing);
    ResolvePatches (clean, after, AllKeys (clean), suns);
    ASSERT_TRUE (clean.FullyCurrent ());

    // ---- 4. they must agree, sample for sample -----------------------------
    ASSERT_EQ (incremental.patches.size (), clean.patches.size ());
    size_t compared = 0;
    for (const auto& entry : clean.patches) {
        const auto mine = incremental.patches.find (entry.first);
        ASSERT_NE (mine, incremental.patches.end ()) << "the incremental cache is missing a patch";
        ASSERT_EQ (mine->second.hours.size (), entry.second.hours.size ());
        for (size_t i = 0; i < entry.second.hours.size (); ++i) {
            EXPECT_DOUBLE_EQ (mine->second.hours[i], entry.second.hours[i])
                << "patch '" << entry.first.element << "' sample " << i
                << ": the reused value disagrees with a clean recomputation";
            ++compared;
        }
    }
    EXPECT_GT (compared, 50u);
}

TEST (SunStudyCache, SkippingTheClassifierProducesTheWRONGAnswer)
{
    // ⚠️ THE TEST THAT PROVES THE ORACLE HAS TEETH. If reusing EVERY patch --
    // including the ones in the moved wall's shadow -- still matched a clean
    // recomputation, the oracle above would be passing vacuously and would go on
    // passing after the classifier was broken.
    const double spacing = 1.0;
    const std::vector<double> suns = SunDirections ();
    const Scene before = MakeScene (8.0);
    const Scene after = MakeScene (11.0);

    SunStudyCache cache = BuildCache (before, spacing);
    ResolvePatches (cache, before, AllKeys (cache), suns);

    SunStudyCache lazy = BuildCache (after, spacing);
    for (auto& entry : lazy.patches) {
        const auto previous = cache.patches.find (entry.first);
        if (previous == cache.patches.end ())
            continue;
        entry.second.hours = previous->second.hours; // reuse everything, wrongly
        entry.second.state = PatchState::Current;
    }
    ResolvePatches (lazy, after, lazy.DirtyKeys (), suns);

    SunStudyCache clean = BuildCache (after, spacing);
    ResolvePatches (clean, after, AllKeys (clean), suns);

    bool anyDisagreement = false;
    for (const auto& entry : clean.patches) {
        const auto mine = lazy.patches.find (entry.first);
        if (mine == lazy.patches.end () || mine->second.hours.size () != entry.second.hours.size ())
            continue;
        for (size_t i = 0; i < entry.second.hours.size (); ++i) {
            if (std::fabs (mine->second.hours[i] - entry.second.hours[i]) > 1e-9)
                anyDisagreement = true;
        }
    }
    EXPECT_TRUE (anyDisagreement) << "the fixture does not actually move any shadow, so the oracle proves nothing";
}
