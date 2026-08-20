#include "ArchViz/DiligentFxLink.hpp"

// ⚠️ d3d11.h BEFORE any Diligent D3D11 interop header (Probe 1a) -- the Diligent
// headers name ID3D11Device/ID3D11Texture2D without including it themselves.
#include <windows.h>
#include <d3d11.h>

// The whole point of the file: real DiligentFX headers, from three different
// modules, so a broken include directory cannot hide behind one lucky path.
//
// ⚠️ THE INCLUDE ROOT IS DiligentFX ITSELF (`target_include_directories(DiligentFX
// PUBLIC .)`), which is what makes the nested `Shaders/Common/public/*.fxh`
// includes inside these headers resolve. If that propagation ever breaks, it
// breaks HERE, at build time, instead of inside whatever feature reached for
// DiligentFX first.
#include "Components/interface/ShadowMapManager.hpp"
#include "PostProcess/Common/interface/PostFXRenderTechnique.hpp"

#include <string>

namespace geomsrv {
namespace archviz {

bool DiligentFxLinked (std::string& report)
{
    // ⚠️ CONSTRUCTED, NOT JUST DECLARED, AND THAT IS THE LINK HALF OF THE PROOF.
    // A type that is only named is resolved entirely by the compiler and would
    // still "pass" with DiligentFX.lib absent from the link line. Calling a
    // non-inline constructor gives the linker a symbol it must find in the
    // library, so this function failing to link IS the failure report.
    Diligent::ShadowMapManager shadowMapManager;

    // Read something back so the constructor cannot be optimised away as dead.
    // A default-constructed manager has no cascades and no SRV; both facts are
    // stable and neither needs a device.
    const bool hasSrv = (shadowMapManager.GetSRV () != nullptr);

    report = "DiligentFX linked: ShadowMapManager constructed (srv=";
    report += hasSrv ? "present" : "null";
    report += ", as expected before Initialize). Headers reached: "
              "Components/ShadowMapManager, PostProcess/PostFXRenderTechnique.";
    return true;
}

}   // namespace archviz
}   // namespace geomsrv
