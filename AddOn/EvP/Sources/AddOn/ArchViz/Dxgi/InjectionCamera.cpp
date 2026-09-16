// ArchViz/Dxgi/InjectionCamera -- see the header. Every rule about this file is
// in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/InjectionCamera.hpp"

#include "ArchViz/Dxgi/InjectionOracle.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"

#include <d3d11_1.h>

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

namespace {

// ⚠️ THE WINDOW SIZE THE CAMERA BINDINGS MUST HAVE. Run twenty-one measured 16
// constants -- 256 bytes, the D3D11.1 minimum granularity -- for both b1 and b2.
// Anything else is a different layout and the predicate refuses rather than
// reading a matrix out of the wrong place.
constexpr UINT kExpectedWindowConstants = 16;

// ⚠️ WHICH OF THE CENSUS'S EIGHT INTERPRETATIONS THE INJECTED SHADER IMPLEMENTS.
// See InjectionRenderer.cpp's shader source: `View` is declared `row_major` and
// `Projection` `column_major`, which is `p * V * Pt` -- interpretation 2, the one
// the selected group returned in phase A and again in phase B at 99% inside the
// clip volume and a median of 0.002 from the viewport centre.
// The declaration bound at start-up, before the census has selected anything.
constexpr uint32_t kDefaultInterpretation = 0;
std::atomic<uint32_t> g_shaderInterpretation {kDefaultInterpretation};

// Sentinel: nothing has been learned, so there is nothing to disagree with.
constexpr uint32_t kNoInterpretation = 0xffffffffu;

// ⚠️ OUR OWN COPIES OF THE CAMERA, 256 BYTES EACH -- one D3D11.1 window. The
// injected draw binds THESE and never Archicad's ring, so nothing Archicad does
// to the ring after the model draw can change what the shader reads.
ID3D11Device* g_device = nullptr;
ID3D11Buffer* g_viewSnapshot = nullptr;
ID3D11Buffer* g_projectionSnapshot = nullptr;
bool g_created = false;
bool g_createFailed = false;

uint64_t g_snapshotModelGeneration = 0;
uint64_t g_snapshotDrawSequence = 0;
bool     g_snapshotValid = false;
contextstate::SceneDrawState g_snapshotDraw;
SelectedCameraState g_selectedCamera;

std::atomic<uint64_t> g_snapshotsTaken {0};
std::atomic<uint64_t> g_qualifyingCameraDraws {0};
std::atomic<uint64_t> g_viewCopies {0};
std::atomic<uint64_t> g_projectionCopies {0};
std::atomic<uint64_t> g_selectedGroupDraws {0};
std::atomic<uint64_t> g_selectedGroupSnapshots {0};
std::atomic<int> g_cameraSource {int (CameraSource::Learner)};
std::atomic<uint32_t> g_expectedInterpretation {kNoInterpretation};

// ⚠️ ONE SLOT PER OCCURRENCE WITHIN A MODEL FRAME, EACH WITH ITS OWN BYTES. The
// selected group draws several times per frame -- six, in run thirty-four -- and
// only one of those draws carries the visible model's camera. A single global
// snapshot handed the shader whichever drew last, which is why the anchor
// alternated between the viewport centre and a clip z of a hundred.
constexpr size_t kErrorSamples = 64;

struct Occurrence {
    ID3D11Buffer* view = nullptr;
    ID3D11Buffer* projection = nullptr;
    ID3D11Buffer* stagingView = nullptr;
    ID3D11Buffer* stagingProjection = nullptr;
    bool     copyPending = false;

    uint64_t draws = 0;
    uint64_t modelFrames = 0;
    uint64_t lastModelCounted = 0;
    uint64_t lastDrawSequence = 0;
    float    viewportX = 0.0f, viewportY = 0.0f;
    float    viewportWidth = 0.0f, viewportHeight = 0.0f;

    uint32_t samples = 0;
    uint32_t insideClip = 0;
    double   spreadSum = 0.0;
    float    errors[kErrorSamples] = {};
    uint32_t errorCount = 0;
    uint32_t errorNext = 0;
    double   errorSum = 0.0;
    float    worst = 0.0f;
    bool     used = false;
};

Occurrence g_occurrences[kOccurrenceCapacity];
uint64_t g_occurrenceModelGeneration = 0;   // the generation being counted through
uint32_t g_occurrenceIndex = 0;             // position within that generation
uint64_t g_occurrenceModelFrames = 0;
std::atomic<uint64_t> g_occurrenceDraws {0};
std::atomic<uint64_t> g_authoritativeSnapshots {0};
bool     g_occurrenceLocked = false;
uint32_t g_lockedOccurrence = 0;

// ⚠️ SCORED AT INTERPRETATION 2 AND NOTHING ELSE. The census already settled
// which reading of these bytes is Archicad's -- `p * V * Pt`, twice over -- and
// the question here is not which transform but which DRAW. Re-opening the
// transform question per occurrence would let a wrong occurrence win under a
// wrong reading.
constexpr size_t kScoredInterpretation = 2;

template <typename T>
void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
}

// ⚠️ `DEFAULT` USAGE, NOT `IMMUTABLE` AND NOT `DYNAMIC`. The GPU writes these
// through `CopySubresourceRegion` and the vertex stage reads them: an immutable
// buffer could not be copied into, and a dynamic one would invite exactly the
// CPU round trip this design exists to avoid.
bool EnsureCameraCreated (ID3D11DeviceContext* context)
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
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = 256;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bool ok = SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &g_viewSnapshot)) &&
              SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &g_projectionSnapshot));

    // Each occurrence binds its own pair, and is scored from its own staging
    // pair -- read later, with DO_NOT_WAIT, never waited for.
    D3D11_BUFFER_DESC staging = {};
    staging.ByteWidth = 256;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (size_t i = 0; i < kOccurrenceCapacity; ++i) {
        Occurrence& slot = g_occurrences[i];
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &slot.view));
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &slot.projection));
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&staging, nullptr, &slot.stagingView));
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&staging, nullptr,
                &slot.stagingProjection));
    }
    if (!ok) {
        g_createFailed = true;
        return false;
    }
    g_created = true;
    return true;
}

// Read one occurrence's staged pair if the GPU has finished with it. Never waits.
void TryScoreOccurrence (ID3D11DeviceContext* context, Occurrence& slot)
{
    D3D11_MAPPED_SUBRESOURCE viewMap = {};
    D3D11_MAPPED_SUBRESOURCE projectionMap = {};
    HRESULT hr = context->Map (slot.stagingView, 0, D3D11_MAP_READ,
                               D3D11_MAP_FLAG_DO_NOT_WAIT, &viewMap);
    if (FAILED (hr) || viewMap.pData == nullptr)
        return;
    hr = context->Map (slot.stagingProjection, 0, D3D11_MAP_READ,
                       D3D11_MAP_FLAG_DO_NOT_WAIT, &projectionMap);
    if (FAILED (hr) || projectionMap.pData == nullptr) {
        context->Unmap (slot.stagingView, 0);
        return;
    }
    float view[16];
    float projection[16];
    std::memcpy (view, viewMap.pData, sizeof (view));
    std::memcpy (projection, projectionMap.pData, sizeof (projection));
    context->Unmap (slot.stagingProjection, 0);
    context->Unmap (slot.stagingView, 0);
    slot.copyPending = false;

    oracle::ViewportRect viewport;
    viewport.x = slot.viewportX;
    viewport.y = slot.viewportY;
    viewport.width = slot.viewportWidth;
    viewport.height = slot.viewportHeight;
    oracle::VariantScore scores[oracle::kVariantCount];
    oracle::ScoreVariants (view, projection, viewport, scores);

    const oracle::VariantScore& score = scores[kScoredInterpretation];
    ++slot.samples;
    if (!score.validProjection)
        return;
    ++slot.insideClip;
    slot.spreadSum += double (score.spreadPixels);
    if (score.centreError > slot.worst)
        slot.worst = score.centreError;
    slot.errorSum += double (score.centreError);
    slot.errors[slot.errorNext] = score.centreError;
    slot.errorNext = uint32_t ((slot.errorNext + 1) % kErrorSamples);
    if (slot.errorCount < kErrorSamples)
        ++slot.errorCount;
}

float OccurrenceMedian (const Occurrence& slot)
{
    if (slot.errorCount == 0)
        return 0.0f;
    float sorted[kErrorSamples];
    std::memcpy (sorted, slot.errors, sizeof (float) * slot.errorCount);
    for (uint32_t a = 1; a < slot.errorCount; ++a) {
        const float key = sorted[a];
        uint32_t b = a;
        while (b > 0 && sorted[b - 1] > key) {
            sorted[b] = sorted[b - 1];
            --b;
        }
        sorted[b] = key;
    }
    return sorted[slot.errorCount / 2];
}

}   // namespace

void CopyCameraWindows (ID3D11DeviceContext* context,
                        const contextstate::SceneDrawState& draw);

void SnapshotCamera (ID3D11DeviceContext* context)
{
    if (context == nullptr || !Enabled ())
        return;
    // ⚠️ THE LEARNER'S PATH STANDS DOWN ONCE A GROUP IS SELECTED. Run thirty
    // proved the learned model pass carries more than one camera, so once the
    // census has committed to a group, letting this path keep snapshotting would
    // put the other camera back in on the draws the census did not choose.
    if (GetCameraSource () != CameraSource::Learner)
        return;

    // ⚠️ COUNTED HERE, BEFORE ANY REFUSAL, because this function is called from
    // exactly one place: a draw that `renderstate::OnDraw` has already certified
    // as a camera-bearing draw of the learned model pass. Every `return` below
    // this line is therefore a measurable gap between "qualified" and "copied".
    g_qualifyingCameraDraws.fetch_add (1, std::memory_order_relaxed);
    // ⚠️ OUR OWN COPIES ARE NOT ARCHICAD'S WORK. Without the guard these two
    // copies would be counted as Archicad copy operations and could trip the
    // scene-consumer logic that once served as the injection trigger.
    if (contextstate::Injecting ())
        return;

    const contextstate::SceneDrawState draw = contextstate::LastCameraDraw ();
    if (!draw.valid)
        return;
    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];
    if (view.numConstants != kExpectedWindowConstants ||
        projection.numConstants != kExpectedWindowConstants)
        return;

    CopyCameraWindows (context, draw);
}

// The two GPU copies, the stamps, and the diagnostic staging -- shared by both
// camera sources so they can never drift apart in what they preserve.
void CopyCameraWindows (ID3D11DeviceContext* context,
                        const contextstate::SceneDrawState& draw)
{
    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];

    contextstate::ScopedInjectionGuard guard;
    if (!EnsureCameraCreated (context))
        return;

    // ⚠️ BYTE COORDINATES, BECAUSE THESE ARE BUFFERS AND NOT TEXTURES. `left` and
    // `right` are byte offsets into the ring; `top`/`bottom`/`front`/`back` are
    // the 0..1 a buffer always has. Copying 256 bytes from `firstConstant * 16`
    // lands the window at offset 0 of our own buffer, which is why the shader
    // needs no padding and why Present can bind it with firstConstant 0.
    D3D11_BOX box = {};
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;

    box.left = view.ByteOffset ();
    box.right = box.left + 256;
    context->CopySubresourceRegion (g_viewSnapshot, 0, 0, 0, 0,
            reinterpret_cast<ID3D11Buffer*> (uintptr_t (view.buffer)), 0, &box);
    g_viewCopies.fetch_add (1, std::memory_order_relaxed);

    box.left = projection.ByteOffset ();
    box.right = box.left + 256;
    context->CopySubresourceRegion (g_projectionSnapshot, 0, 0, 0, 0,
            reinterpret_cast<ID3D11Buffer*> (uintptr_t (projection.buffer)), 0, &box);
    g_projectionCopies.fetch_add (1, std::memory_order_relaxed);

    // ⚠️ EVERY QUALIFYING DRAW, NOT A GUESS AT THE LAST ONE. Two 256-byte GPU
    // copies are nothing beside a draw, and overwriting on each one means that by
    // Present the snapshot simply holds the most recent camera of the pass --
    // with no prediction about which draw would turn out to be final.
    g_snapshotModelGeneration = draw.modelSceneGeneration;
    g_snapshotDrawSequence = draw.drawSequence;
    g_snapshotDraw = draw;
    g_snapshotValid = true;
    const uint64_t sequence = g_snapshotsTaken.fetch_add (1, std::memory_order_relaxed) + 1;

    // ⚠️ THE DIAGNOSTIC COPY IS OF OUR COPY, NEVER OF ARCHICAD'S RING. It is one
    // more GPU-to-GPU copy of 256 bytes and it is read back LATER, from a slot
    // the GPU has finished with; nothing on this line waits for anything.
    oracle::OnSnapshot (context, g_viewSnapshot, g_projectionSnapshot, sequence,
            draw.modelSceneGeneration, draw.drawSequence,
            view.buffer, view.firstConstant, view.numConstants,
            projection.buffer, projection.firstConstant, projection.numConstants,
            draw.viewportX, draw.viewportY, draw.viewportWidth, draw.viewportHeight);
}

uint32_t ShaderInterpretation ()
{
    return g_shaderInterpretation.load (std::memory_order_acquire);
}

void SetShaderInterpretation (uint32_t variant)
{
    g_shaderInterpretation.store (variant, std::memory_order_release);
}

void SetExpectedInterpretation (uint32_t variant)
{
    g_expectedInterpretation.store (variant, std::memory_order_release);
}

uint32_t ExpectedInterpretation ()
{
    return g_expectedInterpretation.load (std::memory_order_acquire);
}

bool InterpretationAgrees ()
{
    const uint32_t expected = ExpectedInterpretation ();
    // Nothing learned yet means nothing to disagree with -- the learner path has
    // no interpretation of its own to offer. ⚠️ AND A REVERSED MULTIPLICATION
    // ORDER (4..7) CANNOT BE EXPRESSED AS A DECLARATION, so it is refused rather
    // than silently drawn as variant 0.
    // ⚠️ EVERY DECLARATION IN 0..3 IS COMPILED, so agreement is structural
    // rather than a coincidence to be checked: the draw binds whichever the
    // census selected. Only a reversed multiplication order (4..7) has no
    // declaration that expresses it, and that is refused rather than silently
    // drawn as variant 0. `ShaderInterpretation ()` then REPORTS what was bound.
    return expected == kNoInterpretation || expected < 4;
}

void SetCameraSource (CameraSource source)
{
    g_cameraSource.store (int (source), std::memory_order_release);
}

CameraSource GetCameraSource ()
{
    return CameraSource (g_cameraSource.load (std::memory_order_acquire));
}

void SnapshotSelectedDraw (ID3D11DeviceContext* context,
                           const contextstate::SceneDrawState& draw, uint32_t groupId)
{
    if (context == nullptr || !Enabled ())
        return;

    // ⚠️ THERE IS DELIBERATELY NO `contextstate::Injecting ()` CHECK HERE. See
    // the header: the census already holds the guard when it calls this, so the
    // check refused every single call. Run thirty-two shipped with it and the
    // report read "Present injections succeeded: 0" with every skip counter also
    // zero -- the signature of a camera that was never connected rather than a
    // camera that was wrong.
    g_selectedGroupDraws.fetch_add (1, std::memory_order_relaxed);

    const contextstate::ConstantBufferBinding& view = draw.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = draw.vsConstantBuffers[2];
    if (view.numConstants != kExpectedWindowConstants ||
        projection.numConstants != kExpectedWindowConstants)
        return;

    if (!EnsureCameraCreated (context))
        return;

    // ⚠️ THE OCCURRENCE IS A POSITION WITHIN THE MODEL FRAME, reset whenever the
    // generation changes. Nothing about the binding is used to derive it: the
    // ring window advances every frame by design, so an offset names a moment
    // and never a role.
    if (draw.modelSceneGeneration != g_occurrenceModelGeneration) {
        g_occurrenceModelGeneration = draw.modelSceneGeneration;
        g_occurrenceIndex = 0;
        ++g_occurrenceModelFrames;
    }
    const uint32_t occurrence = g_occurrenceIndex;
    if (occurrence < kOccurrenceCapacity)
        ++g_occurrenceIndex;
    g_occurrenceDraws.fetch_add (1, std::memory_order_relaxed);
    if (occurrence >= kOccurrenceCapacity)
        return;

    Occurrence& slot = g_occurrences[occurrence];
    slot.used = true;
    ++slot.draws;
    slot.lastDrawSequence = draw.drawSequence;
    slot.viewportX = draw.viewportX;
    slot.viewportY = draw.viewportY;
    slot.viewportWidth = draw.viewportWidth;
    slot.viewportHeight = draw.viewportHeight;
    if (slot.lastModelCounted != draw.modelSceneGeneration) {
        slot.lastModelCounted = draw.modelSceneGeneration;
        ++slot.modelFrames;
    }

    // ⚠️ EVERY OCCURRENCE KEEPS ITS OWN BYTES. Overwriting one global pair is
    // exactly what handed the shader whichever of the six drew last.
    D3D11_BOX box = {};
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    box.left = view.ByteOffset ();
    box.right = box.left + 256;
    context->CopySubresourceRegion (slot.view, 0, 0, 0, 0,
            reinterpret_cast<ID3D11Buffer*> (uintptr_t (view.buffer)), 0, &box);
    box.left = projection.ByteOffset ();
    box.right = box.left + 256;
    context->CopySubresourceRegion (slot.projection, 0, 0, 0, 0,
            reinterpret_cast<ID3D11Buffer*> (uintptr_t (projection.buffer)), 0, &box);

    if (!slot.copyPending) {
        context->CopyResource (slot.stagingView, slot.view);
        context->CopyResource (slot.stagingProjection, slot.projection);
        slot.copyPending = true;
    } else {
        TryScoreOccurrence (context, slot);
    }

    // ⚠️ THE CENSUS HAS ALREADY FILTERED BY OCCURRENCE, so this does not
    // re-derive or re-compare it: the camera identity is atomic and the census
    // owns it. What remains here is the fail-closed rule -- until something has
    // been selected there is no authoritative camera, `SnapshotValid` stays
    // false, and Present injects nothing.
    if (!g_occurrenceLocked)
        return;

    CopyCameraWindows (context, draw);
    g_authoritativeSnapshots.fetch_add (1, std::memory_order_relaxed);

    // ⚠️ THE AUTHORITATIVE STATE, WRITTEN ONLY HERE. Present reads this and
    // nothing else while a group is selected.
    // ⚠️ AND `modelSceneGeneration` IS THE MODEL FRAME'S, NOT A COUNTER OF ITS
    // OWN. Incrementing per snapshot would make every Present a NEW_SCENE and
    // REPEAT_SCENE would never occur -- which would hide exactly the case Present
    // injection exists to handle: Archicad presenting again without re-rendering.
    g_selectedCamera.valid = true;
    g_selectedCamera.groupId = groupId;
    ++g_selectedCamera.snapshotGeneration;
    g_selectedCamera.modelSceneGeneration = draw.modelSceneGeneration;
    g_selectedCamera.sourceDrawSequence = draw.drawSequence;
    g_selectedCamera.viewportX = draw.viewportX;
    g_selectedCamera.viewportY = draw.viewportY;
    g_selectedCamera.viewportWidth = draw.viewportWidth;
    g_selectedCamera.viewportHeight = draw.viewportHeight;
    g_selectedGroupSnapshots.fetch_add (1, std::memory_order_relaxed);
}

SelectedCameraState GetSelectedCamera ()
{
    return g_selectedCamera;
}

ID3D11Buffer* ViewSnapshotBuffer ()
{
    return g_viewSnapshot;
}

ID3D11Buffer* ProjectionSnapshotBuffer ()
{
    return g_projectionSnapshot;
}

bool SnapshotValid ()
{
    return g_snapshotValid;
}

contextstate::SceneDrawState SnapshotDraw ()
{
    return g_snapshotDraw;
}

uint64_t SnapshotModelGeneration ()
{
    return g_snapshotModelGeneration;
}

uint64_t SnapshotDrawSequence ()
{
    return g_snapshotDrawSequence;
}

size_t CopyOccurrences (OccurrenceStats* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    size_t written = 0;
    for (size_t i = 0; i < kOccurrenceCapacity && written < capacity; ++i) {
        const Occurrence& slot = g_occurrences[i];
        if (!slot.used)
            continue;
        OccurrenceStats stats;
        stats.index = uint32_t (i);
        stats.draws = slot.draws;
        stats.modelFrames = slot.modelFrames;
        stats.samples = slot.samples;
        stats.insideClip = slot.insideClip;
        stats.medianCentreError = OccurrenceMedian (slot);
        stats.meanCentreError = slot.errorCount > 0
                ? float (slot.errorSum / double (slot.errorCount)) : 0.0f;
        stats.worstCentreError = slot.worst;
        stats.meanSpreadPixels = slot.insideClip > 0
                ? float (slot.spreadSum / double (slot.insideClip)) : 0.0f;
        stats.viewportWidth = slot.viewportWidth;
        stats.viewportHeight = slot.viewportHeight;
        stats.lastDrawSequence = slot.lastDrawSequence;
        out[written] = stats;
        ++written;
    }
    return written;
}

uint64_t OccurrenceModelFrames ()
{
    return g_occurrenceModelFrames;
}

void ResetOccurrences ()
{
    for (size_t i = 0; i < kOccurrenceCapacity; ++i) {
        Occurrence& slot = g_occurrences[i];
        Occurrence fresh;
        fresh.view = slot.view;
        fresh.projection = slot.projection;
        fresh.stagingView = slot.stagingView;
        fresh.stagingProjection = slot.stagingProjection;
        slot = fresh;
    }
    g_occurrenceModelFrames = 0;
    g_occurrenceModelGeneration = 0;
    g_occurrenceIndex = 0;
    g_occurrenceDraws.store (0, std::memory_order_relaxed);
    g_authoritativeSnapshots.store (0, std::memory_order_relaxed);
}

bool SelectOccurrence ()
{
    // ⚠️ THE SAME SHAPE OF GATE THE CENSUS USES, FOR THE SAME REASON. An
    // occurrence seen twice, with one valid sample, is not a candidate however
    // small its error.
    const uint64_t frames = g_occurrenceModelFrames;
    int best = -1;
    float bestMedian = 0.0f;
    float bestInside = 0.0f;
    for (size_t i = 0; i < kOccurrenceCapacity; ++i) {
        const Occurrence& slot = g_occurrences[i];
        if (!slot.used || slot.samples < 8)
            continue;
        const float inside = float (double (slot.insideClip) / double (slot.samples));
        const float coverage = frames > 0
                ? float (double (slot.modelFrames) / double (frames)) : 0.0f;
        const float median = OccurrenceMedian (slot);
        // ⚠️ THE SAME LOOSENED THRESHOLD AS THE CENSUS, AND FOR THE SAME REASON:
        // 0.05 was calibrated against a transform that collapsed the primitive to
        // a point, and therefore selected for degeneracy. See
        // `CameraCensus::Eligibility::maxMedianCentreError`.
        if (coverage < 0.80f || inside < 0.95f || median > 0.25f)
            continue;
        if (best < 0 || inside > bestInside + 0.005f ||
            (inside >= bestInside - 0.005f && median < bestMedian)) {
            best = int (i);
            bestMedian = median;
            bestInside = inside;
        }
    }
    if (best < 0) {
        g_occurrenceLocked = false;
        return false;
    }
    g_occurrenceLocked = true;
    g_lockedOccurrence = uint32_t (best);
    return true;
}

void SetSelectedOccurrence (uint32_t index)
{
    g_occurrenceLocked = true;
    g_lockedOccurrence = index;
}

void ClearOccurrenceLock ()
{
    g_occurrenceLocked = false;
    g_lockedOccurrence = 0;
}

bool OccurrenceLocked ()
{
    return g_occurrenceLocked;
}

uint32_t LockedOccurrence ()
{
    return g_lockedOccurrence;
}

CameraStats GetCameraStats ()
{
    CameraStats stats;
    stats.qualifyingCameraDraws = g_qualifyingCameraDraws.load (std::memory_order_relaxed);
    stats.viewCopies = g_viewCopies.load (std::memory_order_relaxed);
    stats.projectionCopies = g_projectionCopies.load (std::memory_order_relaxed);
    stats.snapshotsTaken = g_snapshotsTaken.load (std::memory_order_relaxed);
    stats.selectedGroupDraws = g_selectedGroupDraws.load (std::memory_order_relaxed);
    stats.selectedGroupSnapshots = g_selectedGroupSnapshots.load (std::memory_order_relaxed);
    stats.selectedGroupId = g_selectedCamera.groupId;
    stats.selectedSnapshotGeneration = g_selectedCamera.snapshotGeneration;
    stats.snapshotValid = g_snapshotValid;
    stats.occurrenceDraws = g_occurrenceDraws.load (std::memory_order_relaxed);
    stats.authoritativeSnapshots = g_authoritativeSnapshots.load (std::memory_order_relaxed);
    stats.occurrenceModelFrames = g_occurrenceModelFrames;
    stats.occurrenceLocked = g_occurrenceLocked;
    stats.lockedOccurrence = g_lockedOccurrence;
    return stats;
}

void ShutdownCamera ()
{
    for (size_t i = 0; i < kOccurrenceCapacity; ++i) {
        Occurrence& slot = g_occurrences[i];
        ReleaseAndNull (slot.view);
        ReleaseAndNull (slot.projection);
        ReleaseAndNull (slot.stagingView);
        ReleaseAndNull (slot.stagingProjection);
        slot = Occurrence {};
    }
    ClearOccurrenceLock ();
    g_occurrenceModelFrames = 0;
    g_occurrenceModelGeneration = 0;
    g_occurrenceIndex = 0;
    ReleaseAndNull (g_projectionSnapshot);
    ReleaseAndNull (g_viewSnapshot);
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_snapshotValid = false;
    g_snapshotModelGeneration = 0;
    g_snapshotDrawSequence = 0;
    g_snapshotDraw = contextstate::SceneDrawState {};
    g_selectedCamera = SelectedCameraState {};
}

}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
