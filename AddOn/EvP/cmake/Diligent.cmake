# Diligent: Core + Tools + FX, the modules the viewport actually uses.
#
# ⚠️ THIS CONFIGURE STEP REACHES THE NETWORK, ONCE. DiligentFX's own CMakeLists
# does a FetchContent of entt (github.com/skypjack/entt, v3.16.0) at configure
# time; there is no vendored copy and no option to skip it. CMake caches the
# clone under the build tree, so it costs one download per fresh build
# directory, not one per build. A machine configuring offline for the first time
# will fail HERE, with entt named in the message.
#
# Draco is the other fetch in the tree and it is NOT triggered:
# DILIGENT_ENABLE_DRACO defaults OFF in DiligentTools, and the glTF loader is
# the only thing that would want it.
#
# The modules are added in the order DiligentEngine's own top-level CMakeLists
# adds them, because FX hard-fails without DILIGENT_TOOLS_FOUND and Tools
# hard-fails without DILIGENT_CORE_FOUND.
set (DILIGENT_DIR "${REF_DIR}/DiligentEngine-master")
if (NOT EXISTS "${DILIGENT_DIR}/DiligentCore/CMakeLists.txt")
    message (FATAL_ERROR "DiligentCore is missing at '${DILIGENT_DIR}'.")
endif ()

set (DILIGENT_NO_DIRECT3D12 OFF CACHE BOOL "" FORCE)
set (DILIGENT_NO_VULKAN     ON CACHE BOOL "" FORCE)
set (DILIGENT_NO_OPENGL     ON CACHE BOOL "" FORCE)
set (DILIGENT_NO_WEBGPU     ON CACHE BOOL "" FORCE)
set (DILIGENT_NO_ARCHIVER   ON CACHE BOOL "" FORCE)
set (DILIGENT_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set (DILIGENT_BUILD_TOOLS   ON  CACHE BOOL "" FORCE)
set (DILIGENT_BUILD_FX      ON  CACHE BOOL "" FORCE)
set (DILIGENT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
# ⚠️ THE THINGS INSIDE Tools/FX THAT WOULD COST MORE THAN THEY GIVE. Draco is a
# whole compression library for a glTF feature nothing here loads, and it is the
# SECOND network fetch in the tree -- leaving it off is what keeps the configure
# to one download. Radient is an ECS-based renderer we do not use; the packager
# is an offline tool.
set (DILIGENT_ENABLE_DRACO          OFF CACHE BOOL "" FORCE)
set (DILIGENT_NO_RENDER_STATE_PACKAGER ON CACHE BOOL "" FORCE)
set (DILIGENT_NO_RADIENT            ON  CACHE BOOL "" FORCE)
set (DILIGENT_INSTALL_CORE  OFF CACHE BOOL "" FORCE)

# Host-process safety beats renderer throughput in Probe 1c. Diligent defaults
# every release target to /GL + /arch:AVX2. The latter is not valid as a minimum
# for Archicad (and this VM does not expose AVX2); static-library initializers
# can execute while the .apx is loading, before the probe is requested. Keep the
# probe binaries at MSVC's x64 baseline until in-process coexistence is proven.
set (DILIGENT_MSVC_RELEASE_COMPILE_OPTIONS "" CACHE STRING "" FORCE)

# ATL is installed beside the toolset rather than under VC/Tools/MSVC/include,
# so CMake's compiler defaults (and DiligentCore's own try_compile) cannot find
# it unaided. Put the exact pinned toolset directory into CMAKE_CXX_FLAGS before
# add_subdirectory: try_compile inherits this variable, target include paths do
# not. This is also an actionable preflight instead of Diligent's misleading
# generic "No rendering backends" failure.
string (REGEX MATCH "^[0-9]+\\.[0-9]+" EVP_MSVC_MAJOR_MINOR "${CMAKE_VS_PLATFORM_TOOLSET_VERSION}")
file (GLOB EVP_ATL_HEADERS
    "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/MSVC/${EVP_MSVC_MAJOR_MINOR}*/atlmfc/include/atlbase.h")
list (LENGTH EVP_ATL_HEADERS EVP_ATL_HEADER_COUNT)
if (EVP_ATL_HEADER_COUNT EQUAL 0)
    message (FATAL_ERROR "Diligent D3D11 needs ATL for MSVC ${EVP_MSVC_MAJOR_MINOR} under '${CMAKE_GENERATOR_INSTANCE}'. Install C++ ATL for the pinned v143 toolset.")
endif ()
list (GET EVP_ATL_HEADERS 0 EVP_ATL_HEADER)
get_filename_component (EVP_ATL_INCLUDE "${EVP_ATL_HEADER}" DIRECTORY)
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /I\"${EVP_ATL_INCLUDE}\"")

# DiligentCore's bare project() enables C and C++. The add-on is C++-only and
# this VS installation exposes cl.exe through the C++ generator setup, so give
# nested CMake detection that same compiler explicitly.
if (NOT CMAKE_C_COMPILER)
    set (CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "DiligentCore C compiler" FORCE)
endif ()

add_subdirectory ("${DILIGENT_DIR}/DiligentCore" "${CMAKE_BINARY_DIR}/DiligentCore" EXCLUDE_FROM_ALL)

# ---- Tools and FX ----------------------------------------------------------
# ⚠️ DiligentTools BRINGS Diligent-Imgui WITH IT, so it is no longer added
# separately -- and it supplies its own vendored imgui 1.92.1, the version
# ImGuiDiligentRenderer is written against.
#
# ⚠️ THERE MUST BE EXACTLY ONE DEAR IMGUI IN THE .apx, and this is now the only
# one. bgfx shipped a PATCHED copy (1.92.8 WIP) whose imconfig.h pulls in bx
# headers, so Diligent-Imgui could not be built against it; the two were kept
# apart by an `EVP_BGFX_HUD` option that was permanently OFF. bgfx and that
# option are gone (PLAT-RE66), and with them the whole hazard — but the rule
# survives its cause: linking two dear imgui copies is a duplicate-symbol
# failure at best, and a silent ABI mismatch between one copy's headers and the
# other's implementation at worst.
add_subdirectory ("${DILIGENT_DIR}/DiligentTools" "${CMAKE_BINARY_DIR}/DiligentTools"
                  EXCLUDE_FROM_ALL)
add_subdirectory ("${DILIGENT_DIR}/DiligentFX" "${CMAKE_BINARY_DIR}/DiligentFX"
                  EXCLUDE_FROM_ALL)

foreach (tgt Diligent-Imgui DiligentFX)
    if (TARGET ${tgt})
        set_target_properties (${tgt} PROPERTIES FOLDER ThirdParty)
    endif ()
endforeach ()

# Explicit Common is required by consumers: GraphicsEngineD3D11 links it
# privately, so RefCntAutoPtr.hpp would otherwise not propagate.
set (DILIGENT_PROBE_LIBS
    Diligent-Common
    Diligent-GraphicsEngineD3D11-static
    # RE51.D1 only: an isolated, explicitly invoked D3D12 feasibility probe.
    # The production viewport remains D3D11 until RE51.D2 is accepted.
    Diligent-GraphicsEngineD3D12-static
    # GraphicsTools is what DiligentFX's components are built on (RenderStateCache,
    # MapHelper, CommonlyUsedStates) and it is linked PRIVATELY by the engine, so
    # a consumer has to ask for it by name.
    Diligent-GraphicsTools
    Diligent-TextureLoader
    Diligent-Imgui
    DiligentFX
    d3d11
    dxgi
    # DirectComposition, for the transparent click-through overlay
    # (ArchViz/DiligentViewportTarget, PLAT-RE37). ⚠️ IT IS A SEPARATE IMPORT
    # LIBRARY FROM dxgi: `CreateSwapChainForComposition` lives in dxgi, but
    # `DCompositionCreateDevice` does not, and the only symptom of leaving this
    # out is one LNK2019 at the very end of a five-minute link.
    dcomp)
