// ArchViz/DiligentDepthRange — one compute dispatch per frame that measures how
// deep the picture actually is. The why is in the header.

#include "ArchViz/DiligentDepthRange.hpp"

#include "ArchViz/DiligentShaders.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).

#include <Buffer.h>
#include <BufferView.h>
#include <DeviceContext.h>
#include <Fence.h>
#include <PipelineState.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>
#include <Shader.h>
#include <ShaderResourceBinding.h>

#include <cstring>

namespace geomsrv {
namespace archviz {

namespace {

// Must match [numthreads] in kArchVizDepthRangeCS. A mismatch does not fail to
// compile -- it silently skips or double-covers pixels.
constexpr uint32_t kGroupSize = 8;

// Element 0 is the nearest depth, element 1 the farthest, both as raw float
// bits. ⚠️ SEEDED INVERTED, and the shader reads that as "nothing measured":
// InterlockedMin/Max need an identity to start from, and min > max is the only
// state that cannot also be a real answer.
constexpr uint32_t kEmptyRange[2] = { 0xFFFFFFFFu, 0u };

} // namespace

struct Reduced {
    Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
    Diligent::RefCntAutoPtr<Diligent::IBufferView> uav;
    Diligent::RefCntAutoPtr<Diligent::IBufferView> srv;

    void Release ()
    {
        srv.Release ();
        uav.Release ();
        buffer.Release ();
    }
};

struct DiligentDepthRange::Impl {
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb;
    // The raw min/max of THIS frame — re-seeded and re-reduced every dispatch.
    Reduced raw;
    // The eased range the pixel shader reads. ⚠️ NEVER RE-SEEDED after Init:
    // it is the only memory the ease has, and clearing it each frame would turn
    // the smoothing back into the raw per-frame fit it exists to replace.
    Reduced smooth;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> smoothPso;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> smoothSrb;
    // #14 readback (RequestReadback / ReadBack): three staging slots, oldest first, each with the
    // fence value its copy signalled (D3D12; null fence on D3D11, where the map itself says "not yet").
    static constexpr int kSlots = 3;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> staging[kSlots];
    uint64_t slotFence[kSlots] = {};
    bool slotPending[kSlots] = {};
    int nextSlot = 0;
    Diligent::RefCntAutoPtr<Diligent::IFence> fence;
    uint64_t fenceValue = 0;
    bool executed = false; // `raw` holds a range from this frame's Execute
    bool have = false;
    float nearNdc = 1.0f, farNdc = 0.0f;
};

DiligentDepthRange::DiligentDepthRange () : impl_ (std::make_unique<Impl> ())
{
}

DiligentDepthRange::~DiligentDepthRange ()
{
    Shutdown ();
}

bool DiligentDepthRange::Init (Diligent::IRenderDevice* device, std::string& error)
{
    if (device == nullptr) {
        error = "DiligentDepthRange::Init got a null device";
        return false;
    }
    if (impl_->pso != nullptr)
        return true;

    auto compile = [&] (const char* name, const char* body, Diligent::RefCntAutoPtr<Diligent::IShader>& out) -> bool {
        const std::string source = ArchVizShaderSource (body);
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = name;
        sci.Desc.ShaderType = Diligent::SHADER_TYPE_COMPUTE;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = source.c_str ();
        sci.SourceLength = source.size ();
        device->CreateShader (sci, &out, nullptr);
        if (out == nullptr) {
            error = std::string ("Diligent CreateShader(") + name +
                    ") failed -- the HLSL compiler's own message is in the debug output";
            return false;
        }
        return true;
    };

    Diligent::RefCntAutoPtr<Diligent::IShader> reduceShader;
    Diligent::RefCntAutoPtr<Diligent::IShader> smoothShader;
    if (!compile ("ArchViz depth range CS", kArchVizDepthRangeCS, reduceShader) ||
        !compile ("ArchViz depth range smooth CS", kArchVizDepthRangeSmoothCS, smoothShader))
        return false;

    // ⚠️ A FORMATTED BUFFER, NOT A STRUCTURED ONE. The shader declares
    // RWBuffer<uint>/Buffer<uint>, which is the formatted view; a structured
    // buffer would need StructuredBuffer<uint> on both sides, and Diligent
    // reports the mismatch only at SRB-commit time, one frame from here.
    auto createReduced = [&] (const char* name, Reduced& out) -> bool {
        Diligent::BufferDesc bd;
        bd.Name = name;
        bd.Size = sizeof (kEmptyRange);
        bd.Usage = Diligent::USAGE_DEFAULT;
        bd.BindFlags = Diligent::BIND_UNORDERED_ACCESS | Diligent::BIND_SHADER_RESOURCE;
        bd.Mode = Diligent::BUFFER_MODE_FORMATTED;
        bd.ElementByteStride = sizeof (uint32_t);
        Diligent::BufferData seed { kEmptyRange, sizeof (kEmptyRange) };
        device->CreateBuffer (bd, &seed, &out.buffer);
        if (out.buffer == nullptr) {
            error = std::string ("Diligent CreateBuffer(") + name + ") failed";
            return false;
        }

        Diligent::BufferViewDesc uavDesc;
        uavDesc.ViewType = Diligent::BUFFER_VIEW_UNORDERED_ACCESS;
        uavDesc.Format.ValueType = Diligent::VT_UINT32;
        uavDesc.Format.NumComponents = 1;
        uavDesc.Format.IsNormalized = Diligent::False;
        out.buffer->CreateView (uavDesc, &out.uav);

        Diligent::BufferViewDesc srvDesc = uavDesc;
        srvDesc.ViewType = Diligent::BUFFER_VIEW_SHADER_RESOURCE;
        out.buffer->CreateView (srvDesc, &out.srv);
        if (out.uav == nullptr || out.srv == nullptr) {
            error = std::string ("Diligent CreateView(") + name + ") failed";
            return false;
        }
        return true;
    };

    if (!createReduced ("ArchViz depth range raw", impl_->raw) ||
        !createReduced ("ArchViz depth range smooth", impl_->smooth))
        return false;
    // #14 readback: if any of it fails the cascades simply keep the scene-box fit.
    for (auto& staging : impl_->staging) {
        Diligent::BufferDesc sd;
        sd.Name = "ArchViz depth range readback";
        sd.Size = sizeof (kEmptyRange);
        sd.Usage = Diligent::USAGE_STAGING;
        sd.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
        device->CreateBuffer (sd, nullptr, &staging);
    }
    if (device->GetDeviceInfo ().Type == Diligent::RENDER_DEVICE_TYPE_D3D12) {
        Diligent::FenceDesc fd;
        fd.Name = "ArchViz depth range readback fence";
        fd.Type = Diligent::FENCE_TYPE_CPU_WAIT_ONLY;
        device->CreateFence (fd, &impl_->fence);
    }

    auto createPso = [&] (const char* name, Diligent::IShader* shader,
                          const Diligent::ShaderResourceVariableDesc* variables, Diligent::Uint32 variableCount,
                          Diligent::RefCntAutoPtr<Diligent::IPipelineState>& pso,
                          Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb) -> bool {
        Diligent::ComputePipelineStateCreateInfo pci;
        pci.PSODesc.Name = name;
        pci.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_COMPUTE;
        pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
        pci.PSODesc.ResourceLayout.Variables = variables;
        pci.PSODesc.ResourceLayout.NumVariables = variableCount;
        pci.pCS = shader;
        device->CreateComputePipelineState (pci, &pso);
        if (pso == nullptr) {
            error = std::string ("Diligent CreateComputePipelineState(") + name + ") failed";
            return false;
        }
        pso->CreateShaderResourceBinding (&srb, true);
        if (srb == nullptr) {
            error = std::string ("Diligent CreateShaderResourceBinding(") + name + ") failed";
            return false;
        }
        return true;
    };

    const Diligent::ShaderResourceVariableDesc reduceVariables[] = {
        { Diligent::SHADER_TYPE_COMPUTE, "g_depth", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_COMPUTE, "g_depthRangeOut", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    const Diligent::ShaderResourceVariableDesc smoothVariables[] = {
        { Diligent::SHADER_TYPE_COMPUTE, "g_depthRangeRaw", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        { Diligent::SHADER_TYPE_COMPUTE, "g_depthRangeSmooth", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    if (!createPso ("ArchViz depth range PSO", reduceShader, reduceVariables, _countof (reduceVariables), impl_->pso,
                    impl_->srb) ||
        !createPso ("ArchViz depth range smooth PSO", smoothShader, smoothVariables, _countof (smoothVariables),
                    impl_->smoothPso, impl_->smoothSrb))
        return false;
    return true;
}

void DiligentDepthRange::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->smoothSrb.Release ();
    impl_->smoothPso.Release ();
    impl_->srb.Release ();
    impl_->pso.Release ();
    impl_->smooth.Release ();
    impl_->raw.Release ();
    for (int i = 0; i < Impl::kSlots; ++i) {
        impl_->staging[i].Release ();
        impl_->slotPending[i] = false;
    }
    impl_->fence.Release ();
    impl_->fenceValue = 0;
    impl_->executed = impl_->have = false;
}

Diligent::IBufferView* DiligentDepthRange::BufferView () const
{
    // ⚠️ THE SMOOTHED ONE. Handing out the raw buffer is the whole bug this
    // class was extended to fix -- it is correct every frame and different
    // every frame.
    return impl_ == nullptr ? nullptr : impl_->smooth.srv.RawPtr ();
}

void DiligentDepthRange::Execute (Diligent::IDeviceContext* context, Diligent::ITextureView* depth, uint32_t width,
                                  uint32_t height)
{
    if (context == nullptr || depth == nullptr || impl_->pso == nullptr || width == 0 || height == 0)
        return;

    // ⚠️ RE-SEEDED EVERY FRAME, AND THAT IS THE WHOLE POINT OF THE PASS. The
    // atomics only ever narrow the range, so a buffer left alone would keep the
    // extremes of every frame since the viewport opened -- the ramp would stop
    // responding to the camera after the first orbit, which looks like it
    // simply stopped working.
    context->UpdateBuffer (impl_->raw.buffer, 0, sizeof (kEmptyRange), kEmptyRange,
                           Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    impl_->srb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, "g_depth")->Set (depth);
    impl_->srb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, "g_depthRangeOut")->Set (impl_->raw.uav);

    context->SetPipelineState (impl_->pso);
    context->CommitShaderResources (impl_->srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Diligent::DispatchComputeAttribs dispatch;
    dispatch.ThreadGroupCountX = (width + kGroupSize - 1) / kGroupSize;
    dispatch.ThreadGroupCountY = (height + kGroupSize - 1) / kGroupSize;
    dispatch.ThreadGroupCountZ = 1;
    context->DispatchCompute (dispatch);

    // Ease this frame's answer into the one the shader reads. One thread: it is
    // two lerps, and the dispatch overhead is the entire cost.
    impl_->smoothSrb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, "g_depthRangeRaw")->Set (impl_->raw.uav);
    impl_->smoothSrb->GetVariableByName (Diligent::SHADER_TYPE_COMPUTE, "g_depthRangeSmooth")->Set (impl_->smooth.uav);
    context->SetPipelineState (impl_->smoothPso);
    context->CommitShaderResources (impl_->smoothSrb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::DispatchComputeAttribs ease;
    ease.ThreadGroupCountX = 1;
    ease.ThreadGroupCountY = 1;
    ease.ThreadGroupCountZ = 1;
    context->DispatchCompute (ease);
    impl_->executed = true;
}

void DiligentDepthRange::RequestReadback (Diligent::IDeviceContext* context)
{
    const int slot = impl_->nextSlot;
    if (context == nullptr || !impl_->executed || impl_->staging[slot] == nullptr || impl_->slotPending[slot])
        return; // every slot still in flight: skip this frame rather than wait
    impl_->executed = false;
    context->CopyBuffer (impl_->raw.buffer, 0, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                         impl_->staging[slot], 0, sizeof (kEmptyRange),
                         Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (impl_->fence != nullptr)
        context->EnqueueSignal (impl_->fence, ++impl_->fenceValue);
    impl_->slotFence[slot] = impl_->fenceValue;
    impl_->slotPending[slot] = true;
    impl_->nextSlot = (slot + 1) % Impl::kSlots;
}

bool DiligentDepthRange::ReadBack (Diligent::IDeviceContext* context, float& nearNdc, float& farNdc)
{
    if (context != nullptr) {
        // Oldest first: the slot after the one written next.
        for (int k = 0; k < Impl::kSlots; ++k) {
            const int slot = (impl_->nextSlot + k) % Impl::kSlots;
            if (!impl_->slotPending[slot])
                continue;
            if (impl_->fence != nullptr && impl_->fence->GetCompletedValue () < impl_->slotFence[slot])
                break; // the GPU has not reached it, nor anything after it
            void* data = nullptr;
            context->MapBuffer (impl_->staging[slot], Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT, data);
            if (data == nullptr)
                break; // D3D11: not finished yet
            uint32_t bits[2];
            std::memcpy (bits, data, sizeof (bits));
            context->UnmapBuffer (impl_->staging[slot], Diligent::MAP_READ);
            impl_->slotPending[slot] = false;
            impl_->have = bits[0] <= bits[1];
            if (impl_->have) {
                std::memcpy (&impl_->nearNdc, &bits[0], sizeof (float));
                std::memcpy (&impl_->farNdc, &bits[1], sizeof (float));
            }
        }
    }
    nearNdc = impl_->nearNdc;
    farNdc = impl_->farNdc;
    return impl_->have;
}

} // namespace archviz
} // namespace geomsrv
