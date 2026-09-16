#ifndef EVP_ARCHVIZ_DXGI_INJECTIONORACLE_HPP
#define EVP_ARCHVIZ_DXGI_INJECTIONORACLE_HPP

// Stage 5's numeric witness: what the injected triangle's transform ACTUALLY
// produced, per Present, in numbers (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ THIS EXISTS BECAUSE VISUAL ITERATION HAS STOPPED BEING ABLE TO ANSWER THE
// QUESTION. Six runs have ended with "the triangle appears at rest and not in
// motion", and every one of them was compatible with at least three different
// causes: the wrong camera, a correct camera that rasterised nothing, or a
// correct rasterisation that something painted over. Those are three different
// investigations and the eye cannot tell them apart. This file makes each of
// them a number, so that ONE bad moving frame classifies itself.
//
// The three questions, in the order they have to be asked:
//
//   1. WHERE DOES THE ANCHOR PROJECT?  The anchor is the orbit target, so its
//      pixel must stay near the viewport centre for the whole gesture. If it
//      does not, the failure is before rasterisation and nothing about raster
//      state is worth reading.
//   2. DID ANYTHING RASTERISE?  An occlusion query around the magenta draw
//      alone. Depth is off, so a triangle whose anchor is inside the frustum
//      MUST produce samples. Zero samples with a correct projection means
//      viewport, scissor, input assembly or colour-write state.
//   3. IF BOTH ARE RIGHT AND IT IS STILL INVISIBLE, the triangle genuinely
//      rasterised into the buffer -- and the fault is presentation: the wrong
//      render target, a blend or write mask, or something overwriting it.
//
// ⚠️ DIAGNOSTIC ONLY, AND IT NEVER BLOCKS THE RENDER THREAD. The production
// path stays GPU-to-GPU: `SnapshotCamera` copies Archicad's ring window into a
// buffer we own and the shader reads that. This file additionally copies those
// snapshots into a small staging ring and reads an OLDER slot with
// `D3D11_MAP_FLAG_DO_NOT_WAIT`. A slot that is not ready yet is SKIPPED, never
// waited for. ⚠️ ARCHICAD'S OWN RING IS NEVER MAPPED -- mapping a buffer the GPU
// is using would stall Archicad's renderer, and a diagnostic that changes the
// timing of the thing it measures is worthless.
//
// ⚠️ THE READBACK LAGS AND THAT IS HARMLESS HERE. A row describes the snapshot
// it names, read a frame or two later. The orbit-target invariant does not care
// which frame it came from: during an orbit about the anchor, EVERY frame's
// projection of that anchor belongs near the viewport centre, so a lagged
// sample falsifies the transform just as well as a live one.
//
// THREAD SAFETY. Every `On*`/`Begin*`/`End*` runs on Archicad's render thread
// inside a detour, under `ScopedInjectionGuard`. `CopyRows` and `GetCounters`
// are the cross-thread reads and they take copies; a torn row costs one wrong
// line in a report and never a crash, which is the same trade the rest of this
// directory makes.

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Buffer;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace oracle {

// ⚠️ SIXTY-FOUR IS A CEILING ON MEMORY, NOT A SAMPLE PLAN. The ring keeps the
// most recent rows, and the most recent rows are the ones next to the moment the
// user says "it disappeared".
constexpr size_t kRowCapacity = 64;

// Which of the three frame states produced this row. Mirrors the classification
// in `InjectAtPresent` so a report never has to re-derive it.
enum class FrameState : uint32_t { NewScene = 0, RepeatScene = 1, InvalidScene = 2 };

// ⚠️ EIGHT INTERPRETATIONS OF THE SAME 128 BYTES, AND THEY EXHAUST THE QUESTION.
// Two orders times four transpose combinations:
//
//   0  p*(V*P)      1  p*(Vt*P)      2  p*(V*Pt)      3  p*(Vt*Pt)
//   4  p*(P*V)      5  p*(Pt*V)      6  p*(P*Vt)      7  p*(Pt*Vt)
//
// ⚠️ THE COLUMN-VECTOR READINGS ARE ALREADY IN THIS TABLE AND DO NOT NEED A
// NINTH ENTRY. Because `p*Mt == (M*pt)t`, the column-vector product `P*V*p` is
// entry 3 and `V*P*p` is entry 7 -- transposing both halves of a row-vector
// product IS the column-vector convention with the order reversed. So these
// eight cover every combination of vector convention and multiplication order
// that the two captured matrices admit.
constexpr size_t kVariantCount = 8;

// One accumulated verdict per interpretation, over the rows that qualify:
// NEW_SCENE, a readback that actually landed, and a camera that MOVED since the
// last accepted sample.
//
// ⚠️ THE FILTER IS THE WHOLE POINT. Run twenty-eight's verdict was decided by
// the frames after the gesture stopped -- at rest the anchor projected to the
// centre and the occlusion query counted 5676 samples, on every one of the last
// nine Presents, from the same snapshot. Those rows outvoted the moving ones and
// reported success for a run in which nothing was visible while moving. A
// diagnostic that is dominated by the condition it is not testing is worse than
// none.
struct VariantStats {
    uint32_t rowsTested = 0;
    uint32_t rowsInsideClip = 0;   // rows with a VALID homogeneous projection
    uint32_t rowsWon = 0;          // this variant was the closest to centre
    float    medianCentreError = 0.0f;
    float    worstCentreError = 0.0f;
    float    meanCentreError = 0.0f;
};

// ⚠️ A CENTRE ERROR COMPUTED FROM AN INVALID PROJECTION IS NOT A SMALL ERROR, IT
// IS A MEANINGLESS ONE -- and ranking by it produced the worst reading yet. Run
// thirty ranked `p*Vt*Pt` first on a median of 0.000 with **0% of its anchors
// inside the clip volume**: a degenerate product whose w collapses puts x/y at
// 0/0, which reads as dead centre and is nothing of the kind. So validity is a
// GATE and not a column:
//
//     finite clip, |w| > epsilon, 0 <= z <= w, |x| <= w and |y| <= w
//
// Only samples that pass it contribute an error at all, and a variant with no
// valid samples can never win no matter what its arithmetic says.
struct VariantScore {
    bool  computed = false;          // the product was finite and w was usable
    bool  validProjection = false;   // ... and it passed the whole gate above
    float centreError = 0.0f;        // fraction of the viewport half-extent

    // ⚠️ HOW BIG THE PRIMITIVE ACTUALLY IS ON SCREEN, AND WITHOUT IT THE WHOLE
    // SCORE IS DEGENERATE. Run thirty-six's occlusion queries settled this:
    // probe B and probe C each produced **one sample per draw** while the
    // clip-space probe produced twenty-two thousand. The transform was putting
    // the anchor at the viewport centre and COLLAPSING EVERY OTHER POINT ONTO IT
    // -- and a one-point "is the anchor near the centre" test is passed
    // perfectly by a matrix that maps all of space to a point. The interpretation
    // that wins such a test can be the one that draws nothing.
    //
    // So all three triangle vertices are projected and this is the diagonal of
    // their pixel bounding box. A transform that squashes the primitive below a
    // few pixels is REFUSED however well-centred its anchor is.
    float spreadPixels = 0.0f;

    // ⚠️ THE TRIANGLE ITSELF, MEASURED. A candidate is invalid if the primitive
    // collapses EVEN IF ITS ANCHOR SITS PERFECTLY AT THE CENTRE -- which is the
    // whole lesson of runs thirty to thirty-six, where a transform that mapped
    // all of space onto the viewport centre scored 0.002 for six runs and drew
    // one pixel. Area and the longest edge are what a collapse cannot fake.
    bool  verticesFinite = false;
    float areaPixels = 0.0f;
    float minEdgePixels = 0.0f;
    float maxEdgePixels = 0.0f;
    float ndcX = 0.0f, ndcY = 0.0f, ndcZ = 0.0f, clipW = 0.0f;
    float pixelX = 0.0f, pixelY = 0.0f;
};

struct ViewportRect {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

// ⚠️ ONE DEFINITION OF THE SCORING, USED BY THE ORACLE AND BY THE CAMERA CENSUS.
// Two copies of this arithmetic would eventually disagree about which
// interpretation won, which is the one question both of them exist to answer.
// The anchor is the point set by `SetAnchor`.
void ScoreVariants (const float view[16], const float projection[16],
                    const ViewportRect& viewport, VariantScore out[kVariantCount]);

struct Row {
    uint64_t present = 0;
    uint64_t modelSceneGeneration = 0;
    uint64_t snapshotSequence = 0;    // which camera snapshot this row's numbers came from
    uint64_t snapshotDrawSequence = 0;
    uint32_t state = 0;               // FrameState

    // ⚠️ WHERE THIS ROW'S CAMERA CAME FROM, CARRIED PER ROW. An accidental
    // fallback from the selected group to the learner would otherwise hide
    // inside an aggregate that looks healthy -- and a fallback that fires
    // occasionally is indistinguishable from the bug it replaced.
    uint32_t cameraSource = 0;        // injection::CameraSource
    uint32_t selectedGroupId = 0;
    uint64_t selectedGroupSnapshotGeneration = 0;
    uint64_t sourceDrawSequence = 0;
    uint64_t sourceModelGeneration = 0;

    // ⚠️ WHERE THE BYTES CAME FROM, CARRIED PER ROW. If two rows disagree about
    // the source window then the pass selection moved, and that is a different
    // fault from a wrong matrix in a stable window.
    uint64_t viewBuffer = 0;
    uint32_t viewFirstConstant = 0;
    uint32_t viewNumConstants = 0;
    uint64_t projectionBuffer = 0;
    uint32_t projectionFirstConstant = 0;
    uint32_t projectionNumConstants = 0;

    // ---- question 1: where does the anchor project? ------------------------
    // Computed with the convention the injected shader itself uses: row-major
    // storage, row-vector multiplication, world on the left. Anything else here
    // would be a second opinion about the matrix rather than a measurement of
    // what the GPU was told.
    bool  matricesRead = false;
    float clipX = 0.0f, clipY = 0.0f, clipZ = 0.0f, clipW = 0.0f;
    float ndcX = 0.0f, ndcY = 0.0f, ndcZ = 0.0f;
    float pixelX = 0.0f, pixelY = 0.0f;
    bool  insideClipVolume = false;

    float viewportX = 0.0f, viewportY = 0.0f;
    float viewportWidth = 0.0f, viewportHeight = 0.0f;

    // ⚠️ AND WHICH INTERPRETATION WOULD HAVE BEEN RIGHT, out of the eight. See
    // `kVariantCount`. This is the row's own winner; the RUN's answer is the
    // accumulated table, because one row can be won by chance and a hundred
    // cannot.
    uint32_t bestVariant = 0;
    float    bestVariantPixelX = 0.0f;
    float    bestVariantPixelY = 0.0f;
    float    bestVariantCentreError = 0.0f;   // pixels from the viewport centre

    // ---- question 2: did anything rasterise? -------------------------------
    bool     drawIssued = false;
    bool     samplesKnown = false;
    uint64_t samplesPassed = 0;
};

struct Counters {
    uint64_t rowsCompleted = 0;
    uint64_t rowsDroppedUnresolved = 0;

    // Readback health. `stillDrawing` is normal and benign -- it is the price of
    // never waiting. `stale` means the staging slot was recycled before it could
    // be read, which is the signal to deepen the ring and not a camera fault.
    uint64_t readbacksAttempted = 0;
    uint64_t readbacksServed = 0;
    uint64_t readbacksStillDrawing = 0;
    uint64_t readbacksStale = 0;

    uint64_t queriesIssued = 0;
    uint64_t queriesResolved = 0;
    uint64_t queriesUnavailable = 0;

    uint64_t snapshotsStaged = 0;

    // ⚠️ HOW MANY ROWS THE VARIANT TABLE IS ACTUALLY BUILT FROM. A table with
    // four rows behind it is not evidence, and a report that does not say so
    // invites the same mistake the rest-frame contamination already caused once.
    uint64_t rowsQualified = 0;
    uint64_t rowsRejectedNotNew = 0;
    uint64_t rowsRejectedStill = 0;   // the camera had not moved since the last
    bool     ready = false;
};

// MAIN THREAD. The world point whose projection every row reports. During an
// orbit this must be the orbit target, which is what makes the row a test
// rather than an observation.
// The size argument matters as much as the point: the scorer projects the whole
// primitive, not one corner of it. See `VariantScore::spreadPixels`.
void SetAnchor (float x, float y, float z, float sizeMetres);

// RENDER THREAD, under the injection guard, immediately after `SnapshotCamera`
// has copied both windows. Stages the same 256-byte copies for readback.
void OnSnapshot (ID3D11DeviceContext* context, ID3D11Buffer* viewSnapshot,
                 ID3D11Buffer* projectionSnapshot, uint64_t snapshotSequence,
                 uint64_t modelSceneGeneration, uint64_t drawSequence,
                 uint64_t viewBuffer, uint32_t viewFirstConstant, uint32_t viewNumConstants,
                 uint64_t projectionBuffer, uint32_t projectionFirstConstant,
                 uint32_t projectionNumConstants, float viewportX, float viewportY,
                 float viewportWidth, float viewportHeight);

// RENDER THREAD. Open a row for the Present about to draw, close it afterwards.
// `EndPresent` is what polls older slots and queries, so it must be called even
// on a Present whose own numbers are not ready.
void BeginPresent (uint64_t present, FrameState state, uint64_t modelSceneGeneration,
                   uint32_t cameraSource, uint32_t selectedGroupId,
                   uint64_t selectedGroupSnapshotGeneration, uint64_t sourceDrawSequence,
                   uint64_t sourceModelGeneration);
void EndPresent (ID3D11DeviceContext* context);

// RENDER THREAD. Wrap the WORLD-SPACE draw only. The clip-space probe is not
// measured: it has never been in doubt and counting its samples would only
// dilute the number that matters.
void BeginTriangleQuery (ID3D11DeviceContext* context);
void EndTriangleQuery (ID3D11DeviceContext* context);

// MAIN THREAD. Clear the accumulated variant table and its filters, WITHOUT
// touching the device objects or the row ring. Called immediately before the
// navigation interval begins, so the table describes that interval and nothing
// before it.
void ResetStatistics ();

// MAIN THREAD, at teardown. Nothing here may outlive Archicad's device.
void Shutdown ();

// ANY THREAD. Newest row last.
size_t   CopyRows (Row* out, size_t capacity);
size_t   CopyVariants (VariantStats* out, size_t capacity);
Counters GetCounters ();

}   // namespace oracle
}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
