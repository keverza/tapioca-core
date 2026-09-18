#ifndef EVP_ARCHVIZ_DXGI_PIPELINESTATEGUARD_HPP
#define EVP_ARCHVIZ_DXGI_PIPELINESTATEGUARD_HPP

// Everything an injected draw disturbs on Archicad's context, captured on the
// way in and put back on the way out (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ THIS EXISTS BECAUSE THERE WERE THREE COPIES OF IT AND THEY HAD ALREADY
// DRIFTED. `InjectionRenderer`, `InjectionProbes` and `DepthCheckpoints` each
// carried the same sixty lines: eleven `*Get*` calls, eleven `*Set*` calls and
// eleven `Release`s, in the same order, for the same reasons. When the ghost
// mesh became the first thing here to use `DrawIndexed`, the index binding had
// to join that list -- and it joined exactly ONE of the three copies. The other
// two would have handed Archicad back our index buffer.
//
// ⚠️ EVERY FIELD IS RESTORED AND EVERY REFERENCE IS RELEASED, AND NEITHER IS
// OPTIONAL. `*Get*` returns an AddRef'd pointer; one leaked per frame keeps
// Archicad's own shaders, buffers and views alive past a resize. Restoring
// without releasing leaks; releasing without restoring corrupts the next draw
// Archicad makes. A destructor does both, once, on every path out -- including
// the early returns that a hand-written block keeps forgetting.
//
// ⚠️ IT SAVES WHAT SOMETHING HERE WRITES, AND THAT SET GREW.
// This file used to say scissor rectangles, the geometry and compute stages,
// SRVs and samplers were "deliberately absent... and the day something does
// write it, it belongs here explicitly". That day is the Diligent backend, and
// the three things it writes were each measured in the vendored engine rather
// than assumed:
//
//   ⚠️ `IDeviceContext::InvalidateState()` IS NOT A CACHE
//   DROP. DeviceContextD3D11Impl.cpp:2155 nulls ALL SIX shader stages, the
//   render target, the vertex buffers, the input layout and the index buffer ON
//   THE NATIVE CONTEXT. It has to be called -- Diligent cannot know Archicad
//   rebound everything underneath it -- and it is safe ONLY inside this guard.
//   Called before construction it would hand Archicad back a dead context.
//
//   Diligent's ImGui renderer binds one pixel-shader texture and its sampler
//   (ImGuiDiligentRenderer.cpp:114) and a vertex-shader constant buffer, and it
//   sets scissor rectangles per draw command.
//
//   Its pipeline state object owns blend, depth-stencil and rasterizer state,
//   which this guard already covered.
//
// ⚠️ THE SLOT COUNTS ARE A CONTRACT, NOT A GUESS. Saving
// all 128 SRV slots per stage every Present is real work in a hot hook, so the
// guard covers a stated prefix: our own injected shaders declare none, and the
// heaviest guest -- the ImGui renderer -- declares t0/s0/b0. Eight leaves room
// for the annotation atlas and a heatmap lookup without revisiting the hot path.
// A shader that binds beyond the prefix is a change TO THIS FILE, deliberately,
// not a silent corruption of Archicad's next draw.
//
// RENDER THREAD ONLY, inside a detour, under `ScopedInjectionGuard`.

#include <d3d11_1.h>

namespace geomsrv {
namespace archviz {
namespace dxgi {

class ScopedPipelineState {
  public:
    ScopedPipelineState (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1)
        : mContext (context), mContext1 (context1)
    {
        if (mContext == nullptr)
            return;
        mContext->VSGetShader (&mVs, nullptr, nullptr);
        mContext->PSGetShader (&mPs, nullptr, nullptr);
        // The other four stages: Archicad may use them, and `InvalidateState`
        // nulls them whether or not Diligent has anything to put there.
        mContext->GSGetShader (&mGs, nullptr, nullptr);
        mContext->HSGetShader (&mHs, nullptr, nullptr);
        mContext->DSGetShader (&mDs, nullptr, nullptr);
        mContext->CSGetShader (&mCs, nullptr, nullptr);
        mContext->VSGetShaderResources (0, kTrackedSlots, mVsSrv);
        mContext->PSGetShaderResources (0, kTrackedSlots, mPsSrv);
        mContext->VSGetSamplers (0, kTrackedSlots, mVsSampler);
        mContext->PSGetSamplers (0, kTrackedSlots, mPsSampler);
        mContext->RSGetScissorRects (&mScissorCount, mScissors);
        mContext->IAGetInputLayout (&mLayout);
        mContext->IAGetVertexBuffers (0, 1, &mVertexBuffer, &mStride, &mOffset);
        mContext->IAGetIndexBuffer (&mIndexBuffer, &mIndexFormat, &mIndexOffset);
        mContext->IAGetPrimitiveTopology (&mTopology);
        mContext->OMGetDepthStencilState (&mDepthState, &mStencilRef);
        mContext->RSGetState (&mRaster);
        mContext->OMGetBlendState (&mBlend, mBlendFactor, &mSampleMask);
        mContext->RSGetViewports (&mViewportCount, mViewports);
        mContext->OMGetRenderTargets (1, &mRtv, &mDsv);
        // ⚠️ SLOTS 0..3, NOT 1..2, AND THE WINDOWED FORM
        // FOR BOTH STAGES. b1 and b2 are Archicad's view and projection (frozen
        // finding 1) and restoring them with their 256-byte windows intact is
        // what keeps the host's own camera correct after we borrow it; b0 is
        // what the ImGui renderer binds. Reading with the D3D11.1 form and
        // writing it back the same way is the only version that preserves an
        // offset -- the legacy setter silently rebinds at constant zero.
        if (mContext1 != nullptr) {
            mContext1->VSGetConstantBuffers1 (0, kTrackedCbSlots, mVsCb, mVsCbFirst, mVsCbNum);
            mContext1->PSGetConstantBuffers1 (0, kTrackedCbSlots, mPsCb, mPsCbFirst, mPsCbNum);
        }
    }

    ~ScopedPipelineState ()
    {
        if (mContext == nullptr)
            return;
        mContext->OMSetRenderTargets (1, &mRtv, mDsv);
        if (mContext1 != nullptr) {
            mContext1->VSSetConstantBuffers1 (0, kTrackedCbSlots, mVsCb, mVsCbFirst, mVsCbNum);
            mContext1->PSSetConstantBuffers1 (0, kTrackedCbSlots, mPsCb, mPsCbFirst, mPsCbNum);
        }
        mContext->VSSetShader (mVs, nullptr, 0);
        mContext->PSSetShader (mPs, nullptr, 0);
        mContext->GSSetShader (mGs, nullptr, 0);
        mContext->HSSetShader (mHs, nullptr, 0);
        mContext->DSSetShader (mDs, nullptr, 0);
        mContext->CSSetShader (mCs, nullptr, 0);
        mContext->VSSetShaderResources (0, kTrackedSlots, mVsSrv);
        mContext->PSSetShaderResources (0, kTrackedSlots, mPsSrv);
        mContext->VSSetSamplers (0, kTrackedSlots, mVsSampler);
        mContext->PSSetSamplers (0, kTrackedSlots, mPsSampler);
        // ⚠️ RESTORED EVEN WHEN THE COUNT IS ZERO. Archicad
        // having no scissor rectangle is a STATE, and leaving Diligent's behind
        // would clip the host's next draw to an ImGui window.
        mContext->RSSetScissorRects (mScissorCount, mScissorCount > 0 ? mScissors : nullptr);
        mContext->IASetInputLayout (mLayout);
        mContext->IASetVertexBuffers (0, 1, &mVertexBuffer, &mStride, &mOffset);
        mContext->IASetIndexBuffer (mIndexBuffer, mIndexFormat, mIndexOffset);
        mContext->IASetPrimitiveTopology (mTopology);
        mContext->OMSetDepthStencilState (mDepthState, mStencilRef);
        mContext->RSSetState (mRaster);
        mContext->OMSetBlendState (mBlend, mBlendFactor, mSampleMask);
        if (mViewportCount > 0)
            mContext->RSSetViewports (mViewportCount, mViewports);

        for (UINT i = 0; i < kTrackedCbSlots; ++i) {
            Release (mVsCb[i]);
            Release (mPsCb[i]);
        }
        for (UINT i = 0; i < kTrackedSlots; ++i) {
            Release (mVsSrv[i]);
            Release (mPsSrv[i]);
            Release (mVsSampler[i]);
            Release (mPsSampler[i]);
        }
        Release (mVs);
        Release (mPs);
        Release (mGs);
        Release (mHs);
        Release (mDs);
        Release (mCs);
        Release (mLayout);
        Release (mVertexBuffer);
        Release (mIndexBuffer);
        Release (mDepthState);
        Release (mRaster);
        Release (mBlend);
        Release (mRtv);
        Release (mDsv);
    }

    ScopedPipelineState (const ScopedPipelineState&) = delete;
    ScopedPipelineState& operator= (const ScopedPipelineState&) = delete;

    // What Archicad had bound, for a caller that needs to rebind it deliberately
    // rather than merely put it back.
    ID3D11RenderTargetView* SavedRenderTarget () const
    {
        return mRtv;
    }
    ID3D11DepthStencilView* SavedDepthStencil () const
    {
        return mDsv;
    }

  private:
    // See the slot-count paragraph in this file's header comment: a prefix, and
    // a shader that needs more changes this number here on purpose.
    static constexpr UINT kTrackedSlots = 8;
    static constexpr UINT kTrackedCbSlots = 4;

    template <typename T> static void Release (T*& object)
    {
        if (object != nullptr) {
            object->Release ();
            object = nullptr;
        }
    }

    ID3D11DeviceContext* mContext = nullptr;
    ID3D11DeviceContext1* mContext1 = nullptr;

    ID3D11VertexShader* mVs = nullptr;
    ID3D11PixelShader* mPs = nullptr;
    ID3D11GeometryShader* mGs = nullptr;
    ID3D11HullShader* mHs = nullptr;
    ID3D11DomainShader* mDs = nullptr;
    ID3D11ComputeShader* mCs = nullptr;
    ID3D11InputLayout* mLayout = nullptr;
    ID3D11Buffer* mVertexBuffer = nullptr;
    UINT mStride = 0;
    UINT mOffset = 0;
    ID3D11Buffer* mIndexBuffer = nullptr;
    DXGI_FORMAT mIndexFormat = DXGI_FORMAT_UNKNOWN;
    UINT mIndexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY mTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11DepthStencilState* mDepthState = nullptr;
    UINT mStencilRef = 0;
    ID3D11RasterizerState* mRaster = nullptr;
    ID3D11BlendState* mBlend = nullptr;
    FLOAT mBlendFactor[4] = {};
    UINT mSampleMask = 0;
    ID3D11Buffer* mVsCb[kTrackedCbSlots] = {};
    UINT mVsCbFirst[kTrackedCbSlots] = {};
    UINT mVsCbNum[kTrackedCbSlots] = {};
    ID3D11Buffer* mPsCb[kTrackedCbSlots] = {};
    UINT mPsCbFirst[kTrackedCbSlots] = {};
    UINT mPsCbNum[kTrackedCbSlots] = {};
    ID3D11ShaderResourceView* mVsSrv[kTrackedSlots] = {};
    ID3D11ShaderResourceView* mPsSrv[kTrackedSlots] = {};
    ID3D11SamplerState* mVsSampler[kTrackedSlots] = {};
    ID3D11SamplerState* mPsSampler[kTrackedSlots] = {};
    D3D11_RECT mScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT mScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RenderTargetView* mRtv = nullptr;
    ID3D11DepthStencilView* mDsv = nullptr;
    D3D11_VIEWPORT mViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT mViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
};

} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
