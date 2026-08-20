cmake_minimum_required (VERSION 3.20)
project (evp-shaderc CXX C)

# ============================================================================
# shaderc — bgfx's shader compiler, built HLSL-only, for this add-on's shaders.
#
# WHY IT EXISTS. bgfx-master/tools/bin/windows/ ships only the compiler BACK
# ENDS (d3dcompiler_47.dll, dxcompiler.dll, dxil.dll), never shaderc.exe. The
# 2135 prebuilt binaries under examples/runtime/shaders/ cover bgfx's OWN
# example shaders and nothing else, so the moment ArchViz has a shader of its
# own -- which is Phase 5 -- this has to exist.
#
# WHY IT IS ITS OWN PROJECT. It is a BUILD-TIME tool, not part of EvP.apx. It
# must not need the Archicad DevKit, must not inherit /Zc:wchar_t-, and must not
# make the add-on's configure step slower. It builds its own bx (one amalgamated
# translation unit) and depends on nothing else in the repo.
#
# WHY HLSL ONLY. shaderc self-configures on __has_include: leave glslang,
# spirv-tools, spirv-cross, tint and glsl-optimizer off the include path and
# those back ends compile themselves out, each returning "unsupported". An
# Archicad 29 Windows add-on renders through D3D11 and needs the `dxbc` profile;
# the others would be several minutes of build time for output nothing consumes.
# ⚠️ SHADERC_CONFIG_HAS_DXC defaults to 1 on Windows and is forced OFF here --
# DXIL is Shader Model 6, i.e. D3D12, which this add-on does not use.
#
# Build it:   .\Build-Shaderc.ps1        -> build\Release\shaderc.exe
# Use it:     .\Build-Shaders.ps1        -> ../../Shaders/*.sc to *.bin.h
# ============================================================================

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

get_filename_component (REF_DIR "${CMAKE_SOURCE_DIR}/../../../reference" ABSOLUTE)
set (BGFX_DIR "${REF_DIR}/bgfx-master")
set (BX_DIR   "${REF_DIR}/bx-master")

foreach (dir "${BGFX_DIR}" "${BX_DIR}")
    if (NOT EXISTS "${dir}")
        message (FATAL_ERROR "shaderc: '${dir}' is missing.")
    endif ()
endforeach ()

# Same conformance flags the add-on needs, for the same reason: bx/platform.h
# #errors without /Zc:__cplusplus and static_asserts on C++20.
add_compile_options ($<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus> $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

add_library (shaderc_bx STATIC "${BX_DIR}/src/amalgamated.cpp")
target_include_directories (shaderc_bx SYSTEM PUBLIC
    "${BX_DIR}/include" "${BX_DIR}/include/compat/msvc")
target_include_directories (shaderc_bx PRIVATE "${BX_DIR}/src")
target_compile_definitions (shaderc_bx PUBLIC BX_CONFIG_DEBUG=0)
target_compile_options (shaderc_bx PRIVATE /EHs-c- /bigobj /W0)

# fcpp is shaderc's C preprocessor — plain C, and shaderc.cpp includes <fpp.h>
# from it. Not optional: every .sc goes through it before any back end sees it.
add_library (shaderc_fcpp STATIC
    "${BGFX_DIR}/3rdparty/fcpp/cpp1.c"
    "${BGFX_DIR}/3rdparty/fcpp/cpp2.c"
    "${BGFX_DIR}/3rdparty/fcpp/cpp3.c"
    "${BGFX_DIR}/3rdparty/fcpp/cpp4.c"
    "${BGFX_DIR}/3rdparty/fcpp/cpp5.c"
    "${BGFX_DIR}/3rdparty/fcpp/cpp6.c")
target_include_directories (shaderc_fcpp PUBLIC "${BGFX_DIR}/3rdparty/fcpp")
target_compile_definitions (shaderc_fcpp PRIVATE NINCLUDE=64 NWORK=65536 NBUFF=65536 OLD_PREPROCESSOR=0)
target_compile_options (shaderc_fcpp PRIVATE /W0)

# Every back end is listed. The ones with no library behind them guard
# themselves out on __has_include and reduce to a "not supported" stub, so this
# list does not have to change when one is enabled later.
add_executable (shaderc
    "${BGFX_DIR}/tools/shaderc/shaderc.cpp"
    "${BGFX_DIR}/tools/shaderc/shaderc_hlsl.cpp"
    # ⚠️ NOT shaderc_glsl.cpp — its no-glsl-optimizer branch defines
    # compileSPIRVShader instead of compileGLSLShader (a copy-paste bug in the
    # vendored source), which is one duplicate symbol and one unresolved one.
    # shaderc_glsl_stub.cpp beside this file supplies what it should have.
    "${CMAKE_SOURCE_DIR}/shaderc_glsl_stub.cpp"
    "${BGFX_DIR}/tools/shaderc/shaderc_spirv.cpp"
    "${BGFX_DIR}/tools/shaderc/shaderc_metal.cpp"
    "${BGFX_DIR}/tools/shaderc/shaderc_pssl.cpp"
    "${BGFX_DIR}/tools/shaderc/shaderc_wgsl.cpp"
    "${BGFX_DIR}/tools/shaderc/shaderc_dxil.cpp"
    # shaderc.h includes ../../src/vertexlayout.h and the linker wants its body:
    # a .sc's `varying.def.sc` becomes a bgfx::VertexLayout in the output blob.
    "${BGFX_DIR}/src/vertexlayout.cpp")
target_include_directories (shaderc PRIVATE
    "${BGFX_DIR}/include"
    "${BGFX_DIR}/tools/shaderc"
    "${BGFX_DIR}/src")
target_compile_definitions (shaderc PRIVATE
    SHADERC_CONFIG_HAS_DXC=0)   # DXIL is Shader Model 6 / D3D12; not used here
target_link_libraries (shaderc PRIVATE shaderc_bx shaderc_fcpp)
target_compile_options (shaderc PRIVATE /bigobj /W0)

if (WIN32)
    # d3dcompiler is LoadLibrary'd by shaderc_hlsl at runtime, but the import lib
    # is needed for the D3D_ constants it links against.
    target_link_libraries (shaderc PRIVATE d3dcompiler dxguid)
endif ()

# Straight into the tool folder, so Build-Shaders.ps1 does not have to know
# which generator or config produced it.
foreach (cfg DEBUG RELEASE RELWITHDEBINFO)
    set_target_properties (shaderc PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_SOURCE_DIR}/bin")
endforeach ()
set_target_properties (shaderc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
