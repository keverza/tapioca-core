#include "SunStudy/SunStudyCache.hpp"

#include <algorithm>
#include <cmath>

namespace evp::sunstudy {

namespace {

void Mix (uint64_t& hash, uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
}

// ⚠️ ROUNDED, NOT TRUNCATED, AND THAT MATTERS AT THE BOUNDARY. Truncation puts
// two values a hair either side of a quantum edge in different buckets; rounding
// puts them in the same one unless they are genuinely a quantum apart, which is
// the property the key needs.
int64_t Quantise (double value)
{
    return static_cast<int64_t> (std::llround (value / kPatchKeyQuantumM));
}

} // namespace

PatchKey MakePatchKey (const std::string& element, const SurfacePatch& patch)
{
    PatchKey key;
    key.element = element;

    uint64_t hash = 1469598103934665603ull;
    // The plane: its normal, and where it sits along that normal.
    //
    // ⚠️ THE NORMAL IS NOT MADE ABSOLUTE. The two faces of a wall share a plane
    // and are two surfaces; a key that ignored the direction would give them one
    // identity, and the cache would hand the outside's sunlight to the inside.
    for (int axis = 0; axis < 3; ++axis)
        Mix (hash, static_cast<uint64_t> (Quantise (patch.normal[axis])));
    const double offset =
        patch.origin[0] * patch.normal[0] + patch.origin[1] * patch.normal[1] + patch.origin[2] * patch.normal[2];
    Mix (hash, static_cast<uint64_t> (Quantise (offset)));

    // ...and WHERE in that plane. ⚠️ WITHOUT THIS, TWO DISCONNECTED COPLANAR
    // PATCHES OF ONE ELEMENT COLLIDE -- a balcony slab and the floor it lines up
    // with would share an identity, and each would keep overwriting the other.
    for (int axis = 0; axis < 3; ++axis)
        Mix (hash, static_cast<uint64_t> (Quantise (patch.origin[axis])));

    key.plane = hash;
    return key;
}

size_t SunStudyCache::CountInState (PatchState state) const
{
    size_t count = 0;
    for (const auto& entry : patches) {
        if (entry.second.state == state)
            ++count;
    }
    return count;
}

bool SunStudyCache::FullyCurrent () const
{
    for (const auto& entry : patches) {
        if (entry.second.state != PatchState::Current)
            return false;
    }
    return !patches.empty ();
}

size_t SunStudyCache::SampleCount () const
{
    size_t count = 0;
    for (const auto& entry : patches)
        count += entry.second.SampleCount ();
    return count;
}

std::vector<PatchKey> SunStudyCache::DirtyKeys () const
{
    // ⚠️ IN KEY ORDER, WHICH std::map ALREADY GIVES. Determinism is what lets an
    // incremental result be compared against a full one sample for sample; a set
    // that came out in a different order would make the oracle test compare two
    // different orderings of the same numbers and fail for no reason.
    std::vector<PatchKey> keys;
    for (const auto& entry : patches) {
        if (entry.second.state != PatchState::Current)
            keys.push_back (entry.first);
    }
    return keys;
}

void ChangedBounds::Add (const double point[3])
{
    if (!valid) {
        for (int axis = 0; axis < 3; ++axis)
            min[axis] = max[axis] = point[axis];
        valid = true;
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        min[axis] = std::min (min[axis], point[axis]);
        max[axis] = std::max (max[axis], point[axis]);
    }
}

bool ChangedBounds::Intersects (const double otherMin[3], const double otherMax[3]) const
{
    if (!valid)
        return false;
    for (int axis = 0; axis < 3; ++axis) {
        if (max[axis] < otherMin[axis] || min[axis] > otherMax[axis])
            return false;
    }
    return true;
}

ChangedBounds ShadowInfluenceEnvelope (const ChangedBounds& bounds, const double* sunDirections, size_t directionCount,
                                       double sceneExtent)
{
    ChangedBounds envelope;
    if (!bounds.valid)
        return envelope;

    // The occluder's own box is always in it: a surface can shade itself, and a
    // changed element's own patches are affected whatever the sun is doing.
    const double corners[8][3] = {
        { bounds.min[0], bounds.min[1], bounds.min[2] }, { bounds.max[0], bounds.min[1], bounds.min[2] },
        { bounds.min[0], bounds.max[1], bounds.min[2] }, { bounds.max[0], bounds.max[1], bounds.min[2] },
        { bounds.min[0], bounds.min[1], bounds.max[2] }, { bounds.max[0], bounds.min[1], bounds.max[2] },
        { bounds.min[0], bounds.max[1], bounds.max[2] }, { bounds.max[0], bounds.max[1], bounds.max[2] },
    };
    for (const auto& corner : corners)
        envelope.Add (corner);

    if (sunDirections == nullptr || directionCount == 0 || !(sceneExtent > 0.0))
        return envelope;

    for (size_t step = 0; step < directionCount; ++step) {
        const double* toSun = sunDirections + step * 3;
        // ⚠️ THE SHADOW FALLS AWAY FROM THE SUN, so the box is swept along MINUS
        // the direction toward it. Getting this sign backwards dirties the
        // surfaces on the sunny side and leaves the shaded ones stale -- which is
        // exactly inverted, and which produces a picture where the shadows that
        // did not move are the ones that were updated.
        const double length = std::sqrt (toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
        if (!(length > 1e-12))
            continue; // a sun below the horizon casts nothing
        const double travel[3] = { -toSun[0] / length * sceneExtent, -toSun[1] / length * sceneExtent,
                                   -toSun[2] / length * sceneExtent };
        for (const auto& corner : corners) {
            const double swept[3] = { corner[0] + travel[0], corner[1] + travel[1], corner[2] + travel[2] };
            envelope.Add (swept);
        }
    }
    return envelope;
}

std::set<PatchKey> ClassifyDirtyPatches (const SunStudyCache& cache, const std::vector<GeometryChange>& changes,
                                         const double* sunDirections, size_t directionCount, double sceneExtent)
{
    std::set<PatchKey> dirty;
    if (changes.empty ())
        return dirty;

    std::vector<ChangedBounds> envelopes;
    envelopes.reserve (changes.size () * 2);
    for (const GeometryChange& change : changes) {
        // ⚠️ BOTH ENDS OF A MOVE. A wall that moved stopped shading where it was
        // and started shading where it is; an envelope built from the new
        // position alone leaves the old shadow burned into the floor, and every
        // reported total stays plausible.
        if (change.oldBounds.valid)
            envelopes.push_back (
                ShadowInfluenceEnvelope (change.oldBounds, sunDirections, directionCount, sceneExtent));
        if (change.newBounds.valid)
            envelopes.push_back (
                ShadowInfluenceEnvelope (change.newBounds, sunDirections, directionCount, sceneExtent));
    }

    for (const auto& entry : cache.patches) {
        const PatchKey& key = entry.first;
        const PatchAnalysis& patch = entry.second;

        // ⚠️ THE CHANGED ELEMENT'S OWN PATCHES ARE ALWAYS DIRTY, whatever the
        // envelopes say. Its surfaces moved, so its samples are in the wrong
        // place -- a question of mapping rather than of shadow, and the envelope
        // test cannot answer it.
        bool hit = false;
        for (const GeometryChange& change : changes) {
            if (change.element == key.element) {
                hit = true;
                break;
            }
        }
        for (size_t i = 0; !hit && i < envelopes.size (); ++i)
            hit = envelopes[i].Intersects (patch.boundsMin, patch.boundsMax);

        if (hit)
            dirty.insert (key);
    }
    return dirty;
}

} // namespace evp::sunstudy
