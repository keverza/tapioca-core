// ArchViz/Dxgi/CameraCensus -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/CameraCensus.hpp"

#include "ArchViz/Dxgi/CameraRecognizer.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"
#include "ArchViz/Dxgi/InjectionDepth.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/InjectionOracle.hpp"
#include "ArchViz/Dxgi/InjectionProbes.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include <d3d11_1.h>

#include <atomic>
#include <cmath>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace census {

namespace {

constexpr UINT kWindowBytes = 256;
constexpr UINT kCameraWindowConstants = 16;
constexpr size_t kErrorSamples = 128;

// ⚠️ AT MOST THIS MANY READBACKS ARE ATTEMPTED PER DRAW. A census that mapped
// thirty-two staging buffers on every draw call would change the frame timing of
// the thing it is measuring, which is the one thing a diagnostic may never do.
// Spreading them costs nothing: a slot that is not read this draw is read on the
// next one, and there are hundreds per second.
constexpr size_t kResolvesPerDraw = 4;

struct Slot {
    Group group;

    ID3D11Buffer* stagingView = nullptr;
    ID3D11Buffer* stagingProjection = nullptr;
    bool copyPending = false;

    double spreadSum = 0.0;
    uint32_t spreadCount = 0;
    float errors[kErrorSamples] = {};
    uint32_t errorCount = 0;
    uint32_t errorNext = 0;
    double errorSum = 0.0;
    double variantErrorSum[kVariantCount] = {};
    float areas[kErrorSamples] = {};
    float edges[kErrorSamples] = {};
    uint32_t triangleCount = 0;
    uint32_t triangleNext = 0;

    uint64_t lastPresentCounted = 0;
    uint64_t lastModelCounted = 0;
    bool used = false;
};

ID3D11Device* g_device = nullptr;
Slot g_slots[kGroupCapacity];
Stats g_stats;
std::atomic<bool> g_enabled { false };
// See `SetAutoSelect`. Off by default so the diagnostic keeps choosing by hand
// and showing its working; the production runtime turns it on.
std::atomic<bool> g_autoSelect { false };

// ⚠️ THIS IS COMPARED AGAINST `modelFramesSeen`, WHICH `ResetCounts`
// SETS BACK TO ZERO -- AND FOR TWO WEEKS THIS DID NOT GO BACK WITH IT. After any
// session that ran to N model frames it was left holding N, so the next session
// needed `modelFramesSeen >= N + 30` from a counter that had just restarted at
// zero. With N in the hundreds that is never, for the rest of the process.
//
// ⚠️ THAT IS EXACTLY "IT WORKED WHEN IT FIRST REPLACED THE OLD
// OVERLAY MODE": the FIRST activation in a fresh Archicad selects, and every
// later one silently never attempts. The census then reported `eligible=0`,
// which was true and misleading -- the gate was never EVALUATED, because
// `SelectCandidate` was never CALLED.
uint64_t g_lastAutoSelectAttempt = 0;
uint64_t g_autoSelectAttempts = 0;

// ⚠️ FOUR FRAMES, NOT THIRTY, AND THE DEAD ZONE WAS THE OTHER HALF
// OF THE FAULT. At thirty the FIRST attempt could not happen until the thirtieth
// model frame; run sixty-six navigated for four seconds, reached twenty-eight,
// and never tried at all. `SelectCandidate` is a stack copy of 48 groups and a
// few compares -- cheap enough four-frame, and it stops entirely once locked.
constexpr uint64_t kAutoSelectEveryModelFrames = 4;
bool g_created = false;
bool g_createFailed = false;
uint64_t g_lastPresentSeen = 0;
uint64_t g_lastModelSeen = 0;

// ⚠️ THE OCCURRENCE INDEX BELONGS TO THE SIGNATURE, NOT TO THE TABLE. Each
// distinct signature draws several times per model frame, and "which of them" is
// counted per signature and reset when the model generation changes. A global
// counter would number draws across different signatures and mean nothing.
constexpr size_t kSignatureCounters = 32;
// ⚠️ KEYED LOGICALLY FOR THE SAME REASON AND, CRITICALLY, AT THE
// SAME GRAIN AS BEFORE. It used to key on (renderTarget, depthStencil): the
// TARGET identity, deliberately coarser than the group key, so that every draw
// into the same target is numbered in one sequence. The logical translation is
// the target's DESCRIPTION plus the viewport -- not the draw shape, which would
// split the numbering and change what "occurrence #0" means. Occurrence
// semantics are frozen; only the way the target is named has changed.
struct SignatureCounter {
    // ⚠️ KEYED EXACTLY AS THE GROUPS ARE, and therefore NOT on the vertex
    // shader. A counter keyed more finely than the table it feeds would number
    // draws the table cannot tell apart.
    uint32_t renderTargetWidth = 0, renderTargetHeight = 0;
    uint32_t renderTargetFormat = 0, renderTargetSamples = 0;
    uint32_t depthWidth = 0, depthHeight = 0;
    uint32_t depthFormat = 0, depthSamples = 0;
    bool depthPresent = false;
    float viewportWidth = 0.0f, viewportHeight = 0.0f;
    uint64_t generation = 0;
    uint32_t count = 0;
    bool used = false;
};

bool SameTarget (const SignatureCounter& counter, const contextstate::ContextState& live)
{
    return counter.renderTargetWidth == live.renderTargetDesc.width &&
           counter.renderTargetHeight == live.renderTargetDesc.height &&
           counter.renderTargetFormat == live.renderTargetDesc.format &&
           counter.renderTargetSamples == live.renderTargetDesc.sampleCount &&
           counter.depthWidth == live.depthStencilDesc.width && counter.depthHeight == live.depthStencilDesc.height &&
           counter.depthFormat == live.depthStencilDesc.format &&
           counter.depthSamples == live.depthStencilDesc.sampleCount &&
           counter.depthPresent == (live.depthStencil != 0) &&
           std::fabs (counter.viewportWidth - live.viewportWidth) < 1.5f &&
           std::fabs (counter.viewportHeight - live.viewportHeight) < 1.5f;
}
SignatureCounter g_counters[kSignatureCounters];

// ⚠️ THE DEPTH PROOF WANTS THE LATEST POINT INSIDE THE PASS, NOT THE CAMERA'S
// OWN DRAW. The camera occurrence is usually the FIRST draw of its family, when
// Archicad's depth buffer holds almost nothing -- injecting there would let the
// BEHIND primitive through and read as "depth not working" when it only means
// "nothing had been drawn yet". So the last occurrence of the previous model
// frame is used as this frame's prediction, which self-corrects in one frame and
// needs no lookahead.
uint32_t g_selectionOccurrencesThisFrame = 0;
uint32_t g_selectionOccurrencesLastFrame = 0;
uint64_t g_selectionFrame = 0;

template <typename T> void ReleaseAndNull (T*& object)
{
    if (object != nullptr) {
        object->Release ();
        object = nullptr;
    }
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

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kWindowBytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    bool ok = true;
    for (size_t i = 0; i < kGroupCapacity; ++i) {
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &g_slots[i].stagingView));
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr, &g_slots[i].stagingProjection));
    }
    if (!ok) {
        g_createFailed = true;
        return false;
    }
    g_created = true;
    g_stats.ready = true;
    return true;
}

bool SameExtent (float a, float b)
{
    return std::fabs (a - b) < 1.5f;
}

// ⚠️ THE SIGNATURE IS WHAT THE DRAW IS, NOT WHERE WE THINK IT IS. No pass
// generation and no learner verdict appear in it: those are recorded as
// observations so the report can say where a group sat, but two draws with the
// same shader, targets, viewport and camera window shape are the same KIND of
// draw whatever pass they landed in -- and that is the thing being counted.
//
// ⚠️ `firstConstant` IS NOT PART OF THE KEY. Archicad binds successive windows of
// one advancing ring, so the offset changes on every frame by design; keying on
// it would make every frame its own group and the census would count nothing.
// ⚠️ THE VERTEX SHADER IS RECORDED AND DELIBERATELY NOT KEYED ON. Run
// thirty-nine added it and the table blew apart: 32 groups used and **4012 draws
// overflowed**, every surviving group down to 2% coverage. The reason is in its
// own rows -- groups 1, 2 and 3 had three DIFFERENT vertex shaders, the same
// render target and depth view, and byte-identical camera metrics. Archicad
// shares one camera across many shaders, so the shader identity fragments the
// signature instead of sharpening it. It stays in the report, where knowing
// which shader drew is useful, and out of the key, where it was fatal.
// ⚠️ AND NOT ONE COM ADDRESS IS IN IT ANY MORE, WHICH RUN
// FORTY-SEVEN FORCED. That run ended `groups used / overflowed : 48 / 5841`
// with 124 readbacks across 48 groups -- six samples each against a gate of 32
// -- and NOTHING QUALIFIED. The cause is the one run forty-five found a level
// down: this key held `renderTarget`, `depthStencil`, `viewBuffer` and
// `projectionBuffer`, all live COM addresses, and run forty-six measured
// Archicad rebuilding its views TWENTY TIMES in fifteen seconds. Every rebuild
// minted a fresh set of groups for the same camera; forty-eight slots is six
// rebuilds, and the table was full in the first second.
//
// The evidence is in run forty-six's own table: groups 1-8 at 1-2% coverage and
// group 36 at 96% were FRAGMENTS OF ONE THING, separated by nothing but the
// address of a view.
//
// So the key is what the draw IS, exactly as `Fingerprint` already is: the
// viewport rectangle, the draw shape, the camera window sizes, and the
// DESCRIPTIONS of the targets rather than their names. Same structure as
// before, logical terms in place of addresses.
bool Matches (const Group& group, const contextstate::ContextState& live, uint32_t occurrence)
{
    return group.occurrenceIndex == occurrence && SameExtent (group.viewportWidth, live.viewportWidth) &&
           SameExtent (group.viewportHeight, live.viewportHeight) && SameExtent (group.viewportX, live.viewportX) &&
           SameExtent (group.viewportY, live.viewportY) &&
           group.viewNumConstants == live.vsConstantBuffers[1].numConstants &&
           group.projectionNumConstants == live.vsConstantBuffers[2].numConstants &&
           group.depthPresent == (live.depthStencil != 0) && group.renderTargetWidth == live.renderTargetDesc.width &&
           group.renderTargetHeight == live.renderTargetDesc.height &&
           group.renderTargetFormat == live.renderTargetDesc.format &&
           group.renderTargetSamples == live.renderTargetDesc.sampleCount &&
           group.depthWidth == live.depthStencilDesc.width && group.depthHeight == live.depthStencilDesc.height &&
           group.depthFormat == live.depthStencilDesc.format && group.depthSamples == live.depthStencilDesc.sampleCount;
}

void RecordSample (Slot& slot, const injection::oracle::VariantScore* scores)
{
    Group& group = slot.group;
    ++group.samplesScored;

    bool haveBest = false;
    float bestError = 0.0f;
    for (size_t variant = 0; variant < kVariantCount; ++variant) {
        if (!scores[variant].validProjection)
            continue;
        ++group.variantValid[variant];
        slot.variantErrorSum[variant] += double (scores[variant].centreError);
        if (!haveBest || scores[variant].centreError < bestError) {
            haveBest = true;
            bestError = scores[variant].centreError;
        }
    }
    // ⚠️ INTERPRETATION 0 IS THE TRANSFORM NOW, AND THE HARD METRICS COME FROM
    // IT ALONE. Re-opening the convention per candidate would let a collapsing
    // reading win a candidate it has no business winning.
    const injection::oracle::VariantScore& production = scores[0];
    if (production.validProjection)
        ++group.anchorInside;
    if (production.verticesFinite) {
        ++group.trianglesFinite;
        slot.areas[slot.triangleNext] = production.areaPixels;
        slot.edges[slot.triangleNext] = production.maxEdgePixels;
        slot.triangleNext = uint32_t ((slot.triangleNext + 1) % kErrorSamples);
        if (slot.triangleCount < kErrorSamples)
            ++slot.triangleCount;
    }

    if (!haveBest)
        return;

    if (bestError > group.worstCentreError)
        group.worstCentreError = bestError;
    for (size_t variant = 0; variant < kVariantCount; ++variant) {
        if (!scores[variant].validProjection)
            continue;
        if (scores[variant].centreError > bestError + 1e-6f)
            continue;
        slot.spreadSum += double (scores[variant].spreadPixels);
        ++slot.spreadCount;
        break;
    }
    slot.errorSum += double (bestError);
    slot.errors[slot.errorNext] = bestError;
    slot.errorNext = uint32_t ((slot.errorNext + 1) % kErrorSamples);
    if (slot.errorCount < kErrorSamples)
        ++slot.errorCount;
}

// Read one group's staged pair if the GPU has finished with it. Never waits.
void TryResolve (ID3D11DeviceContext* context, Slot& slot)
{
    D3D11_MAPPED_SUBRESOURCE viewMap = {};
    D3D11_MAPPED_SUBRESOURCE projectionMap = {};
    HRESULT hr = context->Map (slot.stagingView, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &viewMap);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++g_stats.readbacksBusy;
        return;
    }
    if (FAILED (hr) || viewMap.pData == nullptr) {
        slot.copyPending = false;
        return;
    }
    hr = context->Map (slot.stagingProjection, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &projectionMap);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING || FAILED (hr) || projectionMap.pData == nullptr) {
        context->Unmap (slot.stagingView, 0);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            ++g_stats.readbacksBusy;
        else
            slot.copyPending = false;
        return;
    }

    float view[16];
    float projection[16];
    std::memcpy (view, viewMap.pData, sizeof (view));
    std::memcpy (projection, projectionMap.pData, sizeof (projection));
    context->Unmap (slot.stagingProjection, 0);
    context->Unmap (slot.stagingView, 0);
    slot.copyPending = false;
    ++g_stats.readbacksServed;

    injection::oracle::ViewportRect viewport;
    viewport.x = slot.group.viewportX;
    viewport.y = slot.group.viewportY;
    viewport.width = slot.group.viewportWidth;
    viewport.height = slot.group.viewportHeight;

    // ⚠️ THE SAME SCORER THE ORACLE USES, INCLUDING ITS VALIDITY GATE. Two copies
    // of this arithmetic would eventually disagree about which interpretation
    // won, which is the one question both of them exist to answer.
    injection::oracle::VariantScore scores[kVariantCount];
    injection::oracle::ScoreVariants (view, projection, viewport, scores);
    RecordSample (slot, scores);
}

void ResolveSome (ID3D11DeviceContext* context)
{
    size_t done = 0;
    for (size_t i = 0; i < kGroupCapacity && done < kResolvesPerDraw; ++i) {
        if (!g_slots[i].used || !g_slots[i].copyPending)
            continue;
        TryResolve (context, g_slots[i]);
        ++done;
    }
}

float MedianOf (const float* values, uint32_t count)
{
    if (count == 0)
        return 0.0f;
    float sorted[kErrorSamples];
    std::memcpy (sorted, values, sizeof (float) * count);
    for (uint32_t a = 1; a < count; ++a) {
        const float key = sorted[a];
        uint32_t b = a;
        while (b > 0 && sorted[b - 1] > key) {
            sorted[b] = sorted[b - 1];
            --b;
        }
        sorted[b] = key;
    }
    return sorted[count / 2];
}

float Median (Slot& slot)
{
    const uint32_t count = slot.errorCount;
    if (count == 0)
        return 0.0f;
    float sorted[kErrorSamples];
    std::memcpy (sorted, slot.errors, sizeof (float) * count);
    for (uint32_t a = 1; a < count; ++a) {
        const float key = sorted[a];
        uint32_t b = a;
        while (b > 0 && sorted[b - 1] > key) {
            sorted[b] = sorted[b - 1];
            --b;
        }
        sorted[b] = key;
    }
    return sorted[count / 2];
}

} // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
    g_stats.enabled = enabled;
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void SetAnchor (float x, float y, float z, float sizeMetres)
{
    // The anchor belongs to the scorer, which both this and the oracle share.
    injection::oracle::SetAnchor (x, y, z, sizeMetres);
}

void ResetCounts ()
{
    for (size_t i = 0; i < kGroupCapacity; ++i) {
        Slot& slot = g_slots[i];
        ID3D11Buffer* const view = slot.stagingView;
        ID3D11Buffer* const projection = slot.stagingProjection;
        slot = Slot {};
        slot.stagingView = view;
        slot.stagingProjection = projection;
    }
    const bool ready = g_stats.ready;
    const bool enabled = g_stats.enabled;
    g_stats = Stats {};
    g_stats.ready = ready;
    g_stats.enabled = enabled;
    g_lastPresentSeen = 0;
    g_lastModelSeen = 0;
    // See the declaration: this is compared against `modelFramesSeen`, which the
    // line above has just zeroed, so it has to go with it.
    g_lastAutoSelectAttempt = 0;
    g_autoSelectAttempts = 0;
    for (size_t i = 0; i < kSignatureCounters; ++i)
        g_counters[i] = SignatureCounter {};

    // ⚠️ THE FINGERPRINT AND THE SELECTION DELIBERATELY SURVIVE THIS.
    // See the header. What is reset is the memory of WHERE the pinned resources
    // were last seen -- phase B's generations are not phase A's, and starting at
    // "never seen" is what lets the first logical match re-acquire immediately
    // instead of waiting out a staleness window.
    // ⚠️ THE RECOGNIZER'S COUNTERS RESET WITH OURS, ITS DECISION
    // DOES NOT. See `CameraRecognizer::ResetBindingStats`.
    ResetBindingStats ();
}

void Reset ()
{
    // ⚠️ THE SELECTION SURVIVES A COUNT RESET, AND THAT IS WHAT PHASE B NEEDS.
    // Proof A keeps scoring the group it locked on to while injecting with it,
    // so the report can say whether the group stayed correct DURING the proof --
    // which is a different claim from "it was correct when we chose it".
    ResetCounts ();
    ClearSelection ();
}

void OnDraw (ID3D11DeviceContext* context, DrawKind kind, uint32_t indexCount)
{
    if (context == nullptr || !g_enabled.load (std::memory_order_acquire))
        return;
    // ⚠️ OUR OWN DRAWS ARE NOT ARCHICAD'S. The injected triangle reaches this
    // same detour on the same context, and counting it as a camera-bearing draw
    // would put the instrument in its own census.
    if (contextstate::Injecting ())
        return;

    ++g_stats.drawsSeen;
    contextstate::ContextState live = contextstate::Snapshot ();

    // ⚠️ DEPTH PROVENANCE SEES EVERY DRAW, NOT ONLY CAMERA-BEARING
    // ONES, AND THAT IS THE POINT. The draw that ruins the depth buffer for us is
    // a transparent build plane, and it has no reason to bind the camera windows
    // the admission test below requires. Classifying after that test would miss
    // exactly the draws this is looking for.
    injection::depth::OnDraw (context, live.depthStencil, renderstate::ModelSceneGeneration ());

    // ⚠️ DEPTH PROVENANCE SEES EVERY DRAW, NOT ONLY CAMERA-BEARING
    // ONES, AND THAT IS THE POINT. The draw that ruins the depth buffer for us is
    // a transparent build plane, and it has no reason to bind the camera windows
    // the admission test below requires. Classifying after that test would miss
    // exactly the draws this is looking for.
    injection::depth::OnDraw (context, live.depthStencil, renderstate::ModelSceneGeneration ());
    const contextstate::ConstantBufferBinding& view = live.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = live.vsConstantBuffers[2];

    // ⚠️ THE ONLY ADMISSION TEST, AND THE LEARNER HAS NO PART IN IT. Both camera
    // windows, bound, at the 256-byte shape stage 3 identified -- whatever pass
    // the draw belongs to.
    if (!view.IsBound () || !projection.IsBound ())
        return;
    if (view.numConstants != kCameraWindowConstants || projection.numConstants != kCameraWindowConstants)
        return;
    ++g_stats.drawsQualified;

    contextstate::ScopedInjectionGuard guard;
    if (!EnsureCreated (context))
        return;
    ResolveSome (context);

    // ⚠️ THE VERTEX SHADER IS ASKED FOR, NOT TRACKED. `VSSetShader` is not one of
    // the patched vtable slots, so the state tracker has always reported zero --
    // which threw away the strongest draw identifier there is and left the
    // signature leaning on render-target pointers. Adding the slot would change
    // the patch profile and force every pinned build to be re-pinned; asking the
    // context costs one COM call on a path that already makes several.
    {
        ID3D11VertexShader* bound = nullptr;
        context->VSGetShader (&bound, nullptr, nullptr);
        live.vertexShader = uint64_t (uintptr_t (bound));
        if (bound != nullptr)
            bound->Release ();
    }

    const uint64_t present = renderstate::CurrentPresentGeneration ();
    if (present != g_lastPresentSeen) {
        g_lastPresentSeen = present;
        ++g_stats.framesSeen;
    }
    // ⚠️ THE COVERAGE DENOMINATOR IS MODEL FRAMES. A camera group can only appear
    // on a frame where the model was actually re-rendered, so dividing by
    // Presents -- of which Archicad issues many per model frame -- punished the
    // real camera for Archicad's UI repaints and reported 61% for a group that
    // was present on 259 of 267 model frames.
    const uint64_t modelGeneration = renderstate::ModelSceneGeneration ();
    if (modelGeneration != g_lastModelSeen) {
        g_lastModelSeen = modelGeneration;
        ++g_stats.modelFramesSeen;

        // ⚠️ THE CENSUS CHOOSES FOR ITSELF, ON THE RENDER THREAD,
        // ONCE A SECOND'S WORTH OF MODEL FRAMES. `SelectCandidate` is pure
        // computation over a copy of the table -- no ACAPI, no allocation beyond
        // its own stack copy, no lock -- so the thread that scores the groups can
        // also be the one that picks. Attempting it on EVERY model frame would
        // copy 48 groups per frame for an answer that cannot change that fast.
        //
        // ⚠️ AND ONLY WHILE THERE IS NO SELECTION, which is what makes
        // it self-healing rather than twitchy: once locked this costs one branch,
        // and if the lock is ever lost it starts again with no user action.
        // ⚠️ THE COMPARISON IS AGAINST A COUNTER THAT RESTARTS, so
        // a marker AHEAD of it can only mean the counter was reset behind us.
        // Catching that here as well as in `ResetCounts` is deliberate: this is
        // the line that failed silently for two weeks.
        if (g_lastAutoSelectAttempt > g_stats.modelFramesSeen)
            g_lastAutoSelectAttempt = 0;
        if (g_autoSelect.load (std::memory_order_acquire) && !GetBindingStats ().fingerprintValid &&
            g_stats.modelFramesSeen >= g_lastAutoSelectAttempt + kAutoSelectEveryModelFrames) {
            g_lastAutoSelectAttempt = g_stats.modelFramesSeen;
            ++g_autoSelectAttempts;
            // ⚠️ THE AUTOMATIC PROMOTION AND THE EXPLICIT COMMAND
            // CALL THE SAME FUNCTION AND NOTHING ELSE. `SelectCandidate` commits
            // the fingerprint, the occurrence, the interpretation AND the
            // injection's camera source as one transaction; this used to add the
            // last of those by hand and the automatic path used to omit it.
            if (SelectCandidate ())
                ++g_stats.autoSelections;
        }
    }
    const renderstate::ScenePass pass = renderstate::CurrentScenePass ();

    // ---- which draw of THIS signature, within THIS model frame? ------------
    uint32_t occurrence = 0;
    {
        SignatureCounter* counter = nullptr;
        for (size_t i = 0; i < kSignatureCounters; ++i) {
            if (g_counters[i].used && SameTarget (g_counters[i], live)) {
                counter = &g_counters[i];
                break;
            }
        }
        if (counter == nullptr) {
            for (size_t i = 0; i < kSignatureCounters; ++i) {
                if (g_counters[i].used)
                    continue;
                counter = &g_counters[i];
                counter->used = true;
                counter->renderTargetWidth = live.renderTargetDesc.width;
                counter->renderTargetHeight = live.renderTargetDesc.height;
                counter->renderTargetFormat = live.renderTargetDesc.format;
                counter->renderTargetSamples = live.renderTargetDesc.sampleCount;
                counter->depthWidth = live.depthStencilDesc.width;
                counter->depthHeight = live.depthStencilDesc.height;
                counter->depthFormat = live.depthStencilDesc.format;
                counter->depthSamples = live.depthStencilDesc.sampleCount;
                counter->depthPresent = live.depthStencil != 0;
                counter->viewportWidth = live.viewportWidth;
                counter->viewportHeight = live.viewportHeight;
                counter->generation = modelGeneration;
                counter->count = 0;
                break;
            }
        }
        if (counter != nullptr) {
            if (counter->generation != modelGeneration) {
                counter->generation = modelGeneration;
                counter->count = 0;
            }
            occurrence = counter->count;
            ++counter->count;
        }
    }

    // ⚠️ BEFORE ANYTHING READS THE SELECTION, GIVE IT A CHANCE TO
    // FIND ITS RESOURCES AGAIN. Everything below -- the depth proof, the camera
    // snapshot -- tests the pinned pointers, so the re-acquisition has to happen
    // first or it would take an extra frame to have any effect.
    MaintainBinding (live, kind, indexCount, occurrence, modelGeneration);

    // ---- the depth proof, at the predicted last draw of this family --------
    if (MatchesSelectionSignature (live)) {
        if (g_selectionFrame != modelGeneration) {
            g_selectionFrame = modelGeneration;
            g_selectionOccurrencesLastFrame = g_selectionOccurrencesThisFrame;
            g_selectionOccurrencesThisFrame = 0;
        }
        ++g_selectionOccurrencesThisFrame;
        if (g_selectionOccurrencesLastFrame > 0 && occurrence + 1 == g_selectionOccurrencesLastFrame) {
            ID3D11DeviceContext1* context1 = nullptr;
            if (SUCCEEDED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), (void**) &context1)) &&
                context1 != nullptr) {
                injection::probes::DrawDepthProof (context, context1);
                context1->Release ();
            }
        }
    }

    // ⚠️ THE LOCKED GROUP FEEDS THE SNAPSHOT DIRECTLY, AND THIS IS THE ONLY LINE
    // IN THE CENSUS THAT AFFECTS WHAT IS DRAWN. Before phase A commits there is
    // no selection and this does nothing; after it, ONLY draws of the chosen
    // group become the camera Present uses.
    if (injection::GetCameraSource () == injection::CameraSource::CensusSelectedGroup &&
        MatchesSelection (live, occurrence)) {
        contextstate::SceneDrawState draw;
        draw.valid = true;
        draw.modelSceneGeneration = modelGeneration;
        draw.scenePassGeneration = pass.generation;
        draw.sceneTargetEpoch = pass.targetEpoch;
        draw.drawSequence = pass.drawsThisEpoch;
        draw.vertexShader = live.vertexShader;
        draw.renderTarget = live.renderTarget;
        draw.depthStencil = live.depthStencil;
        draw.viewportX = live.viewportX;
        draw.viewportY = live.viewportY;
        draw.viewportWidth = live.viewportWidth;
        draw.viewportHeight = live.viewportHeight;
        for (size_t i = 0; i < contextstate::kConstantBufferSlots; ++i)
            draw.vsConstantBuffers[i] = live.vsConstantBuffers[i];
        injection::SnapshotSelectedDraw (context, draw, GetSelection ().groupId);
        NoteSnapshot ();
    }

    Slot* found = nullptr;
    for (size_t i = 0; i < kGroupCapacity; ++i) {
        if (g_slots[i].used && Matches (g_slots[i].group, live, occurrence)) {
            found = &g_slots[i];
            break;
        }
    }
    if (found == nullptr) {
        for (size_t i = 0; i < kGroupCapacity; ++i) {
            if (g_slots[i].used)
                continue;
            found = &g_slots[i];
            found->used = true;
            Group& fresh = found->group;
            fresh.groupId = uint32_t (i + 1);
            fresh.occurrenceIndex = occurrence;
            fresh.vertexShader = live.vertexShader;
            fresh.renderTarget = live.renderTarget;
            fresh.depthStencil = live.depthStencil;
            fresh.viewportX = live.viewportX;
            fresh.viewportY = live.viewportY;
            fresh.viewportWidth = live.viewportWidth;
            fresh.viewportHeight = live.viewportHeight;
            // ⚠️ THE KEY FIELDS ARE SET AT CREATION, NOT ONLY
            // REFRESHED LATER. `Matches` reads them on the very next draw; a
            // group created with zeroes would never match itself and would mint
            // a new slot every frame -- the overflow this change exists to end.
            fresh.renderTargetWidth = live.renderTargetDesc.width;
            fresh.renderTargetHeight = live.renderTargetDesc.height;
            fresh.renderTargetFormat = live.renderTargetDesc.format;
            fresh.renderTargetSamples = live.renderTargetDesc.sampleCount;
            fresh.depthPresent = live.depthStencil != 0;
            fresh.depthWidth = live.depthStencilDesc.width;
            fresh.depthHeight = live.depthStencilDesc.height;
            fresh.depthFormat = live.depthStencilDesc.format;
            fresh.depthSamples = live.depthStencilDesc.sampleCount;
            fresh.viewBuffer = view.buffer;
            fresh.viewNumConstants = view.numConstants;
            fresh.projectionBuffer = projection.buffer;
            fresh.projectionNumConstants = projection.numConstants;
            fresh.firstPresent = present;
            fresh.passGeneration = pass.generation;
            fresh.drawSequenceFirst = pass.drawsThisEpoch;
            ++g_stats.groupsUsed;
            break;
        }
    }
    if (found == nullptr) {
        ++g_stats.groupsOverflowed;
        return;
    }

    Group& group = found->group;
    ++group.drawsObserved;
    group.lastPresent = present;
    group.passGeneration = pass.generation;
    group.targetEpoch = pass.targetEpoch;
    group.drawSequenceLast = pass.drawsThisEpoch;
    group.drawKindMask |= (1u << uint32_t (kind));
    group.lastIndexCount = indexCount;

    // ⚠️ AND THE POINTERS TRACK THE NEWEST DRAW, BECAUSE A GROUP NOW
    // OUTLIVES THEM. The key is logical, so one group spans every view
    // generation Archicad builds for that camera; the addresses recorded at
    // creation are the FIRST generation's and may be dead by the time phase A
    // selects. Keeping the latest means the selection pins to something alive
    // and the re-acquisition path is a repair rather than a routine.
    group.renderTarget = live.renderTarget;
    group.depthStencil = live.depthStencil;
    group.viewBuffer = view.buffer;
    group.projectionBuffer = projection.buffer;
    group.vertexShader = live.vertexShader;

    // ⚠️ REFRESHED EVERY DRAW, NOT CAPTURED AT CREATION. A group can
    // be created on a draw whose targets have not been described yet -- the
    // tracker resolves a description when the bound view CHANGES -- and a
    // fingerprint built from zeroes would match nothing at all.
    group.renderTargetWidth = live.renderTargetDesc.width;
    group.renderTargetHeight = live.renderTargetDesc.height;
    group.renderTargetFormat = live.renderTargetDesc.format;
    group.renderTargetSamples = live.renderTargetDesc.sampleCount;
    group.depthPresent = live.depthStencil != 0;
    group.depthWidth = live.depthStencilDesc.width;
    group.depthHeight = live.depthStencilDesc.height;
    group.depthFormat = live.depthStencilDesc.format;
    group.depthSamples = live.depthStencilDesc.sampleCount;
    group.viewFirstConstant = view.firstConstant;
    group.projectionFirstConstant = projection.firstConstant;
    if (found->lastPresentCounted != present) {
        found->lastPresentCounted = present;
        ++group.framesObserved;
    }
    if (found->lastModelCounted != modelGeneration) {
        found->lastModelCounted = modelGeneration;
        ++group.modelFramesObserved;
    }

    // ⚠️ RECORDED AND NOT USED. If a group turns out to have plausible x/y and
    // impossible depth, this is the first thing to look at -- but only after the
    // group itself is identified.
    const contextstate::ConstantBufferBinding& b0 = live.vsConstantBuffers[0];
    group.b0Bound = b0.IsBound ();
    group.b0Buffer = b0.buffer;
    group.b0FirstConstant = b0.firstConstant;
    group.b0NumConstants = b0.numConstants;

    // One outstanding copy per group: sampling every draw would queue readbacks
    // faster than the GPU retires them and measure nothing extra.
    if (found->copyPending)
        return;

    D3D11_BOX box = {};
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    box.left = view.ByteOffset ();
    box.right = box.left + kWindowBytes;
    context->CopySubresourceRegion (found->stagingView, 0, 0, 0, 0,
                                    reinterpret_cast<ID3D11Buffer*> (uintptr_t (view.buffer)), 0, &box);
    box.left = projection.ByteOffset ();
    box.right = box.left + kWindowBytes;
    context->CopySubresourceRegion (found->stagingProjection, 0, 0, 0, 0,
                                    reinterpret_cast<ID3D11Buffer*> (uintptr_t (projection.buffer)), 0, &box);
    found->copyPending = true;
    ++g_stats.copiesIssued;
}

void Shutdown ()
{
    g_enabled.store (false, std::memory_order_release);
    for (size_t i = 0; i < kGroupCapacity; ++i) {
        ReleaseAndNull (g_slots[i].stagingView);
        ReleaseAndNull (g_slots[i].stagingProjection);
        g_slots[i] = Slot {};
    }
    ReleaseAndNull (g_device);
    g_created = false;
    g_createFailed = false;
    g_stats = Stats {};
    ShutdownRecognizer ();
}

size_t CopyGroups (Group* out, size_t capacity)
{
    if (out == nullptr || capacity == 0)
        return 0;
    size_t written = 0;
    for (size_t i = 0; i < kGroupCapacity && written < capacity; ++i) {
        Slot& slot = g_slots[i];
        if (!slot.used)
            continue;
        Group group = slot.group;

        // ⚠️ THE WINNER IS THE INTERPRETATION THAT WAS VALID MOST OFTEN, and only
        // then the one with the smallest mean error. Ranking by error first is
        // what let a degenerate product win on a median of zero while never once
        // putting the anchor inside the clip volume.
        uint32_t bestVariant = 0;
        uint32_t bestValid = 0;
        double bestMean = 0.0;
        for (size_t variant = 0; variant < kVariantCount; ++variant) {
            const uint32_t valid = group.variantValid[variant];
            if (valid == 0)
                continue;
            const double mean = slot.variantErrorSum[variant] / double (valid);
            if (valid > bestValid || (valid == bestValid && mean < bestMean)) {
                bestVariant = uint32_t (variant);
                bestValid = valid;
                bestMean = mean;
            }
        }
        group.winningVariant = bestVariant;
        group.winningVariantValid = bestValid;
        group.medianCentreError = Median (slot);
        group.meanCentreError = slot.errorCount > 0 ? float (slot.errorSum / double (slot.errorCount)) : 0.0f;
        group.meanSpreadPixels = slot.spreadCount > 0 ? float (slot.spreadSum / double (slot.spreadCount)) : 0.0f;
        group.errorSamples = slot.errorCount;
        group.medianAreaPixels = MedianOf (slot.areas, slot.triangleCount);
        group.medianMaxEdgePixels = MedianOf (slot.edges, slot.triangleCount);
        out[written] = group;
        ++written;
    }
    return written;
}

// ⚠️ PHASE A'S DECISION, MADE FROM A COPIED TABLE. The census
// owns the measurements and hands them over; the recognizer owns the choice. It
// still fails closed -- no eligible group means no selection and no injection.
void SetAutoSelect (bool enabled)
{
    // Arming starts a fresh search: a marker from a previous session would put
    // the first attempt an unbounded number of frames into the future.
    if (enabled)
        g_lastAutoSelectAttempt = 0;
    g_autoSelect.store (enabled, std::memory_order_release);
}

bool AutoSelect ()
{
    return g_autoSelect.load (std::memory_order_acquire);
}

bool SelectCandidate ()
{
    Group groups[kGroupCapacity];
    const size_t count = CopyGroups (groups, kGroupCapacity);
    return SelectCandidate (groups, count, g_stats.modelFramesSeen);
}

Stats GetStats ()
{
    // ⚠️ THE BINDING COUNTERS ARE REPORTED THROUGH THE CENSUS because
    // that is the verb every report already calls. They are MEASURED by the
    // recognizer, which is why they are copied here rather than kept here.
    Stats stats = g_stats;
    const BindingStats binding = GetBindingStats ();
    stats.selectionMatches = binding.selectionMatches;
    stats.logicalMatches = binding.logicalMatches;
    stats.rebinds = binding.rebinds;
    stats.rebindsRefused = binding.rebindsRefused;
    stats.resizeRelearns = binding.resizeRelearns;
    stats.fingerprintValid = binding.fingerprintValid;
    stats.eligibleCandidates = EligibleCandidates ();
    stats.autoSelectAttempts = g_autoSelectAttempts;
    stats.lifecycle =
        LifecycleName (GetLifecycle (g_enabled.load (std::memory_order_acquire), renderstate::ModelSceneGeneration ()));
    return stats;
}

} // namespace census
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
