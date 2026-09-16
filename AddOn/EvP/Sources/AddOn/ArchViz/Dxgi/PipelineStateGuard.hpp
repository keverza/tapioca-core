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
// ⚠️ IT DOES NOT SAVE WHAT NOBODY HERE CHANGES. Stencil reference, scissor
// rectangles, geometry and compute stages, SRVs and samplers are deliberately
// absent: the injected draws bind their own rasterizer state with scissor
// disabled and touch no other stage. Saving state nothing writes would be
// cargo cult, and the day something does write it, it belongs here explicitly.
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
        mContext->IAGetInputLayout (&mLayout);
        mContext->IAGetVertexBuffers (0, 1, &mVertexBuffer, &mStride, &mOffset);
        mContext->IAGetIndexBuffer (&mIndexBuffer, &mIndexFormat, &mIndexOffset);
        mContext->IAGetPrimitiveTopology (&mTopology);
        mContext->OMGetDepthStencilState (&mDepthState, &mStencilRef);
        mContext->RSGetState (&mRaster);
        mContext->OMGetBlendState (&mBlend, mBlendFactor, &mSampleMask);
        mContext->RSGetViewports (&mViewportCount, mViewports);
        mContext->OMGetRenderTargets (1, &mRtv, &mDsv);
        if (mContext1 != nullptr)
            mContext1->VSGetConstantBuffers1 (1, 2, mCb, mCbFirst, mCbNum);
    }

    ~ScopedPipelineState ()
    {
        if (mContext == nullptr)
            return;
        mContext->OMSetRenderTargets (1, &mRtv, mDsv);
        if (mContext1 != nullptr)
            mContext1->VSSetConstantBuffers1 (1, 2, mCb, mCbFirst, mCbNum);
        mContext->VSSetShader (mVs, nullptr, 0);
        mContext->PSSetShader (mPs, nullptr, 0);
        mContext->IASetInputLayout (mLayout);
        mContext->IASetVertexBuffers (0, 1, &mVertexBuffer, &mStride, &mOffset);
        mContext->IASetIndexBuffer (mIndexBuffer, mIndexFormat, mIndexOffset);
        mContext->IASetPrimitiveTopology (mTopology);
        mContext->OMSetDepthStencilState (mDepthState, mStencilRef);
        mContext->RSSetState (mRaster);
        mContext->OMSetBlendState (mBlend, mBlendFactor, mSampleMask);
        if (mViewportCount > 0)
            mContext->RSSetViewports (mViewportCount, mViewports);

        Release (mCb[0]);
        Release (mCb[1]);
        Release (mVs);
        Release (mPs);
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
    ID3D11Buffer* mCb[2] = {};
    UINT mCbFirst[2] = {};
    UINT mCbNum[2] = {};
    ID3D11RenderTargetView* mRtv = nullptr;
    ID3D11DepthStencilView* mDsv = nullptr;
    D3D11_VIEWPORT mViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT mViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
};

} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
