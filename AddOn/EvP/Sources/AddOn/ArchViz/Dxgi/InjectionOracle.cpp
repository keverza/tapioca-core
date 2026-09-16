// ArchViz/Dxgi/InjectionOracle -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/InjectionOracle.hpp"

#include <d3d11_1.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace oracle {

namespace {

// ⚠️ THE STAGING RING HAS TO OUTLIVE THE GPU'S LAG OR IT MEASURES NOTHING. A
// slot is written on every qualifying camera draw -- several per frame -- and can
// only be read once the GPU has actually performed the copy, one to two frames
// later. Too few slots and every read finds its slot already recycled, which
// reports as `readbacksStale` rather than as data.
constexpr size_t kStagingDepth = 8;
constexpr size_t kPendingCapacity = 8;
constexpr size_t kQueryCount = 8;
constexpr UINT   kWindowBytes = 256;

// A row that has waited this many Presents for its GPU results is committed with
// whatever it has. ⚠️ A ROW THAT NEVER COMMITS IS WORSE THAN AN INCOMPLETE ONE:
// the rows nobody can see are exactly the frames the user is complaining about.
constexpr uint64_t kMaxRowAgePresents = 30;

struct SnapshotIdentity {
    uint64_t sequence = 0;
    uint64_t modelSceneGeneration = 0;
    uint64_t drawSequence = 0;
    uint64_t viewBuffer = 0;
    uint32_t viewFirstConstant = 0;
    uint32_t viewNumConstants = 0;
    uint64_t projectionBuffer = 0;
    uint32_t projectionFirstConstant = 0;
    uint32_t projectionNumConstants = 0;
    float viewportX = 0.0f;
    float viewportY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
};

struct StagingSlot {
    ID3D11Buffer* view = nullptr;
    ID3D11Buffer* projection = nullptr;
    SnapshotIdentity identity;
    bool written = false;
};

struct Pending {
    bool active = false;
    bool closed = false;          // the Present that opened it has finished drawing
    bool matricesDone = false;
    bool queryDone = false;
    size_t slot = 0;
    uint64_t slotSequence = 0;
    int query = -1;
    uint64_t openedAtPresent = 0;
    Row row;
};

ID3D11Device* g_device = nullptr;
StagingSlot   g_staging[kStagingDepth];
ID3D11Query*  g_queries[kQueryCount] = {};
bool          g_queryBusy[kQueryCount] = {};
Pending       g_pending[kPendingCapacity];

SnapshotIdentity g_lastStaged;
size_t   g_lastStagedSlot = 0;
bool     g_haveStaged = false;
size_t   g_nextSlot = 0;
int      g_openPending = -1;
uint64_t g_presentCounter = 0;

float g_anchorX = 0.0f;
float g_anchorY = 0.0f;
float g_anchorZ = 0.0f;
float g_anchorSize = 1.0f;

// ⚠️ THE SMALLEST PRIMITIVE WORTH CALLING RENDERED. Below this the transform has
// collapsed the triangle to a point: run thirty-six measured exactly ONE sample
// per draw from a two-metre triangle whose anchor scored 0.002 from the centre.
constexpr float kMinSpreadPixels = 6.0f;

bool g_created = false;
bool g_createFailed = false;

Row      g_rows[kRowCapacity];
std::atomic<uint64_t> g_rowsWritten {0};
Counters g_counters;

// ⚠️ THE VARIANT TABLE IS THE RUN'S ANSWER; A ROW IS ONLY A SAMPLE. One row can
// be won by an interpretation through sheer luck -- a garbage matrix that lands
// near the centre once -- and a hundred cannot.
constexpr size_t kErrorSamples = 256;
VariantStats g_variants[kVariantCount];
float    g_errors[kVariantCount][kErrorSamples] = {};
uint32_t g_errorCount[kVariantCount] = {};
uint32_t g_errorNext[kVariantCount] = {};
double   g_errorSum[kVariantCount] = {};

// ⚠️ "MOVING" IS DEFINED BY THE MATRIX, NOT BY A CLOCK. A camera identical to the
// last accepted one is a frame at rest no matter when it arrived, and those are
// exactly the frames that outvoted the moving ones in run twenty-eight. Comparing
// the 128 bytes is the whole test: no wall time, no heuristic, no gesture
// detection.
float g_previousView[16] = {};
float g_previousProjection[16] = {};
bool  g_havePrevious = false;

template <typename T>
void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

// ---- matrix helpers --------------------------------------------------------
// ⚠️ ROW-MAJOR STORAGE, ROW-VECTOR MULTIPLICATION, WORLD ON THE LEFT -- the
// convention the injected shader declares with `row_major` and uses with
// `mul (p, View)`. This is deliberately not a second opinion about the matrix:
// it reproduces exactly what the GPU was told to do, so a disagreement between
// this number and the screen is a fact about the pipeline and not about the
// arithmetic.

void Transpose (float out[16], const float m[16])
{
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            out[row * 4 + column] = m[column * 4 + row];
    }
}

void Multiply (float out[16], const float a[16], const float b[16])
{
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[row * 4 + k] * b[k * 4 + column];
            out[row * 4 + column] = sum;
        }
    }
}

void TransformPoint (float out[4], const float point[4], const float m[16])
{
    for (int column = 0; column < 4; ++column) {
        float sum = 0.0f;
        for (int row = 0; row < 4; ++row)
            sum += point[row] * m[row * 4 + column];
        out[column] = sum;
    }
}

bool Finite (const float m[16])
{
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite (m[i]))
            return false;
    }
    return true;
}

bool ProjectAnchor (const float view[16], const float projection[16], float clip[4])
{
    if (!Finite (view) || !Finite (projection))
        return false;
    float viewProjection[16];
    Multiply (viewProjection, view, projection);
    if (!Finite (viewProjection))
        return false;
    const float anchor[4] = { g_anchorX, g_anchorY, g_anchorZ, 1.0f };
    TransformPoint (clip, anchor, viewProjection);
    return std::isfinite (clip[0]) && std::isfinite (clip[1]) &&
           std::isfinite (clip[2]) && std::isfinite (clip[3]);
}

// NDC to pixels, with Y running down the screen as D3D's viewport transform
// does. The viewport is the one the model draw itself used, not the swap chain's
// -- if those two differ, that difference is a finding and not a rounding error.
void PixelFromClip (const float clip[4], const SnapshotIdentity& id, float& pixelX,
                    float& pixelY)
{
    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    pixelX = id.viewportX + (ndcX * 0.5f + 0.5f) * id.viewportWidth;
    pixelY = id.viewportY + (0.5f - ndcY * 0.5f) * id.viewportHeight;
}

// Build one interpretation of the pair. See `kVariantCount` for why eight
// exhausts the question.
void ComposeVariant (float out[16], const float view[16], const float projection[16],
                     uint32_t variant)
{
    float first[16];
    float second[16];
    const bool reversedOrder = (variant & 4u) != 0;
    const float* left = reversedOrder ? projection : view;
    const float* right = reversedOrder ? view : projection;
    const uint32_t leftTransposed = reversedOrder ? (variant & 2u) : (variant & 1u);
    const uint32_t rightTransposed = reversedOrder ? (variant & 1u) : (variant & 2u);

    if (leftTransposed != 0)
        Transpose (first, left);
    else
        std::memcpy (first, left, sizeof (first));
    if (rightTransposed != 0)
        Transpose (second, right);
    else
        std::memcpy (second, right, sizeof (second));
    Multiply (out, first, second);
}

// True when this row belongs in the variant table: a new model scene, and a
// camera that actually moved since the last row accepted into it.
bool QualifiesForTable (const Row& row, const float view[16], const float projection[16])
{
    if (row.state != uint32_t (FrameState::NewScene)) {
        ++g_counters.rowsRejectedNotNew;
        return false;
    }
    if (g_havePrevious) {
        bool moved = false;
        for (int i = 0; i < 16 && !moved; ++i) {
            if (std::fabs (view[i] - g_previousView[i]) > 1e-6f ||
                std::fabs (projection[i] - g_previousProjection[i]) > 1e-6f)
                moved = true;
        }
        if (!moved) {
            ++g_counters.rowsRejectedStill;
            return false;
        }
    }
    std::memcpy (g_previousView, view, sizeof (g_previousView));
    std::memcpy (g_previousProjection, projection, sizeof (g_previousProjection));
    g_havePrevious = true;
    ++g_counters.rowsQualified;
    return true;
}

// ⚠️ AN INVALID PROJECTION CONTRIBUTES NO ERROR AT ALL, it only contributes an
// attempt. See `VariantScore`: a degenerate product puts x/y at 0/0 and would
// otherwise report a median of 0.000 while being outside the clip volume on
// every single sample.
void RecordVariantSample (size_t variant, const VariantScore& score)
{
    VariantStats& stats = g_variants[variant];
    ++stats.rowsTested;
    if (!score.validProjection)
        return;
    ++stats.rowsInsideClip;
    if (score.centreError > stats.worstCentreError)
        stats.worstCentreError = score.centreError;
    g_errorSum[variant] += double (score.centreError);
    g_errors[variant][g_errorNext[variant]] = score.centreError;
    g_errorNext[variant] = (g_errorNext[variant] + 1) % kErrorSamples;
    if (g_errorCount[variant] < kErrorSamples)
        ++g_errorCount[variant];
}

bool EnsureCreated (ID3D11DeviceContext* context)
{
    if (g_created)
        return true;
    if (g_createFailed || context == nullptr)
        return false;

    context->GetDevice (&g_device);
    if (g_device == nullptr) {
        g_createFailed = true;
        return false;
    }

    // ⚠️ STAGING, CPU READ, NO BIND FLAGS. These are never bound to the pipeline
    // and never written by the CPU: the GPU copies into them and the CPU reads
    // them back when -- and only when -- the copy has landed.
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kWindowBytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    bool ok = true;
    for (size_t i = 0; i < kStagingDepth; ++i) {
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &g_staging[i].view));
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &g_staging[i].projection));
    }

    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kQueryCount; ++i)
        ok = ok && SUCCEEDED (g_device->CreateQuery (&queryDesc, &g_queries[i]));

    if (!ok) {
        g_createFailed = true;
        return false;
    }
    g_created = true;
    g_counters.ready = true;
    return true;
}

int AcquireQuery ()
{
    for (size_t i = 0; i < kQueryCount; ++i) {
        if (!g_queryBusy[i]) {
            g_queryBusy[i] = true;
            return int (i);
        }
    }
    return -1;
}

void CommitRow (Pending& pending)
{
    const uint64_t written = g_rowsWritten.load (std::memory_order_relaxed);
    g_rows[written % kRowCapacity] = pending.row;
    g_rowsWritten.store (written + 1, std::memory_order_release);
    ++g_counters.rowsCompleted;
    if (!pending.matricesDone || !pending.queryDone)
        ++g_counters.rowsDroppedUnresolved;
    if (pending.query >= 0)
        g_queryBusy[pending.query] = false;
    pending = Pending {};
}

// Read one staging slot if the GPU has finished with it. Never waits.
bool TryReadMatrices (ID3D11DeviceContext* context, Pending& pending)
{
    StagingSlot& slot = g_staging[pending.slot];
    ++g_counters.readbacksAttempted;

    // ⚠️ THE SLOT MAY HAVE BEEN RECYCLED UNDER US, and that has to be detected
    // rather than read. Reading a slot whose sequence has moved on would produce
    // a perfectly plausible matrix belonging to a different draw -- the exact
    // class of error this whole rung exists to eliminate.
    if (slot.identity.sequence != pending.slotSequence) {
        ++g_counters.readbacksStale;
        pending.row.matricesRead = false;
        return true;
    }

    D3D11_MAPPED_SUBRESOURCE viewMap = {};
    D3D11_MAPPED_SUBRESOURCE projectionMap = {};
    HRESULT hr = context->Map (slot.view, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
                               &viewMap);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++g_counters.readbacksStillDrawing;
        return false;
    }
    if (FAILED (hr) || viewMap.pData == nullptr)
        return false;
    hr = context->Map (slot.projection, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
                       &projectionMap);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING || FAILED (hr) || projectionMap.pData == nullptr) {
        context->Unmap (slot.view, 0);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            ++g_counters.readbacksStillDrawing;
        return false;
    }

    float view[16];
    float projection[16];
    std::memcpy (view, viewMap.pData, sizeof (view));
    std::memcpy (projection, projectionMap.pData, sizeof (projection));
    context->Unmap (slot.projection, 0);
    context->Unmap (slot.view, 0);
    ++g_counters.readbacksServed;

    Row& row = pending.row;
    float clip[4] = {};
    if (!ProjectAnchor (view, projection, clip) || !(std::fabs (clip[3]) > 1e-9f)) {
        row.matricesRead = false;
        return true;
    }
    row.matricesRead = true;
    row.clipX = clip[0];
    row.clipY = clip[1];
    row.clipZ = clip[2];
    row.clipW = clip[3];
    row.ndcX = clip[0] / clip[3];
    row.ndcY = clip[1] / clip[3];
    row.ndcZ = clip[2] / clip[3];
    PixelFromClip (clip, slot.identity, row.pixelX, row.pixelY);
    row.insideClipVolume =
            clip[3] > 0.0f &&
            std::fabs (clip[0]) <= clip[3] && std::fabs (clip[1]) <= clip[3] &&
            clip[2] >= 0.0f && clip[2] <= clip[3];

    // ⚠️ ALL EIGHT INTERPRETATIONS, SCORED AGAINST THE VIEWPORT CENTRE, AND ONLY
    // ON ROWS THAT WERE MOVING. During an orbit the anchor IS the orbit target,
    // so the interpretation that keeps it at the centre THROUGH THE GESTURE is
    // the correct one. A row at rest proves nothing here -- at rest the model is
    // not being re-rendered and any interpretation that was ever right stays
    // right -- which is precisely how run twenty-eight reported success for a run
    // in which nothing was visible while moving.
    const bool qualifies = QualifiesForTable (row, view, projection);

    ViewportRect viewport;
    viewport.x = slot.identity.viewportX;
    viewport.y = slot.identity.viewportY;
    viewport.width = slot.identity.viewportWidth;
    viewport.height = slot.identity.viewportHeight;

    VariantScore scores[kVariantCount];
    ScoreVariants (view, projection, viewport, scores);

    float bestError = -1.0f;
    bool haveWinner = false;
    for (uint32_t variant = 0; variant < uint32_t (kVariantCount); ++variant) {
        if (qualifies)
            RecordVariantSample (variant, scores[variant]);
        // ⚠️ ONLY A VALID PROJECTION MAY WIN A ROW, for the same reason it is the
        // only one that may contribute an error.
        if (!scores[variant].validProjection)
            continue;
        if (!haveWinner || scores[variant].centreError < bestError) {
            haveWinner = true;
            bestError = scores[variant].centreError;
            row.bestVariant = variant;
            row.bestVariantPixelX = scores[variant].pixelX;
            row.bestVariantPixelY = scores[variant].pixelY;
            row.bestVariantCentreError = scores[variant].centreError;
        }
    }
    if (qualifies && haveWinner)
        ++g_variants[row.bestVariant].rowsWon;
    return true;
}

bool TryReadQuery (ID3D11DeviceContext* context, Pending& pending)
{
    if (pending.query < 0) {
        ++g_counters.queriesUnavailable;
        return true;
    }
    UINT64 samples = 0;
    // ⚠️ `DO_NOT_FLUSH`: asking the driver to flush here would change Archicad's
    // submission pattern to answer a question about Archicad's submission
    // pattern. S_FALSE simply means "not yet" and the row waits.
    const HRESULT hr = context->GetData (g_queries[pending.query], &samples, sizeof (samples),
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr != S_OK)
        return false;
    pending.row.samplesKnown = true;
    pending.row.samplesPassed = uint64_t (samples);
    ++g_counters.queriesResolved;
    return true;
}

void ResolvePending (ID3D11DeviceContext* context)
{
    for (size_t i = 0; i < kPendingCapacity; ++i) {
        Pending& pending = g_pending[i];
        if (!pending.active || !pending.closed)
            continue;
        if (!pending.matricesDone)
            pending.matricesDone = TryReadMatrices (context, pending);
        if (!pending.queryDone)
            pending.queryDone = TryReadQuery (context, pending);
        const bool aged = g_presentCounter - pending.openedAtPresent > kMaxRowAgePresents;
        if ((pending.matricesDone && pending.queryDone) || aged)
            CommitRow (pending);
    }
}

}   // namespace

void ScoreVariants (const float view[16], const float projection[16],
                    const ViewportRect& viewport, VariantScore out[kVariantCount])
{
    // ⚠️ THE WHOLE PRIMITIVE, NOT ONE CORNER OF IT. These are the three vertices
    // the production vertex buffer holds, and projecting all of them is what
    // makes a collapsing transform fail instead of winning.
    const float corners[3][4] = {
        { g_anchorX, g_anchorY, g_anchorZ, 1.0f },
        { g_anchorX + g_anchorSize, g_anchorY, g_anchorZ, 1.0f },
        { g_anchorX, g_anchorY, g_anchorZ + g_anchorSize, 1.0f },
    };
    const float* const anchor = corners[0];
    for (size_t variant = 0; variant < kVariantCount; ++variant) {
        out[variant] = VariantScore {};
        float composed[16];
        ComposeVariant (composed, view, projection, uint32_t (variant));
        if (!Finite (composed))
            continue;

        float clip[4] = {};
        TransformPoint (clip, anchor, composed);
        bool finite = true;
        for (int i = 0; i < 4; ++i)
            finite = finite && std::isfinite (clip[i]);
        if (!finite || !(std::fabs (clip[3]) > 1e-9f))
            continue;

        VariantScore& score = out[variant];
        score.computed = true;
        score.clipW = clip[3];
        score.ndcX = clip[0] / clip[3];
        score.ndcY = clip[1] / clip[3];
        score.ndcZ = clip[2] / clip[3];
        score.pixelX = viewport.x + (score.ndcX * 0.5f + 0.5f) * viewport.width;
        score.pixelY = viewport.y + (0.5f - score.ndcY * 0.5f) * viewport.height;

        // ⚠️ THE WHOLE GATE, NOT PART OF IT. A point behind the eye, a collapsed
        // w, a clip z outside the depth range, or an x/y outside the frustum all
        // mean the same thing: this interpretation did not put the anchor on the
        // screen, and its distance from the centre is not evidence about anything.
        const bool inside =
                clip[3] > 0.0f &&
                std::fabs (clip[0]) <= clip[3] && std::fabs (clip[1]) <= clip[3] &&
                clip[2] >= 0.0f && clip[2] <= clip[3];
        if (!inside)
            continue;
        if (viewport.width < 2.0f || viewport.height < 2.0f)
            continue;

        // ⚠️ AND IS IT BIG ENOUGH TO BE A TRIANGLE? The other two vertices are
        // projected here, and a primitive squashed below a few pixels is refused
        // however perfectly its anchor sits at the centre. This is the test run
        // thirty-six proved was missing: one sample per draw from a two-metre
        // triangle, under a transform that scored 0.002.
        float minX = score.pixelX;
        float maxX = score.pixelX;
        float minY = score.pixelY;
        float maxY = score.pixelY;
        float px[3] = { score.pixelX, 0.0f, 0.0f };
        float py[3] = { score.pixelY, 0.0f, 0.0f };
        bool cornersUsable = true;
        for (int corner = 1; corner < 3 && cornersUsable; ++corner) {
            float cornerClip[4] = {};
            TransformPoint (cornerClip, corners[corner], composed);
            for (int i = 0; i < 4; ++i)
                cornersUsable = cornersUsable && std::isfinite (cornerClip[i]);
            if (!cornersUsable || !(cornerClip[3] > 1e-9f)) {
                cornersUsable = false;
                break;
            }
            const float ndcX = cornerClip[0] / cornerClip[3];
            const float ndcY = cornerClip[1] / cornerClip[3];
            const float cornerX = viewport.x + (ndcX * 0.5f + 0.5f) * viewport.width;
            const float cornerY = viewport.y + (0.5f - ndcY * 0.5f) * viewport.height;
            px[corner] = cornerX;
            py[corner] = cornerY;
            minX = cornerX < minX ? cornerX : minX;
            maxX = cornerX > maxX ? cornerX : maxX;
            minY = cornerY < minY ? cornerY : minY;
            maxY = cornerY > maxY ? cornerY : maxY;
        }
        if (!cornersUsable)
            continue;
        score.verticesFinite = true;
        const float spreadX = maxX - minX;
        const float spreadY = maxY - minY;
        score.spreadPixels = std::sqrt (spreadX * spreadX + spreadY * spreadY);

        // ⚠️ AREA AND THE LONGEST EDGE, BECAUSE A BOUNDING BOX ALONE CAN BE
        // FOOLED. Three collinear points span a box and enclose nothing; the
        // cross product is what says whether there is a triangle there at all.
        const float ax = px[1] - px[0];
        const float ay = py[1] - py[0];
        const float bx = px[2] - px[0];
        const float by = py[2] - py[0];
        score.areaPixels = std::fabs (ax * by - ay * bx) * 0.5f;
        float edges[3];
        edges[0] = std::sqrt (ax * ax + ay * ay);
        edges[1] = std::sqrt (bx * bx + by * by);
        const float cx = px[2] - px[1];
        const float cy = py[2] - py[1];
        edges[2] = std::sqrt (cx * cx + cy * cy);
        score.minEdgePixels = edges[0];
        score.maxEdgePixels = edges[0];
        for (int i = 1; i < 3; ++i) {
            score.minEdgePixels = edges[i] < score.minEdgePixels ? edges[i]
                                                                 : score.minEdgePixels;
            score.maxEdgePixels = edges[i] > score.maxEdgePixels ? edges[i]
                                                                 : score.maxEdgePixels;
        }
        if (score.spreadPixels < kMinSpreadPixels)
            continue;

        score.validProjection = true;
        const float centreX = viewport.x + viewport.width * 0.5f;
        const float centreY = viewport.y + viewport.height * 0.5f;
        const float dx = (score.pixelX - centreX) / (viewport.width * 0.5f);
        const float dy = (score.pixelY - centreY) / (viewport.height * 0.5f);
        score.centreError = std::fabs (dx) > std::fabs (dy) ? std::fabs (dx)
                                                            : std::fabs (dy);
    }
}

void SetAnchor (float x, float y, float z, float sizeMetres)
{
    g_anchorX = x;
    g_anchorY = y;
    g_anchorZ = z;
    g_anchorSize = (sizeMetres > 0.001f) ? sizeMetres : 1.0f;
}

void OnSnapshot (ID3D11DeviceContext* context, ID3D11Buffer* viewSnapshot,
                 ID3D11Buffer* projectionSnapshot, uint64_t snapshotSequence,
                 uint64_t modelSceneGeneration, uint64_t drawSequence,
                 uint64_t viewBuffer, uint32_t viewFirstConstant, uint32_t viewNumConstants,
                 uint64_t projectionBuffer, uint32_t projectionFirstConstant,
                 uint32_t projectionNumConstants, float viewportX, float viewportY,
                 float viewportWidth, float viewportHeight)
{
    if (context == nullptr || viewSnapshot == nullptr || projectionSnapshot == nullptr)
        return;
    if (!EnsureCreated (context))
        return;

    StagingSlot& slot = g_staging[g_nextSlot];
    // ⚠️ THE SNAPSHOTS ARE COPIED, NOT ARCHICAD'S RING. Our own 256-byte buffers
    // already hold exactly the bytes the model draw consumed, so the diagnostic
    // reads a copy of a copy and Archicad's allocation is never touched by the
    // CPU at all.
    context->CopyResource (slot.view, viewSnapshot);
    context->CopyResource (slot.projection, projectionSnapshot);

    slot.identity.sequence = snapshotSequence;
    slot.identity.modelSceneGeneration = modelSceneGeneration;
    slot.identity.drawSequence = drawSequence;
    slot.identity.viewBuffer = viewBuffer;
    slot.identity.viewFirstConstant = viewFirstConstant;
    slot.identity.viewNumConstants = viewNumConstants;
    slot.identity.projectionBuffer = projectionBuffer;
    slot.identity.projectionFirstConstant = projectionFirstConstant;
    slot.identity.projectionNumConstants = projectionNumConstants;
    slot.identity.viewportX = viewportX;
    slot.identity.viewportY = viewportY;
    slot.identity.viewportWidth = viewportWidth;
    slot.identity.viewportHeight = viewportHeight;
    slot.written = true;

    g_lastStaged = slot.identity;
    g_lastStagedSlot = g_nextSlot;
    g_haveStaged = true;
    g_nextSlot = (g_nextSlot + 1) % kStagingDepth;
    ++g_counters.snapshotsStaged;
}

void BeginPresent (uint64_t present, FrameState state, uint64_t modelSceneGeneration,
                   uint32_t cameraSource, uint32_t selectedGroupId,
                   uint64_t selectedGroupSnapshotGeneration, uint64_t sourceDrawSequence,
                   uint64_t sourceModelGeneration)
{
    g_presentCounter = present;
    g_openPending = -1;
    if (!g_created || !g_haveStaged)
        return;

    for (size_t i = 0; i < kPendingCapacity; ++i) {
        if (g_pending[i].active)
            continue;
        Pending& pending = g_pending[i];
        pending = Pending {};
        pending.active = true;
        pending.slot = g_lastStagedSlot;
        pending.slotSequence = g_lastStaged.sequence;
        pending.openedAtPresent = present;
        pending.query = AcquireQuery ();

        Row& row = pending.row;
        row.present = present;
        row.modelSceneGeneration = modelSceneGeneration;
        row.snapshotSequence = g_lastStaged.sequence;
        row.snapshotDrawSequence = g_lastStaged.drawSequence;
        row.state = uint32_t (state);
        row.cameraSource = cameraSource;
        row.selectedGroupId = selectedGroupId;
        row.selectedGroupSnapshotGeneration = selectedGroupSnapshotGeneration;
        row.sourceDrawSequence = sourceDrawSequence;
        row.sourceModelGeneration = sourceModelGeneration;
        row.viewBuffer = g_lastStaged.viewBuffer;
        row.viewFirstConstant = g_lastStaged.viewFirstConstant;
        row.viewNumConstants = g_lastStaged.viewNumConstants;
        row.projectionBuffer = g_lastStaged.projectionBuffer;
        row.projectionFirstConstant = g_lastStaged.projectionFirstConstant;
        row.projectionNumConstants = g_lastStaged.projectionNumConstants;
        row.viewportX = g_lastStaged.viewportX;
        row.viewportY = g_lastStaged.viewportY;
        row.viewportWidth = g_lastStaged.viewportWidth;
        row.viewportHeight = g_lastStaged.viewportHeight;
        g_openPending = int (i);
        return;
    }
    // Every slot is still waiting on the GPU. Not an error: the next Present
    // will have room, and `rowsDroppedUnresolved` says if that is chronic.
}

void BeginTriangleQuery (ID3D11DeviceContext* context)
{
    if (context == nullptr || g_openPending < 0)
        return;
    Pending& pending = g_pending[g_openPending];
    if (pending.query < 0)
        return;
    context->Begin (g_queries[pending.query]);
}

void EndTriangleQuery (ID3D11DeviceContext* context)
{
    if (context == nullptr || g_openPending < 0)
        return;
    Pending& pending = g_pending[g_openPending];
    pending.row.drawIssued = true;
    if (pending.query < 0) {
        pending.queryDone = true;   // nothing to wait for
        return;
    }
    context->End (g_queries[pending.query]);
    ++g_counters.queriesIssued;
}

void EndPresent (ID3D11DeviceContext* context)
{
    if (g_openPending >= 0) {
        Pending& pending = g_pending[g_openPending];
        pending.closed = true;
        // A Present that never issued the draw has no query in flight and would
        // otherwise wait for a result that can never arrive.
        if (!pending.row.drawIssued) {
            if (pending.query >= 0) {
                g_queryBusy[pending.query] = false;
                pending.query = -1;
            }
            pending.queryDone = true;
        }
        g_openPending = -1;
    }
    if (g_created && context != nullptr)
        ResolvePending (context);
}

void ResetStatistics ()
{
    for (size_t i = 0; i < kVariantCount; ++i) {
        g_variants[i] = VariantStats {};
        g_errorCount[i] = 0;
        g_errorNext[i] = 0;
        g_errorSum[i] = 0.0;
    }
    g_havePrevious = false;
    g_counters.rowsQualified = 0;
    g_counters.rowsRejectedNotNew = 0;
    g_counters.rowsRejectedStill = 0;
}

void Shutdown ()
{
    for (size_t i = 0; i < kPendingCapacity; ++i)
        g_pending[i] = Pending {};
    for (size_t i = 0; i < kQueryCount; ++i) {
        ReleaseAndNull (g_queries[i]);
        g_queryBusy[i] = false;
    }
    for (size_t i = 0; i < kStagingDepth; ++i) {
        ReleaseAndNull (g_staging[i].view);
        ReleaseAndNull (g_staging[i].projection);
        g_staging[i] = StagingSlot {};
    }
    ReleaseAndNull (g_device);
    g_haveStaged = false;
    g_nextSlot = 0;
    g_openPending = -1;
    g_created = false;
    g_createFailed = false;
    g_counters.ready = false;
}

size_t CopyRows (Row* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    const uint64_t written = g_rowsWritten.load (std::memory_order_acquire);
    const uint64_t available = written < kRowCapacity ? written : kRowCapacity;
    const uint64_t wanted = available < capacity ? available : uint64_t (capacity);
    const uint64_t firstIndex = written - wanted;
    for (uint64_t i = 0; i < wanted; ++i)
        out[i] = g_rows[(firstIndex + i) % kRowCapacity];
    return size_t (wanted);
}

size_t CopyVariants (VariantStats* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    const size_t wanted = capacity < kVariantCount ? capacity : kVariantCount;
    for (size_t i = 0; i < wanted; ++i) {
        out[i] = g_variants[i];
        const uint32_t count = g_errorCount[i];
        if (count == 0) {
            out[i].medianCentreError = 0.0f;
            out[i].meanCentreError = 0.0f;
            continue;
        }
        out[i].meanCentreError = float (g_errorSum[i] / double (out[i].rowsTested));
        // A copy is sorted rather than the ring itself: the render thread keeps
        // writing into the ring while this runs.
        float sorted[kErrorSamples];
        std::memcpy (sorted, g_errors[i], sizeof (float) * count);
        for (uint32_t a = 1; a < count; ++a) {
            const float key = sorted[a];
            uint32_t b = a;
            while (b > 0 && sorted[b - 1] > key) {
                sorted[b] = sorted[b - 1];
                --b;
            }
            sorted[b] = key;
        }
        out[i].medianCentreError = sorted[count / 2];
    }
    return wanted;
}

Counters GetCounters ()
{
    return g_counters;
}

}   // namespace oracle
}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
