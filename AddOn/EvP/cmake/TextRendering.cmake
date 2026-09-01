# FreeType + msdfgen + msdf-atlas-gen for the Diligent scene-text layer.
# Every dependency is provisioned through AddOn/reference; configuration must
# never invoke vcpkg or fetch source from the network.

set (FREETYPE_DIR "${REF_DIR}/freetype-master")
set (MSDFGEN_DIR "${REF_DIR}/msdfgen-master")
set (MSDF_ATLAS_DIR "${REF_DIR}/msdf-atlas-gen-master")

foreach (dependency_dir FREETYPE_DIR MSDFGEN_DIR MSDF_ATLAS_DIR)
    if (NOT EXISTS "${${dependency_dir}}/CMakeLists.txt")
        message (FATAL_ERROR "Text-rendering dependency is missing at '${${dependency_dir}}'. Run scripts/Link-Reference.ps1 or provision the build reference profile.")
    endif ()
endforeach ()

set (BUILD_SHARED_LIBS OFF CACHE BOOL "Build vendored text dependencies statically" FORCE)

# FreeType is used only for ordinary TTF/OTF font outlines. Disable optional
# machine-discovered libraries so two developers configure the same graph.
set (FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set (FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set (FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set (FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set (FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set (FT_DISABLE_HVF ON CACHE BOOL "" FORCE)
set (FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
set (SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory ("${FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/TextRendering/freetype" EXCLUDE_FROM_ALL)

# FreeType exposes this name only from an installed export. msdfgen correctly
# expects the same canonical FindFreetype target in an in-tree build.
if (NOT TARGET Freetype::Freetype)
    add_library (Freetype::Freetype ALIAS freetype-interface)
endif ()

# msdfgen-ext supplies FreeType-backed glyph-outline loading. SVG, PNG, Skia,
# OpenMP, vcpkg and the command-line program are outside the viewer contract.
set (MSDFGEN_CORE_ONLY OFF CACHE BOOL "" FORCE)
set (MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set (MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
set (MSDFGEN_USE_OPENMP OFF CACHE BOOL "" FORCE)
set (MSDFGEN_USE_CPP11 ON CACHE BOOL "" FORCE)
set (MSDFGEN_USE_SKIA OFF CACHE BOOL "" FORCE)
set (MSDFGEN_DISABLE_SVG ON CACHE BOOL "" FORCE)
set (MSDFGEN_DISABLE_PNG ON CACHE BOOL "" FORCE)
set (MSDFGEN_INSTALL OFF CACHE BOOL "" FORCE)
set (MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
add_subdirectory ("${MSDFGEN_DIR}" "${CMAKE_BINARY_DIR}/TextRendering/msdfgen" EXCLUDE_FROM_ALL)

# The atlas library consumes the explicit msdfgen target above. Its own source
# archive has empty submodule placeholders, so silently falling back to them is
# prohibited even on a machine that happens to have initialized one by hand.
set (MSDF_ATLAS_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set (MSDF_ATLAS_USE_VCPKG OFF CACHE BOOL "" FORCE)
set (MSDF_ATLAS_USE_SKIA OFF CACHE BOOL "" FORCE)
set (MSDF_ATLAS_NO_ARTERY_FONT ON CACHE BOOL "" FORCE)
set (MSDF_ATLAS_MSDFGEN_EXTERNAL ON CACHE BOOL "" FORCE)
set (MSDF_ATLAS_INSTALL OFF CACHE BOOL "" FORCE)
set (MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
add_subdirectory ("${MSDF_ATLAS_DIR}" "${CMAKE_BINARY_DIR}/TextRendering/msdf-atlas-gen" EXCLUDE_FROM_ALL)

# EvP intentionally uses /MD even in Debug. Upstream's dynamic-runtime options
# select /MDd there, so force all four static libraries onto the host contract.
if (MSVC)
    foreach (text_target freetype msdfgen-core msdfgen-ext msdf-atlas-gen)
        set_property (TARGET ${text_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endforeach ()
endif ()

foreach (text_target freetype msdfgen-core msdfgen-ext msdf-atlas-gen)
    set_target_properties (${text_target} PROPERTIES FOLDER ThirdParty)
endforeach ()

set (TAPIOCA_TEXT_LIBS
    Freetype::Freetype
    msdf-atlas-gen::msdf-atlas-gen)
