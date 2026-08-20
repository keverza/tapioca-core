#ifndef EVP_ARCHVIZ_PICKVOTE_HPP
#define EVP_ARCHVIZ_PICKVOTE_HPP

// ArchViz/PickVote — turning an NxN block of element ids into "the one the user
// meant". Pure arithmetic over a small array: no Diligent, no GPU, no ACAPI, so
// the rule that decides a click is covered by the offline suite rather than only
// by clicking things in Archicad and seeing what happens.
//
// It is a separate unit for exactly that reason. The rule has now been wrong
// three times (PLAT-RE44, and the accuracy complaints after it), each time
// discovered by a human clicking on a building, and each round trip cost a
// build, a sync and a restart. The rule is arithmetic; arithmetic can be tested.
//
// THE RULE, and why each part of it is there:
//
//   1. THE TEXELS NEAREST THE CURSOR WIN IF THEY HIT ANYTHING. A pure majority
//      answers "what was the cursor MOSTLY over", which is the wrong question:
//      someone aiming at a door handle in front of a wall is deliberately over
//      the handle, and the wall wins a majority every time. That is the reported
//      "selects the object behind". With the cursor on a texel CENTRE there is
//      exactly one nearest texel; with it between texels (an even-sized block
//      sampled around its own middle) there are the four straddling it.
//   2. AMONG THOSE NEAREST TEXELS, THE MOST COMMON WINS -- not the first in scan
//      order. Taking the first biased every tie up and to the left by half a
//      texel, which on a boundary between two surfaces is a visible, repeatable
//      wrong answer rather than a coin toss.
//   3. THE FALLBACK IS THE NEAREST TEXEL, not a count and not a weighted sum.
//      ANY additive score is a size contest with extra steps: a flat count lets a
//      large surface in the corner outvote a small one two texels from the
//      cursor, and weighting by 1/(1+d^2) merely narrows that gap without closing
//      it (9 far texels still beat 2 near ones, 0.74 to 0.57 -- measured in
//      test_pickvote). Taking the single closest sample makes the answer depend
//      on DISTANCE alone, which is the thing the cursor actually expresses.
//      Count breaks a tie, where the larger presence is the better guess.
//
// ⚠️ ID 0 IS BACKGROUND -- "the user clicked the sky" -- and is never a winner.
// Returning 0 is a real answer and means deselect.

#include <cstdint>

namespace geomsrv {
namespace archviz {

// `ids` is `width * height` element ids in row-major order -- the block read back
// around the cursor -- and (`centreX`, `centreY`) is where the CURSOR sits inside
// it, in texel coordinates (0 = the centre of the first texel). Returns the
// winning id, or 0 for background.
//
// ⚠️ THE CURSOR IS NOT ASSUMED TO BE AT THE BLOCK'S MIDDLE, and that is what the
// id G-buffer needs (PLAT-RE136). The readback box is CLAMPED to the viewport, so
// a click three pixels from the left edge yields a block whose middle is three
// pixels to the right of where the user is pointing. Voting around the block's
// middle there answers a question about a point the cursor is not on -- and it is
// worst exactly at the frame edges, which reads as "picking drifts near the
// edges" rather than as an off-by-a-clamp.
//
// A zero size, a null pointer or a centre outside the block are handled rather
// than asserted: this runs on the render thread, where a wrong answer is a
// misselection and an assert is a dead Archicad.
uint32_t ResolvePickIdAt (const uint32_t* ids, uint32_t width, uint32_t height, float centreX,
                          float centreY);

// The square, centred case: `size * size` ids with the cursor at the block's
// exact middle. Kept because the offline suite is written against it and because
// it is the only form a caller with a symmetric block ever wants.
uint32_t ResolvePickId (const uint32_t* ids, uint32_t size);

// Which box to read back around the cursor, and where the cursor lands inside
// it once that box has been clamped to the target.
//
// ⚠️ IT LIVES HERE, WITH THE VOTE, BECAUSE IT DECIDES CLICKS JUST AS MUCH AS THE
// VOTE DOES -- and it is the half that is wrong only near the frame's edges,
// where a human tester is least likely to look. It is pure integer arithmetic
// over the cursor and the target size: no GPU, no Diligent, so the offline suite
// covers it instead of a build/sync/restart round trip. The alternative, doing
// it inline in DiligentPickBuffer::Request, is how `centreX` and the clamp got
// to disagree in the first place.
//
// `valid` is false when the cursor is outside the target or the target is empty;
// there is no box and the caller has not lost a request it thinks is in flight.
// `centreX`/`centreY` are in the texel coordinates ResolvePickIdAt expects.
struct PickReadback {
    bool valid = false;
    uint32_t minX = 0;
    uint32_t minY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float centreX = 0.0f;
    float centreY = 0.0f;
};

PickReadback PlanPickReadback (int32_t px, int32_t py, uint32_t targetWidth,
                               uint32_t targetHeight, uint32_t boxSize);

}   // namespace archviz
}   // namespace geomsrv

#endif
