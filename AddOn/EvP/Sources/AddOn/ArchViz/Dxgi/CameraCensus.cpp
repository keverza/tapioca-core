// ArchViz/Dxgi/CameraCensus -- see the header. Every rule about this file is in
// that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/CameraCensus.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/InjectionOracle.hpp"
#include "ArchViz/Dxgi/InjectionRenderer.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#include <d3d11_1.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace census {

namespace {

constexpr UINT   kWindowBytes = 256;
constexpr UINT   kCameraWindowConstants = 16;
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
    bool     copyPending = false;

    double   spreadSum = 0.0;
    uint32_t spreadCount = 0;
    float    errors[kErrorSamples] = {};
    uint32_t errorCount = 0;
    uint32_t errorNext = 0;
    double   errorSum = 0.0;
    double   variantErrorSum[kVariantCount] = {};
    float    areas[kErrorSamples] = {};
    float    edges[kErrorSamples] = {};
    uint32_t triangleCount = 0;
    uint32_t triangleNext = 0;

    uint64_t lastPresentCounted = 0;
    uint64_t lastModelCounted = 0;
    bool     used = false;
};

ID3D11Device* g_device = nullptr;
Slot          g_slots[kGroupCapacity];
Stats         g_stats;
std::atomic<bool> g_enabled {false};
bool g_created = false;
bool g_createFailed = false;
uint64_t g_lastPresentSeen = 0;
uint64_t g_lastModelSeen = 0;

// ⚠️ THE OCCURRENCE INDEX BELONGS TO THE SIGNATURE, NOT TO THE TABLE. Each
// distinct signature draws several times per model frame, and "which of them" is
// counted per signature and reset when the model generation changes. A global
// counter would number draws across different signatures and mean nothing.
constexpr size_t kSignatureCounters = 16;
struct SignatureCounter {
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    uint64_t depthStencil = 0;
    uint64_t generation = 0;
    uint32_t count = 0;
    bool     used = false;
};
SignatureCounter g_counters[kSignatureCounters];

Selection g_selection;
Eligibility g_eligibility;

template <typename T>
void ReleaseAndNull (T*& object)
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
        ok = ok && SUCCEEDED (g_device->CreateBuffer (&desc, nullptr,
                &g_slots[i].stagingProjection));
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
bool Matches (const Group& group, const contextstate::ContextState& live,
              uint32_t occurrence)
{
    return group.occurrenceIndex == occurrence &&
           group.vertexShader == live.vertexShader &&
           group.renderTarget == live.renderTarget &&
           group.depthStencil == live.depthStencil &&
           SameExtent (group.viewportWidth, live.viewportWidth) &&
           SameExtent (group.viewportHeight, live.viewportHeight) &&
           SameExtent (group.viewportX, live.viewportX) &&
           SameExtent (group.viewportY, live.viewportY) &&
           group.viewBuffer == live.vsConstantBuffers[1].buffer &&
           group.projectionBuffer == live.vsConstantBuffers[2].buffer &&
           group.viewNumConstants == live.vsConstantBuffers[1].numConstants &&
           group.projectionNumConstants == live.vsConstantBuffers[2].numConstants;
}

// The locked group's signature, tested against what is bound right now. Same
// fields as `Matches`, for the same reasons -- and `firstConstant` is absent from
// both because Archicad's ring window advances every frame by design.
bool MatchesSelection (const contextstate::ContextState& live, uint32_t occurrence)
{
    if (!g_selection.valid)
        return false;
    return g_selection.occurrenceIndex == occurrence &&
           g_selection.vertexShader == live.vertexShader &&
           g_selection.renderTarget == live.renderTarget &&
           g_selection.depthStencil == live.depthStencil &&
           SameExtent (g_selection.viewportWidth, live.viewportWidth) &&
           SameExtent (g_selection.viewportHeight, live.viewportHeight) &&
           SameExtent (g_selection.viewportX, live.viewportX) &&
           SameExtent (g_selection.viewportY, live.viewportY) &&
           g_selection.viewBuffer == live.vsConstantBuffers[1].buffer &&
           g_selection.projectionBuffer == live.vsConstantBuffers[2].buffer &&
           g_selection.viewNumConstants == live.vsConstantBuffers[1].numConstants &&
           g_selection.projectionNumConstants == live.vsConstantBuffers[2].numConstants;
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
    HRESULT hr = context->Map (slot.stagingView, 0, D3D11_MAP_READ,
                               D3D11_MAP_FLAG_DO_NOT_WAIT, &viewMap);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++g_stats.readbacksBusy;
        return;
    }
    if (FAILED (hr) || viewMap.pData == nullptr) {
        slot.copyPending = false;
        return;
    }
    hr = context->Map (slot.stagingProjection, 0, D3D11_MAP_READ,
                       D3D11_MAP_FLAG_DO_NOT_WAIT, &projectionMap);
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

}   // namespace

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
    for (size_t i = 0; i < kSignatureCounters; ++i)
        g_counters[i] = SignatureCounter {};
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
    const contextstate::ConstantBufferBinding& view = live.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = live.vsConstantBuffers[2];

    // ⚠️ THE ONLY ADMISSION TEST, AND THE LEARNER HAS NO PART IN IT. Both camera
    // windows, bound, at the 256-byte shape stage 3 identified -- whatever pass
    // the draw belongs to.
    if (!view.IsBound () || !projection.IsBound ())
        return;
    if (view.numConstants != kCameraWindowConstants ||
        projection.numConstants != kCameraWindowConstants)
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
    }
    const renderstate::ScenePass pass = renderstate::CurrentScenePass ();

    // ---- which draw of THIS signature, within THIS model frame? ------------
    uint32_t occurrence = 0;
    {
        SignatureCounter* counter = nullptr;
        for (size_t i = 0; i < kSignatureCounters; ++i) {
            if (g_counters[i].used && g_counters[i].vertexShader == live.vertexShader &&
                g_counters[i].renderTarget == live.renderTarget &&
                g_counters[i].depthStencil == live.depthStencil) {
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
                counter->vertexShader = live.vertexShader;
                counter->renderTarget = live.renderTarget;
                counter->depthStencil = live.depthStencil;
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
        injection::SnapshotSelectedDraw (context, draw, g_selection.groupId);
        ++g_selection.snapshotsTaken;
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
    g_selection = Selection {};
}

bool Qualifies (const Group& group, const Slot& slot, uint64_t modelFrames,
                float& coverage, float& insideClip, float& agreement)
{
    coverage = modelFrames > 0
            ? float (double (group.modelFramesObserved) / double (modelFrames)) : 0.0f;
    insideClip = group.samplesScored > 0
            ? float (double (group.anchorInside) / double (group.samplesScored)) : 0.0f;
    agreement = group.samplesScored > 0
            ? float (double (group.trianglesFinite) / double (group.samplesScored)) : 0.0f;
    if (group.samplesScored < g_eligibility.minSamples)
        return false;
    if (coverage < g_eligibility.minModelCoverage)
        return false;
    if (insideClip < g_eligibility.minInsideClip)
        return false;
    if (agreement < g_eligibility.minFiniteTriangles)
        return false;

    // ⚠️ THE TWO GATES A COLLAPSE CANNOT PASS. Everything above this line was
    // satisfied for six runs by a transform that drew one pixel.
    const float area = MedianOf (slot.areas, slot.triangleCount);
    const float edge = MedianOf (slot.edges, slot.triangleCount);
    if (area < g_eligibility.minMedianAreaPixels)
        return false;
    if (edge < g_eligibility.minMedianMaxEdgePixels)
        return false;

    // Centre error is the weak term: a human hand on a mouse does not put the
    // orbit target on the anchor to the pixel.
    return !(slot.errorCount > 0 &&
             group.medianCentreError > g_eligibility.maxMedianCentreError);
}

bool SelectCandidate ()
{
    Group groups[kGroupCapacity];
    const size_t count = CopyGroups (groups, kGroupCapacity);
    const uint64_t modelFrames = g_stats.modelFramesSeen;

    // ⚠️ ELIGIBILITY FIRST, RANK SECOND, AND NEVER THE OTHER WAY. Ranking by
    // error alone put a group with ONE sample and no coverage ahead of the real
    // model camera, which carried 1497 draws across 259 model frames.
    const Group* best = nullptr;
    float bestCoverage = 0.0f;
    float bestInside = 0.0f;
    float bestMedian = 0.0f;
    for (size_t i = 0, slotIndex = 0; i < count; ++i) {
        while (slotIndex < kGroupCapacity && !g_slots[slotIndex].used)
            ++slotIndex;
        if (slotIndex >= kGroupCapacity)
            break;
        const Slot& slot = g_slots[slotIndex];
        ++slotIndex;

        float coverage = 0.0f;
        float insideClip = 0.0f;
        float agreement = 0.0f;
        if (!Qualifies (groups[i], slot, modelFrames, coverage, insideClip, agreement))
            continue;
        const bool better =
                best == nullptr ||
                coverage > bestCoverage + 0.01f ||
                (coverage >= bestCoverage - 0.01f && insideClip > bestInside + 0.005f) ||
                (coverage >= bestCoverage - 0.01f && insideClip >= bestInside - 0.005f &&
                 groups[i].medianCentreError < bestMedian);
        if (better) {
            best = &groups[i];
            bestCoverage = coverage;
            bestInside = insideClip;
            bestMedian = groups[i].medianCentreError;
        }
    }
    if (best == nullptr) {
        // ⚠️ FAIL CLOSED. A run that could not identify the camera injects
        // nothing, rather than injecting with whatever drew last -- which is the
        // behaviour that produced six inconclusive runs.
        g_selection = Selection {};
        return false;
    }

    Selection chosen;
    chosen.valid = true;
    // ⚠️ A RUN-LOCAL ID, WHICH IS ALL A GROUP IDENTITY MAY BE. It exists so every
    // injection row can name the group it came from; it means nothing in the next
    // session and is never written anywhere that outlives one.
    chosen.groupId = best->groupId;
    chosen.occurrenceIndex = best->occurrenceIndex;
    chosen.vertexShader = best->vertexShader;
    chosen.renderTarget = best->renderTarget;
    chosen.depthStencil = best->depthStencil;
    chosen.viewportX = best->viewportX;
    chosen.viewportY = best->viewportY;
    chosen.viewportWidth = best->viewportWidth;
    chosen.viewportHeight = best->viewportHeight;
    chosen.viewBuffer = best->viewBuffer;
    chosen.viewNumConstants = best->viewNumConstants;
    chosen.projectionBuffer = best->projectionBuffer;
    chosen.projectionNumConstants = best->projectionNumConstants;
    chosen.variant = best->winningVariant;
    chosen.samples = best->samplesScored;
    chosen.modelCoverage = bestCoverage;
    chosen.insideClip = bestInside;
    chosen.medianCentreError = best->medianCentreError;
    chosen.medianAreaPixels = best->medianAreaPixels;
    chosen.medianMaxEdgePixels = best->medianMaxEdgePixels;
    g_selection = chosen;
    // ⚠️ THE SHADER IS TOLD WHAT WAS LEARNED, AND REFUSES IF IT CANNOT HONOUR IT.
    // This is the link that was missing for run thirty-three: the census proved
    // `p * V * Pt` twice over while the shader rendered `p * V * P`, and nothing
    // in the pipeline compared the two.
    injection::SetExpectedInterpretation (chosen.variant);
    // ⚠️ THE OCCURRENCE TRAVELS WITH THE SIGNATURE. They are one identity, and
    // handing over only half of it is what let the snapshot take whichever draw
    // of the family came last.
    injection::SetSelectedOccurrence (chosen.occurrenceIndex);
    return true;
}

void ClearSelection ()
{
    g_selection = Selection {};
    injection::SetExpectedInterpretation (0xffffffffu);
}

Selection GetSelection ()
{
    return g_selection;
}

Eligibility GetEligibility ()
{
    return g_eligibility;
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
        double   bestMean = 0.0;
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
        group.meanCentreError = slot.errorCount > 0
                ? float (slot.errorSum / double (slot.errorCount)) : 0.0f;
        group.meanSpreadPixels = slot.spreadCount > 0
                ? float (slot.spreadSum / double (slot.spreadCount)) : 0.0f;
        group.medianAreaPixels = MedianOf (slot.areas, slot.triangleCount);
        group.medianMaxEdgePixels = MedianOf (slot.edges, slot.triangleCount);
        out[written] = group;
        ++written;
    }
    return written;
}

Stats GetStats ()
{
    return g_stats;
}

}   // namespace census
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv
