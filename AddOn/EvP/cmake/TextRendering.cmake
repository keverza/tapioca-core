# FreeType + msdfgen + msdf-atlas-gen for the Diligent scene-text layer.
# Every dependency is provisioned through AddOn/reference; configuration must
# never invoke vcpkg or fetch source from the network.

set (FREETYPE_DIR "${REF_DIR}/freetype-master")
set (HARFBUZZ_DIR "${REF_DIR}/harfbuzz-main")
set (MSDFGEN_DIR "${REF_DIR}/msdfgen-master")
set (MSDF_ATLAS_DIR "${REF_DIR}/msdf-atlas-gen-master")
set (TAPIOCA_TEXT_FONT_DIR "${REF_DIR}/noto-sans-font-v2.015/hinted/ttf")
set (TAPIOCA_TEXT_FONT "${TAPIOCA_TEXT_FONT_DIR}/NotoSans-Regular.ttf")

foreach (dependency_dir FREETYPE_DIR HARFBUZZ_DIR MSDFGEN_DIR MSDF_ATLAS_DIR)
    if (NOT EXISTS "${${dependency_dir}}/CMakeLists.txt")
        message (FATAL_ERROR "Text-rendering dependency is missing at '${${dependency_dir}}'. Run scripts/Link-Reference.ps1 or provision the build reference profile.")
    endif ()
endforeach ()
if (NOT EXISTS "${TAPIOCA_TEXT_FONT}")
    message (FATAL_ERROR "Bundled text font is missing at '${TAPIOCA_TEXT_FONT}'. Run scripts/Link-Reference.ps1 or provision the build reference profile.")
endif ()

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

# HarfBuzz supplies OpenType shaping and its FreeType bridge only. The subset,
# raster, vector, GPU and platform integrations are separate products and would
# add dependencies that scene labels neither expose nor use.
set (HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
set (HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set (HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
set (HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set (HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set (HB_HAVE_UNISCRIBE OFF CACHE BOOL "" FORCE)
set (HB_HAVE_GDI OFF CACHE BOOL "" FORCE)
set (HB_HAVE_DIRECTWRITE OFF CACHE BOOL "" FORCE)
set (HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set (HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set (HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set (HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set (HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
set (HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
set (HB_HAVE_INTROSPECTION OFF CACHE BOOL "" FORCE)
add_subdirectory ("${HARFBUZZ_DIR}" "${CMAKE_BINARY_DIR}/TextRendering/harfbuzz" EXCLUDE_FROM_ALL)

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

# THE RUNTIME LIBRARY IS PINNED EITHER WAY, NEVER LEFT TO UPSTREAM.
#
# ⚠️ AN UNPINNED CONSUMER IS A LINK FAILURE WAITING FOR A REBUILD. EvP.apx uses
# /MD even in Debug and sets TAPIOCA_TEXT_FORCE_HOST_RUNTIME for it. The offline
# C++ suite does not, and leaving the else branch empty meant these four
# libraries took whatever MSDFGEN_DYNAMIC_RUNTIME chose - plain /MD - inside a
# /MDd test executable. Nothing failed until something touched the configure
# step, and then the whole suite stopped linking with fifty-two LNK2038s that
# named none of the code that changed. So the second consumer states its
# contract as explicitly as the first: the CMake default, matching the config.
if (MSVC)
    if (TAPIOCA_TEXT_FORCE_HOST_RUNTIME)
        set (tapioca_text_runtime "MultiThreadedDLL")
    else ()
        set (tapioca_text_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif ()
    foreach (text_target freetype harfbuzz msdfgen-core msdfgen-ext msdf-atlas-gen)
        set_property (TARGET ${text_target} PROPERTY MSVC_RUNTIME_LIBRARY "${tapioca_text_runtime}")
    endforeach ()
endif ()
if (MSVC)
    foreach (text_target freetype harfbuzz msdfgen-core msdfgen-ext msdf-atlas-gen)
        target_compile_options (${text_target} PRIVATE /FS)
    endforeach ()
endif ()

foreach (text_target freetype harfbuzz msdfgen-core msdfgen-ext msdf-atlas-gen)
    set_target_properties (${text_target} PROPERTIES FOLDER ThirdParty)
endforeach ()

set (TAPIOCA_TEXT_LIBS
    Freetype::Freetype
    harfbuzz
    msdf-atlas-gen::msdf-atlas-gen)
