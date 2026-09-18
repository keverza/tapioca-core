// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// The HLSL bodies of the injection's own primitives. Split out of
// InjectionRenderer.cpp, which is at its size cap and whose subject is the
// Present sequence, not shader text.

#ifndef GEOMSRV_ARCHVIZ_DXGI_PROOFSHADERSOURCE_HPP
#define GEOMSRV_ARCHVIZ_DXGI_PROOFSHADERSOURCE_HPP

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

// ⚠️ THE DECLARATIONS ARE IN `CameraShaderSource.hpp`, NOT HERE. The
// ghost mesh reads the same `b1` and `b2` through the same two lines, and a
// second copy of them in a second file is exactly how run thirty-three ended up
// measuring one reading while drawing another. This file owns the BODIES of the
// proof shaders and nothing about the camera contract.
const char* const kProofShaderBody = "float4 VSMain (float3 position : POSITION) : SV_POSITION\n"
                                     "{\n"
                                     "    float4 p = float4 (position, 1.0);\n"
                                     "    p = mul (p, View);\n"
                                     "    p = mul (p, Projection);\n"
                                     "    return p;\n"
                                     "}\n"
                                     "float4 PSMain () : SV_TARGET\n"
                                     "{\n"
                                     "    return float4 (1.0, 0.15, 0.85, 1.0);\n"
                                     "}\n"
                                     // ⚠️ PROBE A: NO CAMERA, NO CONSTANT BUFFERS, NO WORLD TRANSFORM. Its
                                     // vertices are already in clip space, so it lands in the same corner of the screen
                                     // on every Present no matter what any matrix says. That is the entire point: it
                                     // separates "the injection or the back buffer is wrong" from "the camera is
                                     // wrong", and those are different investigations. ⚠️ DO NOT DEBUG THE
                                     // CAMERA UNTIL PROBE A IS ROCK SOLID.
                                     "float4 VSScreen (float3 position : POSITION) : SV_POSITION\n"
                                     "{\n"
                                     "    return float4 (position, 1.0);\n"
                                     "}\n"
                                     "float4 PSScreen () : SV_TARGET\n"
                                     "{\n"
                                     "    return float4 (0.1, 1.0, 0.3, 1.0);\n"
                                     "}\n";

} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
