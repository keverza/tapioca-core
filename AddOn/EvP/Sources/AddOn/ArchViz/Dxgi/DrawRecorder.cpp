// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/Dxgi/DrawRecorder -- see the header for what is recorded and why.

#include "ArchViz/Dxgi/DrawRecorder.hpp"

#include "ArchViz/Dxgi/CameraCensus.hpp"
#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/RenderStateCapture.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace drawrecorder {

namespace {

constexpr uint32_t kBytesPerWindow = uint32_t (kFloatsPerWindow * sizeof (float));
constexpr uint32_t kRegionBytes = uint32_t (kMaxDraws * kWindows) * kBytesPerWindow;
constexpr uint32_t kStagingBytes = 2 * kRegionBytes; // the copies before the draws, then after them
constexpr uint32_t kCameraWindowConstants = 16;      // the census's admission shape

std::atomic<uint32_t> g_state { uint32_t (State::Idle) };
std::atomic<uint32_t> g_framesWanted { 0 };
std::atomic<uint32_t> g_framesSeen { 0 };
std::atomic<uint32_t> g_reserved { 0 };
std::atomic<int32_t> g_inFlight { 0 };    // draws between `OnDraw` and their second copy
std::atomic<int32_t> g_presentBusy { 0 }; // a Present inside the readback
std::atomic<uint32_t> g_dropped { 0 };
std::atomic<uint32_t> g_readbacksPending { 0 };
std::atomic<uint32_t> g_readbackFailures { 0 };
std::atomic<uint32_t> g_createFailures { 0 };
std::atomic<uint32_t> g_recordCount { 0 };
std::atomic<uint64_t> g_captures { 0 };

// Armed for a moving camera (`Arm`): the model redraws to wait for, the model
// generation the render thread first saw after arming, and whether it has since
// advanced far enough. The generation is the render thread's to read.
constexpr uint64_t kGenerationUnset = ~uint64_t (0);
std::atomic<uint32_t> g_modelFramesWanted { 0 };
std::atomic<uint64_t> g_armGeneration { kGenerationUnset };
std::atomic<bool> g_moving { false };

DrawRecord g_records[kMaxDraws];
// Render thread creates it, the Present thread maps it, `Shutdown` releases it
// with the hooks out -- the same division `CameraAgreement` has run with.
ID3D11Buffer* g_staging = nullptr;

// The record this thread's current draw took, until its second copy; -1 for none.
// The draw, `OnDraw` and `OnDrawCompleted` all run on the thread that issues it.
thread_local int32_t t_open = -1;

// Ends the record's time in flight. Also called for a draw whose detour never
// reported completion, so a missed second copy cannot hold the readback forever.
void CloseOpen ()
{
    if (t_open < 0)
        return;
    t_open = -1;
    g_inFlight.fetch_sub (1, std::memory_order_acq_rel);
}

uint64_t Id (const void* object)
{
    return uint64_t (uintptr_t (object));
}

bool EnsureStaging (ID3D11DeviceContext* context)
{
    if (g_staging != nullptr)
        return true;
    ID3D11Device* device = nullptr;
    context->GetDevice (&device);
    if (device == nullptr)
        return false;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kStagingBytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const bool ok = SUCCEEDED (device->CreateBuffer (&desc, nullptr, &g_staging)) && g_staging != nullptr;
    device->Release ();
    return ok;
}

// The texture behind a view: identity and description, every reference returned.
void Describe (ID3D11View* view, uint64_t& resource, uint32_t& width, uint32_t& height, uint32_t& format,
               uint32_t* samples)
{
    ID3D11Resource* owner = nullptr;
    view->GetResource (&owner);
    if (owner == nullptr)
        return;
    resource = Id (owner);
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED (owner->QueryInterface (__uuidof (ID3D11Texture2D), reinterpret_cast<void**> (&texture))) &&
        texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc (&desc);
        width = desc.Width;
        height = desc.Height;
        format = uint32_t (desc.Format);
        if (samples != nullptr)
            *samples = desc.SampleDesc.Count;
        texture->Release ();
    }
    owner->Release ();
}

// One bound window's first 64 bytes, clamped to its buffer, to `at` in the staging
// buffer. The caller's `BoundConstants` holds the buffer until this is issued.
uint32_t CopyWindow (ID3D11DeviceContext* context, const contextstate::ConstantBufferBinding& binding,
                     uint32_t bufferBytes, UINT at)
{
    const uint32_t offset = binding.ByteOffset ();
    if (g_staging == nullptr || offset >= bufferBytes)
        return 0;
    const uint32_t bytes = bufferBytes - offset < kBytesPerWindow ? bufferBytes - offset : kBytesPerWindow;
    D3D11_BOX box = {};
    box.left = offset;
    box.right = offset + bytes;
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    context->CopySubresourceRegion (g_staging, 0, at, 0, 0,
                                    reinterpret_cast<ID3D11Buffer*> (uintptr_t (binding.buffer)), 0, &box);
    return bytes;
}

uint32_t BufferBytes (const contextstate::ConstantBufferBinding& binding)
{
    D3D11_BUFFER_DESC desc = {};
    reinterpret_cast<ID3D11Buffer*> (uintptr_t (binding.buffer))->GetDesc (&desc);
    return desc.ByteWidth;
}

Reason Classify (bool censusEnabled, const contextstate::ContextState& bound, bool boundValid)
{
    const contextstate::ConstantBufferBinding& view = bound.vsConstantBuffers[1];
    const contextstate::ConstantBufferBinding& projection = bound.vsConstantBuffers[2];
    if (!censusEnabled)
        return Reason::CensusOff;
    if (!boundValid || !view.IsBound ())
        return Reason::ViewUnbound;
    if (!projection.IsBound ())
        return Reason::ProjectionUnbound;
    if (view.numConstants != kCameraWindowConstants)
        return Reason::ViewWindow;
    if (projection.numConstants != kCameraWindowConstants)
        return Reason::ProjectionWindow;
    return Reason::Admissible;
}

void Record (ID3D11DeviceContext* context, DrawRecord& record, uint32_t index, uint32_t kind, uint32_t count,
             uint32_t instances)
{
    record = DrawRecord {};
    record.sequence = index;
    record.frame = g_framesSeen.load (std::memory_order_relaxed);
    record.kind = kind;
    record.count = count;
    record.instances = instances;
    record.thread = uint32_t (GetCurrentThreadId ());
    record.context = Id (context);

    const renderstate::ScenePass pass = renderstate::CurrentScenePass ();
    const renderstate::SceneSignature signature = renderstate::GetSceneSignature ();
    record.scenePass = pass.generation;
    record.passColour = pass.colorResource;
    record.passBoundaryHit = pass.boundaryHit;
    record.signatureColour = signature.colorResource;
    record.signatureLearned = signature.learned;
    record.modelGeneration = renderstate::ModelSceneGeneration ();
    record.censusEnabled = census::Enabled ();

    // Our queries and copies are not Archicad's work: the copy slot is hooked.
    contextstate::ScopedInjectionGuard guard;

    ID3D11VertexShader* vertexShader = nullptr;
    context->VSGetShader (&vertexShader, nullptr, nullptr);
    if (vertexShader != nullptr) {
        record.vertexShader = Id (vertexShader);
        vertexShader->Release ();
    }
    ID3D11PixelShader* pixelShader = nullptr;
    context->PSGetShader (&pixelShader, nullptr, nullptr);
    if (pixelShader != nullptr) {
        record.pixelShader = Id (pixelShader);
        pixelShader->Release ();
    }

    ID3D11RenderTargetView* colour = nullptr;
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets (1, &colour, &depth);
    if (colour != nullptr) {
        record.renderTarget = Id (colour);
        Describe (colour, record.renderResource, record.renderWidth, record.renderHeight, record.renderFormat,
                  &record.renderSamples);
        colour->Release ();
    }
    if (depth != nullptr) {
        record.depthView = Id (depth);
        Describe (depth, record.depthResource, record.depthWidth, record.depthHeight, record.depthFormat, nullptr);
        depth->Release ();
    }

    D3D11_VIEWPORT viewport = {};
    UINT viewports = 1;
    context->RSGetViewports (&viewports, &viewport);
    if (viewports > 0) {
        record.viewport[0] = viewport.TopLeftX;
        record.viewport[1] = viewport.TopLeftY;
        record.viewport[2] = viewport.Width;
        record.viewport[3] = viewport.Height;
    }

    // b0..b3 as the context has them; `bound` holds each buffer until the copies
    // out of it have been issued.
    const contextstate::BoundConstants bound (context);
    contextstate::ContextState live;
    bound.ApplyTo (live);
    record.reason = Classify (record.censusEnabled, live, bound.Valid ());
    if (!bound.Valid ())
        return;
    const bool staging = EnsureStaging (context);
    if (!staging)
        g_createFailures.fetch_add (1, std::memory_order_relaxed);
    for (size_t slot = 0; slot < kWindows; ++slot) {
        const contextstate::ConstantBufferBinding& binding = live.vsConstantBuffers[slot];
        if (!binding.IsBound ())
            continue;
        record.buffer[slot] = binding.buffer; // held by `bound`
        record.bufferBytes[slot] = BufferBytes (binding);
        record.firstConstant[slot] = binding.firstConstant;
        record.numConstants[slot] = binding.numConstants;
        if (staging)
            record.bytesCopied[slot] = CopyWindow (context, binding, record.bufferBytes[slot],
                                                   UINT ((index * kWindows + slot) * kBytesPerWindow));
    }
}

// The second copy, once the draw returned: the slots as the context has them now,
// into the second half of the staging buffer.
void RecordAfter (ID3D11DeviceContext* context, DrawRecord& record, uint32_t index)
{
    contextstate::ScopedInjectionGuard guard;
    const contextstate::BoundConstants bound (context);
    contextstate::ContextState live;
    bound.ApplyTo (live);
    if (!bound.Valid ())
        return;
    for (size_t slot = 0; slot < kWindows; ++slot) {
        const contextstate::ConstantBufferBinding& binding = live.vsConstantBuffers[slot];
        record.bindingKept[slot] = binding.buffer == record.buffer[slot] &&
                                   binding.firstConstant == record.firstConstant[slot] &&
                                   binding.numConstants == record.numConstants[slot];
        if (binding.IsBound ())
            record.bytesCopiedAfter[slot] =
                CopyWindow (context, binding, BufferBytes (binding),
                            UINT (kRegionBytes + (index * kWindows + slot) * kBytesPerWindow));
    }
}

// RENDER THREAD, while armed for a moving camera: the model generation advances
// only when Archicad redraws the model, and a still camera does not.
void NoteModelFrame ()
{
    const uint32_t wanted = g_modelFramesWanted.load (std::memory_order_relaxed);
    if (wanted == 0 || g_moving.load (std::memory_order_relaxed))
        return;
    const uint64_t generation = renderstate::ModelSceneGeneration ();
    const uint64_t armed = g_armGeneration.load (std::memory_order_relaxed);
    if (armed == kGenerationUnset)
        g_armGeneration.store (generation, std::memory_order_relaxed);
    else if (generation >= armed + wanted)
        g_moving.store (true, std::memory_order_release);
}

void Unpack (const uint8_t* from, uint32_t copied, float* window)
{
    std::memset (window, 0, sizeof (float) * kFloatsPerWindow);
    if (copied > 0)
        std::memcpy (window, from, copied < kBytesPerWindow ? copied : kBytesPerWindow);
}

// PRESENT THREAD, with the capture closed and every draw drained: one
// DO_NOT_WAIT readback. False while the GPU is still on the copies.
bool ReadBack (IDXGISwapChain* swapChain, uint32_t records)
{
    if (records == 0 || g_staging == nullptr)
        return true;
    ID3D11Device* device = nullptr;
    swapChain->GetDevice (__uuidof (ID3D11Device), reinterpret_cast<void**> (&device));
    if (device == nullptr)
        return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext (&context);
    device->Release ();
    if (context == nullptr)
        return false;
    contextstate::ScopedInjectionGuard guard; // our Map is not Archicad's either
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT hr = context->Map (g_staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        context->Release ();
        g_readbacksPending.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    if (SUCCEEDED (hr) && mapped.pData != nullptr) {
        const uint8_t* const bytes = static_cast<const uint8_t*> (mapped.pData);
        for (uint32_t i = 0; i < records; ++i) {
            DrawRecord& record = g_records[i];
            for (size_t slot = 0; slot < kWindows; ++slot) {
                const size_t at = (size_t (i) * kWindows + slot) * kBytesPerWindow;
                Unpack (bytes + at, record.bytesCopied[slot], record.window[slot]);
                Unpack (bytes + kRegionBytes + at, record.bytesCopiedAfter[slot], record.windowAfter[slot]);
            }
        }
        context->Unmap (g_staging, 0);
    }
    else {
        g_readbackFailures.fetch_add (1, std::memory_order_relaxed);
    }
    context->Release ();
    return true;
}

void WaitForQuiet ()
{
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (g_inFlight.load (std::memory_order_acquire) == 0 && g_presentBusy.load (std::memory_order_acquire) == 0)
            return;
        Sleep (1);
    }
}

} // namespace

void Arm (uint32_t frames, uint32_t afterModelFrames)
{
    // Nothing may still be writing the records this is about to clear.
    g_state.store (uint32_t (State::Idle), std::memory_order_release);
    WaitForQuiet ();
    g_modelFramesWanted.store (afterModelFrames, std::memory_order_relaxed);
    g_armGeneration.store (kGenerationUnset, std::memory_order_relaxed);
    g_moving.store (false, std::memory_order_relaxed);
    g_framesWanted.store (frames < 1 ? 1 : (frames > kMaxFrames ? kMaxFrames : frames), std::memory_order_relaxed);
    g_framesSeen.store (0, std::memory_order_relaxed);
    g_reserved.store (0, std::memory_order_relaxed);
    g_dropped.store (0, std::memory_order_relaxed);
    g_readbacksPending.store (0, std::memory_order_relaxed);
    g_readbackFailures.store (0, std::memory_order_relaxed);
    g_createFailures.store (0, std::memory_order_relaxed);
    g_recordCount.store (0, std::memory_order_relaxed);
    g_state.store (uint32_t (State::Armed), std::memory_order_release);
}

void Disarm ()
{
    g_state.store (uint32_t (State::Idle), std::memory_order_release);
    WaitForQuiet ();
}

Status GetStatus ()
{
    Status status;
    status.state = State (g_state.load (std::memory_order_acquire));
    status.modelFramesWanted = g_modelFramesWanted.load (std::memory_order_relaxed);
    status.framesWanted = g_framesWanted.load (std::memory_order_relaxed);
    status.framesSeen = g_framesSeen.load (std::memory_order_relaxed);
    const uint32_t reserved = g_reserved.load (std::memory_order_relaxed);
    status.draws = status.state == State::Done ? g_recordCount.load (std::memory_order_relaxed)
                                               : (reserved < kMaxDraws ? reserved : uint32_t (kMaxDraws));
    status.dropped = g_dropped.load (std::memory_order_relaxed);
    status.readbacksPending = g_readbacksPending.load (std::memory_order_relaxed);
    status.readbackFailures = g_readbackFailures.load (std::memory_order_relaxed);
    status.createFailures = g_createFailures.load (std::memory_order_relaxed);
    status.captures = g_captures.load (std::memory_order_relaxed);
    return status;
}

size_t CopyRecords (DrawRecord* out, size_t capacity)
{
    if (out == nullptr || g_state.load (std::memory_order_acquire) != uint32_t (State::Done))
        return 0;
    const size_t count = g_recordCount.load (std::memory_order_relaxed);
    const size_t n = count < capacity ? count : capacity;
    for (size_t i = 0; i < n; ++i)
        out[i] = g_records[i];
    return n;
}

void OnDraw (ID3D11DeviceContext* context, uint32_t kind, uint32_t count, uint32_t instances)
{
    CloseOpen ();
    const uint32_t state = g_state.load (std::memory_order_acquire); // `Arm`'s fields before it
    if (state == uint32_t (State::Armed))
        NoteModelFrame ();
    if (state != uint32_t (State::Capturing) || context == nullptr)
        return;
    g_inFlight.fetch_add (1, std::memory_order_acq_rel);
    // Re-checked inside the count: the Present thread closes the capture and
    // then reads nothing until this has drained.
    if (g_state.load (std::memory_order_acquire) == uint32_t (State::Capturing)) {
        const uint32_t index = g_reserved.fetch_add (1, std::memory_order_relaxed);
        if (index < kMaxDraws) {
            Record (context, g_records[index], index, kind, count, instances);
            // ⚠️ STILL IN FLIGHT: the second copy is issued after the draw, and
            // the readback must not map the staging buffer before it.
            t_open = int32_t (index);
            return;
        }
        g_dropped.fetch_add (1, std::memory_order_relaxed);
    }
    g_inFlight.fetch_sub (1, std::memory_order_acq_rel);
}

void OnDrawCompleted (ID3D11DeviceContext* context)
{
    if (t_open < 0)
        return;
    if (context != nullptr)
        RecordAfter (context, g_records[t_open], uint32_t (t_open));
    CloseOpen ();
}

void OnPresent (IDXGISwapChain* swapChain)
{
    const uint32_t state = g_state.load (std::memory_order_acquire);
    if (swapChain == nullptr || state == uint32_t (State::Idle) || state == uint32_t (State::Done))
        return;
    g_presentBusy.fetch_add (1, std::memory_order_acq_rel);
    if (g_state.load (std::memory_order_acquire) == uint32_t (State::Armed) &&
        (g_modelFramesWanted.load (std::memory_order_relaxed) == 0 || g_moving.load (std::memory_order_acquire))) {
        // The capture begins at a frame boundary, so frame 0 is a whole frame.
        g_framesSeen.store (0, std::memory_order_relaxed);
        g_state.store (uint32_t (State::Capturing), std::memory_order_release);
    }
    else if (g_state.load (std::memory_order_acquire) == uint32_t (State::Capturing)) {
        const uint32_t seen = g_framesSeen.fetch_add (1, std::memory_order_relaxed) + 1;
        if (seen >= g_framesWanted.load (std::memory_order_relaxed) ||
            g_reserved.load (std::memory_order_relaxed) >= kMaxDraws)
            g_state.store (uint32_t (State::Closing), std::memory_order_release);
    }
    // Closing: read back once every draw that took a record has finished it.
    if (g_state.load (std::memory_order_acquire) == uint32_t (State::Closing) &&
        g_inFlight.load (std::memory_order_acquire) == 0) {
        const uint32_t reserved = g_reserved.load (std::memory_order_relaxed);
        const uint32_t records = reserved < kMaxDraws ? reserved : uint32_t (kMaxDraws);
        if (ReadBack (swapChain, records)) {
            g_recordCount.store (records, std::memory_order_relaxed);
            g_captures.fetch_add (1, std::memory_order_relaxed);
            g_state.store (uint32_t (State::Done), std::memory_order_release);
        }
    }
    g_presentBusy.fetch_sub (1, std::memory_order_acq_rel);
}

void Shutdown ()
{
    Disarm ();
    if (g_staging != nullptr) {
        g_staging->Release ();
        g_staging = nullptr;
    }
}

} // namespace drawrecorder
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
