#ifndef EVP_ARCHVIZ_DXGI_CONTEXTHOOKDETOURTYPES_HPP
#define EVP_ARCHVIZ_DXGI_CONTEXTHOOKDETOURTYPES_HPP

// INTERNAL to the context hook: the function-pointer types of the patched
// ID3D11DeviceContext slots, moved verbatim out of ContextHookDetours.cpp so
// that file stays under the size cap. Only ContextHookDetours.cpp includes it.

#include <windows.h>

#include <d3d11.h>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hookshared {

using RSSetViewportsFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT,
                                                          ID3D11ShaderResourceView* const*);
using SetConstantBuffersFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                            D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                       const void*, UINT, UINT);
using ClearRTVFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDSVFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using DrawIndexedFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using CopyResourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ExecuteCommandListFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using DrawInstancedFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*);
using DrawIndirectFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirectFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using CopySubresourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                     ID3D11Resource*, UINT, const D3D11_BOX*);
using ResolveSubresourceFn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*,
                                                        UINT, DXGI_FORMAT);
using SetConstantBuffers1Fn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*,
                                                         const UINT*, const UINT*);
using UpdateSubresource1Fn = void (STDMETHODCALLTYPE*) (ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                        const void*, UINT, UINT, UINT);

} // namespace hookshared
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
