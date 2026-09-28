// ArchViz/Dxgi/ContextStateTracker -- see the header. Every rule about this file
// is in that header's comments; this is the mechanism.

#include "ArchViz/Dxgi/ContextStateTracker.hpp"

#include <d3d11_1.h>

#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace contextstate {

namespace {

ContextState g_state;
SceneDrawState g_lastSceneDraw;
SceneDrawState g_lastCameraDraw;
DrawCameraCounts g_drawCounts;

// The window size stage 3 measured for both camera constants: 16 constants, 256
// bytes, the D3D11.1 minimum granularity, matrix at offset zero.
constexpr uint32_t kCameraWindowConstants = 16;

// ⚠️ A COUNTER, NOT A FLAG, because the guard has to nest: an injected draw runs
// inside a detour that is already forwarding one of Archicad's calls, and a
// bool would be cleared by the inner scope while the outer one still needed it.
// The context producer and Present consumer are distinct stable threads. The
// guard therefore belongs to the injecting thread; a process-global flag would
// suppress genuine host calls while Present-side injection is in progress.
thread_local int g_injectionDepth = 0;

} // namespace

ScopedInjectionGuard::ScopedInjectionGuard ()
{
    ++g_injectionDepth;
}

ScopedInjectionGuard::~ScopedInjectionGuard ()
{
    if (g_injectionDepth > 0)
        --g_injectionDepth;
}

bool Injecting ()
{
    return g_injectionDepth > 0;
}

bool OnSceneDraw (uint64_t scenePassGeneration, uint64_t sceneTargetEpoch, uint64_t drawSequence,
                  uint64_t modelSceneGeneration, bool inModelPass,
                  const ConstantBufferBinding (&constants)[kConstantBufferSlots])
{
    // ⚠️ THE WHOLE LIVE STATE, LATCHED AT THE DRAW. Not a reference to it, not a
    // promise to read it later: by the time anything wants this, Archicad may
    // have bound something else.
    g_lastSceneDraw.valid = true;
    g_lastSceneDraw.modelSceneGeneration = modelSceneGeneration;
    g_lastSceneDraw.scenePassGeneration = scenePassGeneration;
    g_lastSceneDraw.sceneTargetEpoch = sceneTargetEpoch;
    g_lastSceneDraw.drawSequence = drawSequence;
    g_lastSceneDraw.vertexShader = g_state.vertexShader;
    g_lastSceneDraw.renderTarget = g_state.renderTarget;
    g_lastSceneDraw.depthStencil = g_state.depthStencil;
    g_lastSceneDraw.viewportX = g_state.viewportX;
    g_lastSceneDraw.viewportY = g_state.viewportY;
    g_lastSceneDraw.viewportWidth = g_state.viewportWidth;
    g_lastSceneDraw.viewportHeight = g_state.viewportHeight;
    for (size_t i = 0; i < kConstantBufferSlots; ++i)
        g_lastSceneDraw.vsConstantBuffers[i] = constants[i];

    // ⚠️ STRICT: THIS DRAW, BOTH WINDOWS, RIGHT SIZE. Anything looser lets a
    // gizmo draw that inherited a stale binding count as camera-bearing.
    ++g_drawCounts.total;
    const ConstantBufferBinding& view = constants[1];
    const ConstantBufferBinding& projection = constants[2];
    const bool hasView = view.IsBound () && view.numConstants == kCameraWindowConstants;
    const bool hasProjection = projection.IsBound () && projection.numConstants == kCameraWindowConstants;
    if (hasView)
        ++g_drawCounts.withView;
    if (hasProjection)
        ++g_drawCounts.withProjection;
    if (hasView && hasProjection) {
        ++g_drawCounts.withBoth;
        // ⚠️ ONLY THE MODEL PASS'S CAMERA IS LATCHED. See `withBothInModelPass`.
        if (inModelPass) {
            ++g_drawCounts.withBothInModelPass;
            g_lastCameraDraw = g_lastSceneDraw;
            return true;
        }
    }
    return false;
}

SceneDrawState LastCameraDraw ()
{
    return g_lastCameraDraw;
}

DrawCameraCounts GetDrawCameraCounts ()
{
    return g_drawCounts;
}

SceneDrawState LastSceneDraw ()
{
    return g_lastSceneDraw;
}

void OnVertexShader (ID3D11VertexShader* shader)
{
    g_state.vertexShader = uint64_t (uintptr_t (shader));
}

void OnVSConstantBuffers (uint32_t startSlot, uint32_t count, ID3D11Buffer* const* buffers,
                          const uint32_t* firstConstant, const uint32_t* numConstants)
{
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = startSlot + i;
        if (slot >= kConstantBufferSlots)
            break;
        ConstantBufferBinding& binding = g_state.vsConstantBuffers[slot];

        // ⚠️ A NULL BUFFER IS AN UNBIND, NOT A NO-OP. D3D11 uses it to clear a
        // slot, and leaving the previous pointer there would have a scene pass
        // inherit a binding Archicad had explicitly removed -- which is exactly
        // the class of wrong answer this tracker exists to stop.
        ID3D11Buffer* const buffer = (buffers != nullptr) ? buffers[i] : nullptr;
        binding.buffer = uint64_t (uintptr_t (buffer));
        binding.firstConstant = (firstConstant != nullptr) ? firstConstant[i] : 0;
        binding.numConstants = (numConstants != nullptr) ? numConstants[i] : 0;
    }
}

namespace {

// ⚠️ THE RESOURCE BEHIND A VIEW, WHICH IS THE PART THAT SURVIVES.
// See the header: a view pointer is a COM address Archicad may rebuild at any
// time, and run forty-five lost the camera to exactly that. Width, height,
// format and sample count belong to the texture and do not move.
void DescribeView (ID3D11View* view, ViewDescriptor& out)
{
    out = ViewDescriptor {};
    if (view == nullptr)
        return;
    out.present = true;

    ID3D11Resource* resource = nullptr;
    view->GetResource (&resource);
    if (resource == nullptr)
        return;
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED (resource->QueryInterface (__uuidof (ID3D11Texture2D), (void**) &texture)) && texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc (&desc);
        out.width = desc.Width;
        out.height = desc.Height;
        out.format = uint32_t (desc.Format);
        out.sampleCount = desc.SampleDesc.Count;
        texture->Release ();
    }
    resource->Release ();
}

} // namespace

void OnRenderTargets (ID3D11RenderTargetView* colour, ID3D11DepthStencilView* depth)
{
    // ⚠️ DESCRIBED ON CHANGE, NOT ON EVERY BIND. Archicad rebinds the
    // same two views tens of times a frame; asking the resource each time would
    // put three COM calls on a hot path to learn something that did not change.
    const uint64_t colourId = uint64_t (uintptr_t (colour));
    const uint64_t depthId = uint64_t (uintptr_t (depth));
    // Unconditional, and it costs one store. The DESCRIPTION is what is expensive
    // to re-derive and what the guard below protects; the pointer is not.
    g_state.depthStencilView = depth;
    if (colourId != g_state.renderTarget || !g_state.renderTargetDesc.present) {
        g_state.renderTarget = colourId;
        DescribeView (colour, g_state.renderTargetDesc);
    }
    if (depthId != g_state.depthStencil || !g_state.depthStencilDesc.present) {
        g_state.depthStencil = depthId;
        DescribeView (depth, g_state.depthStencilDesc);
    }
}

void OnViewport (const D3D11_VIEWPORT& viewport)
{
    g_state.viewportX = viewport.TopLeftX;
    g_state.viewportY = viewport.TopLeftY;
    g_state.viewportWidth = viewport.Width;
    g_state.viewportHeight = viewport.Height;
}

ContextState Snapshot ()
{
    return g_state;
}

void Reset ()
{
    // See the header. `g_injectionDepth` is thread-local and belongs to the
    // thread that raised it; a reset from any other thread must not touch it.
    g_state = ContextState {};
    g_lastSceneDraw = SceneDrawState {};
    g_lastCameraDraw = SceneDrawState {};
    g_drawCounts = DrawCameraCounts {};
}

BoundConstants::BoundConstants (ID3D11DeviceContext* context)
{
    if (context == nullptr)
        return;
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED (context->QueryInterface (__uuidof (ID3D11DeviceContext1), reinterpret_cast<void**> (&context1))) ||
        context1 == nullptr)
        return;
    // Each returned buffer carries a reference of ours; the destructor drops it.
    context1->VSGetConstantBuffers1 (0, UINT (kSlots), m_buffers, m_first, m_count);
    context1->Release ();
    m_valid = true;
}

BoundConstants::~BoundConstants ()
{
    for (ID3D11Buffer*& buffer : m_buffers) {
        if (buffer != nullptr)
            buffer->Release ();
        buffer = nullptr;
    }
}

namespace {

ConstantBufferBinding Bound (ID3D11Buffer* buffer, unsigned int first, unsigned int count)
{
    ConstantBufferBinding binding;
    binding.buffer = uint64_t (uintptr_t (buffer));
    binding.firstConstant = buffer != nullptr ? first : 0;
    binding.numConstants = buffer != nullptr ? count : 0;
    return binding;
}

// A tracked slot disagrees when it names another buffer, or -- for a windowed
// bind, the only kind the tracker records a window for -- another window.
bool Differs (const ConstantBufferBinding& tracked, const ConstantBufferBinding& bound)
{
    if (tracked.buffer != bound.buffer)
        return true;
    return tracked.numConstants != 0 &&
           (tracked.firstConstant != bound.firstConstant || tracked.numConstants != bound.numConstants);
}

} // namespace

void BoundConstants::ApplyTo (ContextState& state, BindingAudit* audit) const
{
    if (!m_valid) {
        if (audit != nullptr)
            ++audit->unavailable;
        return;
    }
    if (audit != nullptr) {
        ++audit->compared;
        if (Differs (state.vsConstantBuffers[1], Bound (m_buffers[1], m_first[1], m_count[1])))
            ++audit->viewDiffered;
        if (Differs (state.vsConstantBuffers[2], Bound (m_buffers[2], m_first[2], m_count[2])))
            ++audit->projectionDiffered;
    }
    for (size_t i = 0; i < kSlots; ++i)
        state.vsConstantBuffers[i] = Bound (m_buffers[i], m_first[i], m_count[i]);
}

void BoundConstants::ApplyTo (SceneDrawState& draw) const
{
    if (!m_valid)
        return;
    for (size_t i = 0; i < kSlots; ++i)
        draw.vsConstantBuffers[i] = Bound (m_buffers[i], m_first[i], m_count[i]);
}

} // namespace contextstate
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
