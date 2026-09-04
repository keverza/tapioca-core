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
