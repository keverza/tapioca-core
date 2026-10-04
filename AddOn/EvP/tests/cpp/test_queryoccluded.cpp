// L2 offline tests for QueryEngine::Occluded — the shadow-ray query.
//
// It exists because a sunlight-hours run asks "is anything in the way" once per
// (sample x timestep) and neither Raycast nor RaycastAll answers only that:
// RaycastAll collects every hit, sorts them, interpolates a normal per hit and
// heap-allocates a vector; Raycast still interpolates a smooth normal the caller
// throws away. These tests pin the BEHAVIOUR that has to match RaycastAll, so
// the cheap path can never disagree with the expensive one about what is
// shadowed.

#include "Geometry/QueryEngine.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <cmath>

using namespace geomsrv;

namespace {

// One axis-aligned quad at z = 5, spanning x,y in [-5, 5]. Two triangles.
std::shared_ptr<const Snapshot> MakeCeilingSnapshot ()
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 1;

    Mesh mesh;
    mesh.guid = "ceiling";
    mesh.elemType = 0;
    mesh.vertices = {
        -5.0, -5.0, 5.0, 5.0, -5.0, 5.0, 5.0, 5.0, 5.0, -5.0, 5.0, 5.0,
    };
    mesh.triangles = { 0, 1, 2, 0, 2, 3 };
    snap->meshes.push_back (std::move (mesh));
    return snap;
}

const double kUp[3] = { 0.0, 0.0, 1.0 };
const double kDown[3] = { 0.0, 0.0, -1.0 };

} // namespace

TEST (QueryOccluded, StraightUpIntoTheCeilingIsOccluded)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 0.0 };
    EXPECT_TRUE (engine.Occluded (origin, kUp, 0.001, 0.0));
}

TEST (QueryOccluded, StraightDownAwayFromTheCeilingIsClear)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 0.0 };
    EXPECT_FALSE (engine.Occluded (origin, kDown, 0.001, 0.0));
}

TEST (QueryOccluded, PastTheEdgeOfTheCeilingIsClear)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 100.0, 0.0, 0.0 };
    EXPECT_FALSE (engine.Occluded (origin, kUp, 0.001, 0.0));
}

// ⚠️ THE INVARIANT THAT MATTERS. Occluded is an optimisation of RaycastAll, so
// the two must never disagree about whether a sample is shadowed. A divergence
// here is the exact shape of a study that looks right and is not.
TEST (QueryOccluded, AgreesWithRaycastAllOverASweep)
{
    QueryEngine engine (MakeCeilingSnapshot ());

    for (int ix = -8; ix <= 8; ++ix) {
        for (int iy = -8; iy <= 8; ++iy) {
            const double origin[3] = { static_cast<double> (ix), static_cast<double> (iy), 0.0 };
            for (const double* dir : { &kUp[0], &kDown[0] }) {
                const bool cheap = engine.Occluded (origin, dir, 0.001, 0.0);
                const QueryEngine::PierceResult all = engine.RaycastAll (origin, dir, 0.0, 0);
                bool expensive = false;
                for (const QueryEngine::PierceHit& hit : all.hits) {
                    if (hit.t >= 0.001) {
                        expensive = true;
                        break;
                    }
                }
                EXPECT_EQ (cheap, expensive) << "at (" << ix << ", " << iy << ") dir z " << dir[2];
            }
        }
    }
}

TEST (QueryOccluded, TmaxBoundsTheQuery)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 0.0 };

    // The ceiling is 5 m up.
    EXPECT_TRUE (engine.Occluded (origin, kUp, 0.001, 10.0));
    EXPECT_FALSE (engine.Occluded (origin, kUp, 0.001, 4.0)) << "a hit beyond tmax must not count";
}

// tmin is the self-hit threshold: it skips the surface the ray starts on.
TEST (QueryOccluded, TminSkipsTheSurfaceTheRayStartsOn)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 5.0 }; // exactly on the ceiling

    // Without a threshold the ray can catch its own surface.
    // With one it must not, and must still see nothing above.
    EXPECT_FALSE (engine.Occluded (origin, kUp, 0.001, 0.0));
}

TEST (QueryOccluded, DirectionNeedNotBeNormalised)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 0.0 };
    const double longUp[3] = { 0.0, 0.0, 1000.0 };

    // tmin/tmax are true distances in metres either way, so a 4 m cap still
    // falls short of the 5 m ceiling.
    EXPECT_TRUE (engine.Occluded (origin, longUp, 0.001, 10.0));
    EXPECT_FALSE (engine.Occluded (origin, longUp, 0.001, 4.0));
}

// ⚠️ A DEGENERATE CALLER MUST COME BACK UNSHADOWED, not shadowed. "Lit" is
// visible in a result and invites a look; "shadowed" reads exactly like a real
// occluder and hides the fault.
TEST (QueryOccluded, DegenerateInputIsNotOccluded)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 0.0 };
    const double zero[3] = { 0.0, 0.0, 0.0 };

    EXPECT_FALSE (engine.Occluded (origin, zero, 0.001, 0.0));
    EXPECT_FALSE (engine.Occluded (origin, kUp, 10.0, 4.0)) << "inverted interval";
    EXPECT_FALSE (engine.Occluded (origin, kUp, 5.0, 5.0)) << "empty interval";
}

TEST (QueryOccluded, EmptySnapshotOccludesNothing)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = 2;
    QueryEngine engine (snap);
    const double origin[3] = { 0.0, 0.0, 0.0 };
    EXPECT_FALSE (engine.Occluded (origin, kUp, 0.001, 0.0));
}

TEST (QueryOccluded, AnyHitPreservesExactIntervalBoundaries)
{
    QueryEngine engine (MakeCeilingSnapshot ());
    const double origin[3] = { 0.0, 0.0, 0.0 };
    EXPECT_FALSE (engine.Occluded (origin, kUp, 0.001, 5.0));
    EXPECT_TRUE (engine.Occluded (origin, kUp, 5.0, 6.0));
    EXPECT_FALSE (engine.Occluded (origin, kUp, std::nextafter (5.0, 6.0), 6.0));
    EXPECT_TRUE (engine.Occluded (origin, kUp, 0.001, std::nextafter (5.0, 6.0)));
}

TEST (QueryOccluded, AnyHitAgreesWithClosestReferenceAcrossManyBranches)
{
    auto snapshot = std::make_shared<Snapshot> ();
    Mesh mesh;
    mesh.guid = "layered-planes";
    // Overlapping blockers on all three axes, including georeferenced coordinates.
    constexpr double offset = 1.0e6;
    for (int axis = 0; axis < 3; ++axis) {
        for (int layer = -20; layer <= 20; ++layer) {
            const uint32_t base = uint32_t (mesh.vertices.size () / 3);
            for (int vertex = 0; vertex < 4; ++vertex) {
                double p[3] = {};
                p[axis] = layer * 0.25;
                p[(axis + 1) % 3] = (vertex == 1 || vertex == 2) ? 8.0 : -8.0;
                p[(axis + 2) % 3] = vertex >= 2 ? 8.0 : -8.0;
                for (double value : p)
                    mesh.vertices.push_back (offset + value);
            }
            mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    }
    snapshot->meshes.push_back (std::move (mesh));
    QueryEngine engine (snapshot);
    std::mt19937 random (71029);
    std::uniform_real_distribution<double> coordinate (-10.0, 10.0);
    for (int ray = 0; ray < 2000; ++ray) {
        const double origin[3] = { offset + coordinate (random), offset + coordinate (random),
                                   offset + coordinate (random) };
        double direction[3] = { coordinate (random), coordinate (random), coordinate (random) };
        if (ray % 3 == 0)
            direction[ray % 3] = 0.0;
        const double minimum = ray % 2 == 0 ? 0.001 : 0.5;
        const double maximum = ray % 5 == 0 ? 1.0 : 25.0;
        const auto hits = engine.RaycastAll (origin, direction, maximum, 0);
        bool expected = false;
        for (const auto& hit : hits.hits)
            expected = expected || (hit.t >= minimum && hit.t < maximum);
        EXPECT_EQ (engine.Occluded (origin, direction, minimum, maximum), expected) << "ray " << ray;
    }
}
