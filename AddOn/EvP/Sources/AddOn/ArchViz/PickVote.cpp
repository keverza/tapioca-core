#include "ArchViz/PickVote.hpp"

#include <cmath>

namespace geomsrv {
namespace archviz {

namespace {

// The most common non-zero id in a list, or 0. Ties go to the earlier entry,
// which for the nearest-texel set means the sample nearer the top-left of a 2x2
// -- arbitrary, but only ever reached when two ids are equally represented among
// the texels closest to the cursor, where there is no better answer available.
uint32_t MostCommon (const uint32_t* values, uint32_t count)
{
    uint32_t bestId = 0;
    uint32_t bestCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (values[i] == 0)
            continue;
        uint32_t seen = 0;
        for (uint32_t j = 0; j < count; ++j) {
            if (values[j] == values[i])
                ++seen;
        }
        if (seen > bestCount) {
            bestCount = seen;
            bestId = values[i];
        }
    }
    return bestId;
}

// How many texels can be exactly equidistant from a point in a grid: four, when
// the point is the corner shared by four texel centres. Anything beyond that is
// floating-point noise, and taking the first four of it is a better failure than
// growing an allocation on the render thread.
constexpr uint32_t kMaxNearest = 4;

// Squared distances are compared for EQUALITY to find the equidistant set, and
// they are computed from coordinates that are exact halves and integers -- so
// they agree bit for bit in the cases that matter. The slack is there for the
// cases that do not (a cursor offset that came from arithmetic), where being one
// ULP apart should not silently drop a texel out of the tie.
constexpr float kDistanceSlack = 1e-4f;

}   // namespace

uint32_t ResolvePickIdAt (const uint32_t* ids, uint32_t width, uint32_t height, float centreX,
                          float centreY)
{
    if (ids == nullptr || width == 0 || height == 0)
        return 0;

    const uint32_t total = width * height;

    auto distanceSq = [&] (uint32_t index) {
        const float dx = float (index % width) - centreX;
        const float dy = float (index / width) - centreY;
        return dx * dx + dy * dy;
    };

    // ---- 1 & 2: the texels the cursor is actually on ------------------------
    // The nearest texel GEOMETRICALLY, background included -- the set is decided
    // before the ids are looked at, so "the cursor was over the sky" stays
    // representable. With the cursor on a texel centre this is one texel; with it
    // on the corner between four (an even-sized block sampled around its middle)
    // it is those four, and the most common of them wins.
    float nearestSq = distanceSq (0);
    for (uint32_t i = 1; i < total; ++i)
        nearestSq = (distanceSq (i) < nearestSq) ? distanceSq (i) : nearestSq;

    uint32_t nearest[kMaxNearest] = {};
    uint32_t nearestCount = 0;
    for (uint32_t i = 0; i < total && nearestCount < kMaxNearest; ++i) {
        if (distanceSq (i) <= nearestSq + kDistanceSlack)
            nearest[nearestCount++] = ids[i];
    }
    const uint32_t winner = MostCommon (nearest, nearestCount);
    if (winner != 0)
        return winner;

    // ---- 3: the fallback -- NEAREST NON-ZERO TEXEL WINS ---------------------
    //
    // Everything under the cursor was background, so the user missed narrowly and
    // the question is "which element were they nearest to".
    //
    // ⚠️ NEAREST, NOT A WEIGHTED SUM, AND THE DIFFERENCE IS NOT COSMETIC. A sum
    // of 1/(1+d^2) still lets AREA win: nine texels of a large surface in the
    // corner outscore two texels of a small one just off centre (0.74 against
    // 0.57 -- measured, and it is what the offline test caught). Any additive
    // score is a size contest with extra steps, which is the exact fault the
    // centre-first rule above exists to avoid. Taking the single closest sample
    // makes the answer depend only on DISTANCE, so a thin railing one texel away
    // beats a wall two texels away no matter how much of the block the wall
    // fills.
    //
    // Count breaks a tie, which only happens when two elements have a texel the
    // same distance out -- there the larger presence is the better guess.
    uint32_t bestId = 0;
    float bestDistanceSq = 0.0f;
    uint32_t bestCount = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t candidate = ids[i];
        if (candidate == 0)
            continue;

        const float candidateSq = distanceSq (i);

        if (bestId == 0 || candidateSq < bestDistanceSq) {
            bestId = candidate;
            bestDistanceSq = candidateSq;
            bestCount = 0;   // recounted below, lazily, only when a tie needs it
            for (uint32_t j = 0; j < total; ++j) {
                if (ids[j] == candidate)
                    ++bestCount;
            }
        } else if (candidate != bestId && candidateSq == bestDistanceSq) {
            uint32_t count = 0;
            for (uint32_t j = 0; j < total; ++j) {
                if (ids[j] == candidate)
                    ++count;
            }
            if (count > bestCount) {
                bestId = candidate;
                bestCount = count;
            }
        }
    }

    return bestId;
}

PickReadback PlanPickReadback (int32_t px, int32_t py, uint32_t targetWidth,
                               uint32_t targetHeight, uint32_t boxSize)
{
    PickReadback out;
    if (targetWidth == 0 || targetHeight == 0 || boxSize == 0)
        return out;
    if (px < 0 || py < 0 || px >= int32_t (targetWidth) || py >= int32_t (targetHeight))
        return out;

    // ⚠️ THE BOX SHRINKS TO THE TARGET rather than the request failing. A viewport
    // narrower than the box is a real state during a resize, and refusing to pick
    // there would read as picking dying whenever the palette is dragged small.
    const uint32_t width = boxSize < targetWidth ? boxSize : targetWidth;
    const uint32_t height = boxSize < targetHeight ? boxSize : targetHeight;

    // Centre the box on the cursor, then slide it back inside the target.
    const int32_t limitX = int32_t (targetWidth - width);
    const int32_t limitY = int32_t (targetHeight - height);
    int32_t minX = px - int32_t (width) / 2;
    int32_t minY = py - int32_t (height) / 2;
    minX = minX < 0 ? 0 : (minX > limitX ? limitX : minX);
    minY = minY < 0 ? 0 : (minY > limitY ? limitY : minY);

    out.valid = true;
    out.minX = uint32_t (minX);
    out.minY = uint32_t (minY);
    out.width = width;
    out.height = height;
    // ⚠️ MEASURED FROM THE BOX THAT WILL ACTUALLY BE COPIED, after the clamp --
    // which is the entire reason this returns a centre at all instead of the
    // caller assuming the middle. The copy is 1:1 out of the id buffer, so the
    // cursor's texel is simply its offset from the box's corner.
    out.centreX = float (px - minX);
    out.centreY = float (py - minY);
    return out;
}

uint32_t ResolvePickId (const uint32_t* ids, uint32_t size)
{
    // The middle of the block: (size-1)/2 is a texel centre for an odd size and
    // the corner between four for an even one, which is exactly the distinction
    // the nearest-texel rule above already makes.
    const float middle = (float (size) - 1.0f) * 0.5f;
    return ResolvePickIdAt (ids, size, size, middle, middle);
}

}   // namespace archviz
}   // namespace geomsrv
