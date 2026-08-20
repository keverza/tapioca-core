// Replaces bgfx's own shaderc_glsl.cpp in this HLSL-only build.
//
// ⚠️ THIS EXISTS BECAUSE OF A COPY-PASTE BUG IN THE VENDORED SOURCE, not because
// we want a different stub. `shaderc_glsl.cpp`'s
// `#else // SHADERC_CONFIG_HAS_GLSL_OPTIMIZER` branch defines
// **compileSPIRVShader**, not compileGLSLShader. So with glsl-optimizer and
// glslang both absent — which is the whole point of an HLSL-only build — you get:
//
//   LNK2005 compileSPIRVShader already defined in shaderc_glsl.obj
//   LNK2019 unresolved external compileGLSLShader
//
// i.e. one symbol twice and the other never. The vendored tree is left alone
// (repo rule: bgfx builds from `reference/` with no source modifications), so
// shaderc_glsl.cpp is dropped from the build and this supplies the symbol it was
// supposed to. `shaderc_spirv.cpp` keeps providing the real SPIRV stub.
//
// If a future bgfx fixes it, this file and the CMake exclusion beside it both go.

#include "shaderc.h"

namespace bgfx {

bool compileGLSLShader (const Options& _options, uint32_t _version, const std::string& _code,
                        bx::WriterI* _shaderWriter, bx::WriterI* _messageWriter)
{
    BX_UNUSED (_options, _version, _code, _shaderWriter);
    bx::Error messageErr;
    bx::write (_messageWriter, &messageErr,
               "GLSL optimizer compiler is not compiled in (EvP builds shaderc for dxbc only).\n");
    return false;
}

}   // namespace bgfx
