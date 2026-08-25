cmake_minimum_required (VERSION 3.16)

# ============================================================================
# EvP (formerly "GeometryServer") — Archicad geometry & query HTTP server add-on
# AC29 first (AC27 port later — see archicad-geometry-addon-plan.md M9).
# Build pattern adapted from specklesystems/speckle-archicad (hand-rolled, no
# LP_XMLConverter needed; resources compiled via DevKit Support/Tools ResConv).
# ============================================================================

set (ADDON_NAME "EvP")
set (ADDON_LIB  "EvPAddOn")
set (ADDON_RES  "EvPResources")
set (PY_BRIDGE  "EvPPy")     # CPython half, built as its own DLL — see Sources/EvPPy/EvPPyApi.h

project (${ADDON_NAME} CXX)

# ---- Options / paths -------------------------------------------------------
# AC_API_DEVKIT_DIR must point to the folder that CONTAINS "Support"
# (i.e. .../acapi29), matching Speckle's convention and CompileResources.py.
if (NOT AC_API_DEVKIT_DIR)
    if (DEFINED ENV{AC_API_DEVKIT_DIR})
        set (AC_API_DEVKIT_DIR $ENV{AC_API_DEVKIT_DIR})
    else ()
        set (AC_API_DEVKIT_DIR "${CMAKE_SOURCE_DIR}/../reference/archicad29-api-devkit")
    endif ()
endif ()
set (AC_API_DEVKIT_DIR "${AC_API_DEVKIT_DIR}" CACHE PATH "API DevKit dir (contains /Support)")

# Default localization resource language (RINT = international/English).
if (NOT AC_ADDON_LANGUAGE)
    set (AC_ADDON_LANGUAGE "INT")
endif ()
set (AC_ADDON_LANGUAGE "${AC_ADDON_LANGUAGE}" CACHE STRING "Resource language code (folder R<code>)")

# Header-only third-party dependencies (vendored under ../reference).
set (REF_DIR "${CMAKE_SOURCE_DIR}/../reference")
set (HTTPLIB_INC "${REF_DIR}/cpp-httplib-master"        CACHE PATH "cpp-httplib include dir")
set (MSGPACK_INC "${REF_DIR}/msgpack-c-cpp_master/include" CACHE PATH "msgpack-c include dir")
set (NANORT_INC  "${REF_DIR}/nanort-release"            CACHE PATH "nanort include dir")

# Embedded CPython (P0+). Build-time only: headers + import lib are vendored, but
# python312.dll itself is DELAY-LOADED at runtime from wherever PythonHost
# resolves it (%EVP_PYTHON_HOME% / %LOCALAPPDATA%\EvP\runtime / an installed
# 3.12). Never statically link CPython — see docs/architecture/commands/SPEC.md.
set (PYTHON_DIR  "${REF_DIR}/python312"                 CACHE PATH "Vendored CPython 3.12 headers + import lib")

# ---- Detect Archicad version from the DevKit -------------------------------
set (ACAPINC_FILE "${AC_API_DEVKIT_DIR}/Support/Inc/ACAPinc.h")
if (EXISTS ${ACAPINC_FILE})
    file (READ ${ACAPINC_FILE} ACAPIncContent)
    # MATCHALL leaves CMAKE_MATCH_1 as the LAST match — the newest ServerMainVers_
    # (e.g. ServerMainVers_2900 -> "29"). Same trick as speckle-archicad.
    string (REGEX MATCHALL "#define[ \t]+ServerMainVers_([0-9][0-9])" _ ${ACAPIncContent})
    set (ARCHICAD_VERSION ${CMAKE_MATCH_1})
    message (STATUS "GeometryServer: Archicad Version = ${ARCHICAD_VERSION}")
    message (STATUS "GeometryServer: DevKit          = ${AC_API_DEVKIT_DIR}")
else ()
    message (FATAL_ERROR "ACAPinc.h not found under '${AC_API_DEVKIT_DIR}/Support/Inc'. "
                         "Set -DAC_API_DEVKIT_DIR=<folder containing /Support>.")
endif ()

set_property (GLOBAL PROPERTY USE_FOLDERS ON)
set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)

if (WIN32)
    add_definitions (-DUNICODE -D_UNICODE -D_ITERATOR_DEBUG_LEVEL=0)
    set (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
else ()
    add_definitions (-Dmacintosh=1)
endif ()
add_definitions (-DACExtension)

set (AddOnSourcesFolder   Sources/AddOn)
set (AddOnResourcesFolder Sources/AddOnResources)
set (PyBridgeSourcesFolder Sources/EvPPy)
set (ResourceObjectsDir "${CMAKE_BINARY_DIR}/ResourceObjects")
file (MAKE_DIRECTORY "${ResourceObjectsDir}")

# ---- Notebook browser UI ---------------------------------------------------
set (NotebookUIFolder "${CMAKE_CURRENT_LIST_DIR}/NotebookUI")
set (NotebookUIOutput "${NotebookUIFolder}/dist/index.html")
set (NOTEBOOK_UI_RESOURCE_ID 32551)
file (GLOB_RECURSE NotebookUIFiles CONFIGURE_DEPENDS
    "${NotebookUIFolder}/src/*"
    "${NotebookUIFolder}/scripts/*")
list (APPEND NotebookUIFiles
    "${NotebookUIFolder}/index.html"
    "${NotebookUIFolder}/package.json"
    "${NotebookUIFolder}/package-lock.json"
    "${NotebookUIFolder}/tsconfig.json"
    "${NotebookUIFolder}/vite.config.ts")

find_program (NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
add_custom_command (
    OUTPUT "${NotebookUIOutput}"
    DEPENDS ${NotebookUIFiles}
    WORKING_DIRECTORY "${NotebookUIFolder}"
    COMMENT "EvP: building the self-contained Tapioca Notebook UI"
    COMMAND "${NPM_EXECUTABLE}" ci
    COMMAND "${NPM_EXECUTABLE}" run typecheck
    COMMAND "${NPM_EXECUTABLE}" run build
    VERBATIM)
add_custom_target (EvPNotebookUI ALL DEPENDS "${NotebookUIOutput}")
set_target_properties (EvPNotebookUI PROPERTIES FOLDER Resources)

# ---- WebUI command palette preview -----------------------------------------
# This first WebUI slice is intentionally a self-contained visual page. Keeping
# it as a DATA resource gives the native palette the same no-server fallback and
# deployment guarantees as the Notebook browser while the command bridge is built.
set (WebUIFolder "${CMAKE_CURRENT_LIST_DIR}/WebUI")
set (WEBUI_PANEL_RESOURCE_ID 32561)
configure_file (
    "${AddOnResourcesFolder}/RFIX.win/WebUIPanel.grc.in"
    "${ResourceObjectsDir}/WebUIPanel.grc"
    @ONLY)
set (WebUIResourceRc2 "${ResourceObjectsDir}/WebUIPanel.grc.rc2")
if (WIN32)
    add_custom_command (
        OUTPUT "${WebUIResourceRc2}"
        DEPENDS "${WebUIFolder}/index.html" "${ResourceObjectsDir}/WebUIPanel.grc"
        COMMENT "EvP: converting the Tapioca WebUI HTML resource"
        COMMAND "${AC_API_DEVKIT_DIR}/Support/Tools/Win/ResConv.exe"
            -m r -T W -q utf8 1252
            -p "${WebUIFolder}"
            -i "${ResourceObjectsDir}/WebUIPanel.grc"
            -o "${WebUIResourceRc2}"
        VERBATIM)
endif ()

# Generate this DATA resource through Graphisoft's ResConv, not directly through
# rc.exe. RSLoadResource expects ResConv's numeric type and length-prefixed data.
configure_file (
    "${AddOnResourcesFolder}/RFIX.win/NotebookUI.grc.in"
    "${ResourceObjectsDir}/NotebookUI.grc"
    @ONLY)
set (NotebookUIResourceRc2 "${ResourceObjectsDir}/NotebookUI.grc.rc2")
if (WIN32)
    add_custom_command (
        OUTPUT "${NotebookUIResourceRc2}"
        DEPENDS "${NotebookUIOutput}" "${ResourceObjectsDir}/NotebookUI.grc"
        COMMENT "EvP: converting the Tapioca Notebook HTML resource"
        COMMAND "${AC_API_DEVKIT_DIR}/Support/Tools/Win/ResConv.exe"
            -m r -T W -q utf8 1252
            -p "${NotebookUIFolder}"
            -i "${ResourceObjectsDir}/NotebookUI.grc"
            -o "${NotebookUIResourceRc2}"
        VERBATIM)
endif ()

# ============================================================================
# Resources: .grc -> .res  (ResConv from DevKit + Windows rc)
# ============================================================================
# Unique add-on identifier (MDID). REPLACE with a registered pair before release;
# arbitrary unique values are fine for internal/dev loading.
#
# RFIX/AddOnFix.grc is the SINGLE SOURCE OF TRUTH: that is the 'MDID' resource
# Archicad actually registers the add-on under, so it is what any C++ naming the
# MDID must agree with (P1's CallFromEventLoop targets our OWN add-on by MDID —
# a mismatch there is a bare APIERR_MODULNOTINSTALLED, painful to diagnose).
# These values used to be duplicated here and HAD silently drifted from the .grc,
# so parse them out instead of keeping a second copy.
file (READ "${CMAKE_CURRENT_LIST_DIR}/${AddOnResourcesFolder}/RFIX/AddOnFix.grc" AddOnFixContent)
string (REGEX MATCH "'MDID'[^{]*{[ \t\r\n]*([0-9]+)[ \t\r\n]+([0-9]+)" MdidMatch "${AddOnFixContent}")
if (NOT MdidMatch)
    message (FATAL_ERROR "Could not parse the 'MDID' resource out of AddOnResources/RFIX/AddOnFix.grc.")
endif ()
set (AC_MDID_DEV ${CMAKE_MATCH_1})
set (AC_MDID_LOC ${CMAKE_MATCH_2})
message (STATUS "EvP: MDID = ${AC_MDID_DEV} / ${AC_MDID_LOC} (parsed from AddOnFix.grc)")
configure_file (${AddOnResourcesFolder}/ResourceMDIDIds.hpp.in
                ${ResourceObjectsDir}/ResourceMDIDIds.hpp @ONLY)

file (GLOB AddOnImageFiles ${AddOnResourcesFolder}/RFIX/Images/*.svg)
file (GLOB AddOnResourceFiles
    ${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
    ${AddOnResourcesFolder}/RFIX/*.grc
    ${AddOnResourcesFolder}/RFIX.win/*.rc2
)

get_filename_component (AddOnSourcesFolderAbsolute   "${CMAKE_CURRENT_LIST_DIR}/${AddOnSourcesFolder}"   ABSOLUTE)
get_filename_component (AddOnResourcesFolderAbsolute "${CMAKE_CURRENT_LIST_DIR}/${AddOnResourcesFolder}" ABSOLUTE)

add_custom_command (
    OUTPUT "${ResourceObjectsDir}/AddOnResources.stamp"
    DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles} "${NotebookUIOutput}" "${NotebookUIResourceRc2}"
            "${WebUIResourceRc2}"
    COMMENT "GeometryServer: compiling resources..."
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
    COMMAND python "${AddOnResourcesFolderAbsolute}/Tools/CompileResources.py"
        "${AC_ADDON_LANGUAGE}"
        "${AC_API_DEVKIT_DIR}"
        "${AddOnSourcesFolderAbsolute}"
        "${AddOnResourcesFolderAbsolute}"
        "${ResourceObjectsDir}"
        "${ResourceObjectsDir}/${ADDON_NAME}.res"
    COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
)
add_custom_target (${ADDON_RES} ALL DEPENDS "${ResourceObjectsDir}/AddOnResources.stamp")
add_dependencies (${ADDON_RES} EvPNotebookUI)
set_target_properties (${ADDON_RES} PROPERTIES FOLDER Resources)
source_group ("Resources" FILES ${AddOnResourceFiles} ${AddOnImageFiles})

# ============================================================================
# Add-On target
# ============================================================================
# CONFIGURE_DEPENDS makes CMake re-glob at build time, so newly added source
# files are picked up without a manual reconfigure.
file (GLOB_RECURSE AddOnHeaderFiles CONFIGURE_DEPENDS ${AddOnSourcesFolder}/*.h ${AddOnSourcesFolder}/*.hpp)
file (GLOB_RECURSE AddOnSourceFiles CONFIGURE_DEPENDS ${AddOnSourcesFolder}/*.c ${AddOnSourcesFolder}/*.cpp)

# ---- ArchViz: the Diligent renderer ----------------------------------------
# ⚠️ cmake/Bgfx.cmake IS GONE, AND WITH IT THE `archviz_render` SEAM
# (PLAT-RE66). bgfx needed one: bx's headers cannot survive the .apx's
# /Zc:wchar_t-, so six ArchViz TUs had to be compiled into a separate static
# library with normal flags -- and the price was that those six could not see
# GS::UniString, DG or any DevKit type. Probe 1a established that Diligent needs
# no such split (docs/architecture/diligent/HANDOFF.md §2), so every ArchViz source is
# now an ordinary member of the .apx target and there is no REMOVE_ITEM, no
# ARCHVIZ_EXCLUDED_SOURCES and no second set of compile flags to keep in step.
#
# The bgfx plan and reference docs are KEPT, marked superseded: they carry the
# handedness, cull-winding and sticky-init lessons this renderer inherited
# without re-deriving them.
include ("${CMAKE_CURRENT_LIST_DIR}/cmake/Diligent.cmake")

set (AddOnFiles ${AddOnHeaderFiles} ${AddOnSourceFiles})
source_group (TREE "${AddOnSourcesFolderAbsolute}" PREFIX "Sources" FILES ${AddOnFiles})

if (WIN32)
    add_library (${ADDON_LIB} SHARED ${AddOnFiles})
else ()
    add_library (${ADDON_LIB} MODULE ${AddOnFiles})
endif ()

set_target_properties (${ADDON_LIB} PROPERTIES OUTPUT_NAME ${ADDON_NAME})
target_compile_features (${ADDON_LIB} PUBLIC cxx_std_20)   # AC29 -> C++20
# ADDON_VERSION and the user-facing product name are NOT defined here any more:
# they live in Sources/AddOn/AddOnVersion.hpp, so they are greppable and a bump is
# one edit in one place rather than a build-system change (F6/F7).
target_compile_definitions (${ADDON_LIB} PRIVATE
    AC${ARCHICAD_VERSION}
    AC_VERSION_NUM=${ARCHICAD_VERSION})

if (WIN32)
    set_target_properties (${ADDON_LIB} PROPERTIES SUFFIX ".apx")
    # VS is a multi-config generator and would append \<Config> to the output
    # dir; pin every config to build_29/ so the .apx path is stable.
    foreach (cfg DEBUG RELEASE RELWITHDEBINFO)
        set_target_properties (${ADDON_LIB} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}")
    endforeach ()
    target_link_options (${ADDON_LIB} PRIVATE
        "${ResourceObjectsDir}/${ADDON_NAME}.res"
        "/export:GetExportedFuncAddrs,@1"
        "/export:SetImportedFuncAddrs,@2")
    # APIdefs_Callback.h instantiates dynamic_cast helpers while the DevKit's
    # add-on configuration deliberately uses /GR-. MSVC reports C4541 in every
    # TU that includes it (including ScreenshotCapture.cpp); it is a DevKit
    # implementation warning, not an add-on RTTI policy change. Keep /GR- and
    # suppress only that known external-header diagnostic.
    target_compile_options (${ADDON_LIB} PRIVATE /W3 /MP /Zc:wchar_t- /EHsc
        /wd4499 /wd4541 /wd4996 /wd5208 -D_CRT_SECURE_NO_WARNINGS)
    # Optional: launch Archicad for debugging (adjust path to your install).
    set_target_properties (${ADDON_LIB} PROPERTIES
        VS_DEBUGGER_COMMAND "C:\\Program Files\\GRAPHISOFT\\Archicad 29\\Archicad.exe")
endif ()

# ---- Includes --------------------------------------------------------------
target_include_directories (${ADDON_LIB} PRIVATE
    ${AddOnSourcesFolder}
    ${PyBridgeSourcesFolder}                  # EvPPyApi.h only — NOT the CPython headers
    ${ResourceObjectsDir}                     # generated ResourceMDIDIds.hpp
    ${AC_API_DEVKIT_DIR}/Support/Inc)
target_include_directories (${ADDON_LIB} SYSTEM PRIVATE
    ${HTTPLIB_INC} ${MSGPACK_INC} ${NANORT_INC})

file (GLOB ModuleFolders ${AC_API_DEVKIT_DIR}/Support/Modules/*)
target_include_directories (${ADDON_LIB} SYSTEM PRIVATE ${ModuleFolders})

target_link_libraries (${ADDON_LIB} ${DILIGENT_PROBE_LIBS})
# d3dcompiler/dxguid/gdi32 came in through ARCHVIZ_LIBS with bgfx. The Diligent
# D3D11 backend compiles its HLSL at runtime, so d3dcompiler is still needed and
# is now named here rather than inherited from a renderer that is gone.
target_link_libraries (${ADDON_LIB} d3dcompiler dxguid gdi32)
# winmm: timeBeginPeriod, which the overlay's camera sync holds while it is armed.
# Without it SetTimer cannot fire between Windows' 15.625 ms ticks and the sync
# interval is only advisory — see ArchVizPanelCamera's HoldTimerResolution.
target_link_libraries (${ADDON_LIB} winmm)

# ---- Link ------------------------------------------------------------------
if (WIN32)
    # Official Graphisoft kit keeps ACAP_STAT.lib flat under Lib/; some vendored
    # copies nest it in Lib/Win/. Support both.
    if (EXISTS "${AC_API_DEVKIT_DIR}/Support/Lib/Win/ACAP_STAT.lib")
        set (ACAP_STAT_LIB "${AC_API_DEVKIT_DIR}/Support/Lib/Win/ACAP_STAT.lib")
    else ()
        set (ACAP_STAT_LIB "${AC_API_DEVKIT_DIR}/Support/Lib/ACAP_STAT.lib")
    endif ()
    target_link_libraries (${ADDON_LIB} "${ACAP_STAT_LIB}")
    file (GLOB ModuleLibs ${AC_API_DEVKIT_DIR}/Support/Modules/*/*/*.lib)
    target_link_libraries (${ADDON_LIB} ${ModuleLibs})
endif ()

set_source_files_properties (${AddOnSourceFiles} PROPERTIES LANGUAGE CXX)
add_dependencies (${ADDON_LIB} ${ADDON_RES})

# ============================================================================
# EvPPy.dll — the CPython half
# ============================================================================
# Separate binary because python312.dll exports DATA symbols (_Py_NoneStruct,
# the type objects behind every Py*_Check, every PyExc_*), and the Windows
# loader cannot delay-load a data import (LNK1194). A binary using the C API
# must therefore import python312.dll at load time — which the .apx must not do,
# or Archicad would fail to load EvP whenever Python isn't on PATH. See
# Sources/EvPPy/EvPPyApi.h for the full rationale and the ABI contract.
#
# Built with STANDARD flags: no /Zc:wchar_t-, no Archicad headers, no SDK libs.
# The .apx's forced flag stops at the extern "C" POD boundary.
if (WIN32)
    if (NOT EXISTS "${PYTHON_DIR}/libs/python312.lib")
        message (FATAL_ERROR "python312.lib not found under '${PYTHON_DIR}/libs'. "
                             "Vendor CPython 3.12's include/ and libs/ there, or set -DPYTHON_DIR=<dir>.")
    endif ()

    file (GLOB_RECURSE PyBridgeFiles CONFIGURE_DEPENDS
        ${PyBridgeSourcesFolder}/*.h ${PyBridgeSourcesFolder}/*.cpp)
    add_library (${PY_BRIDGE} SHARED ${PyBridgeFiles})
    source_group ("Sources" FILES ${PyBridgeFiles})

    set_target_properties (${PY_BRIDGE} PROPERTIES OUTPUT_NAME ${PY_BRIDGE})
    target_compile_features (${PY_BRIDGE} PUBLIC cxx_std_20)
    target_compile_options (${PY_BRIDGE} PRIVATE /W3 /MP /EHsc -D_CRT_SECURE_NO_WARNINGS)
    target_include_directories (${PY_BRIDGE} SYSTEM PRIVATE ${PYTHON_DIR}/include)
    # pyconfig.h auto-links "python312.lib" by bare name, so its folder must be
    # searchable even though we also name the lib explicitly.
    target_link_directories (${PY_BRIDGE} PRIVATE "${PYTHON_DIR}/libs")
    target_link_libraries (${PY_BRIDGE} "${PYTHON_DIR}/libs/python312.lib")

    # Must land next to EvP.apx: PythonHost loads it relative to its own module.
    foreach (cfg DEBUG RELEASE RELWITHDEBINFO)
        set_target_properties (${PY_BRIDGE} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}")
    endforeach ()

    add_dependencies (${ADDON_LIB} ${PY_BRIDGE})

    # The `evp` Python package ships WITH the add-on and must sit beside the .apx:
    # PythonHost puts this folder at the FRONT of sys.path so nothing pip installs
    # can shadow it. Staged on every build so edits to the .py files take effect
    # without a manual copy. (P4 moves the writable environment to %LOCALAPPDATA%;
    # this folder stays read-only and shipped.)
    file (GLOB_RECURSE PyPackageFiles CONFIGURE_DEPENDS Sources/PyPackage/*.py)
    add_custom_target (EvPPyPackage ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_LIST_DIR}/Sources/PyPackage" "${CMAKE_BINARY_DIR}/PyPackage"
        DEPENDS ${PyPackageFiles}
        COMMENT "EvP: staging the evp Python package next to the .apx"
        SOURCES ${PyPackageFiles})
    set_target_properties (EvPPyPackage PROPERTIES FOLDER Resources)
    add_dependencies (${ADDON_LIB} EvPPyPackage)
endif ()

# ============================================================================
# Tapioca.GrasshopperHost.dll — the managed half of the Rhino.Inside host
# ============================================================================
# Separate binary for the same reason EvPPy.dll is one: it speaks a language the
# .apx cannot link against. The .apx stays native C++20 and imports NOTHING from
# .NET or from Rhino; it loads this assembly through hostfxr at run time, from a
# menu command, or reports why it could not. See Sources/AddOn/Grasshopper/.
#
# ⚠️ OPTIONAL BY DESIGN, AND THE `if` BELOW IS THE POINT. A machine without the
# .NET SDK still builds a complete, loadable EvP.apx; Rhino.Inside is then a
# menu item that explains what is missing. Making this a hard requirement would
# trade the whole add-on for one experimental feature.
#
# Framework-dependent publish, straight into the .apx's directory: the assembly,
# its .deps.json and its .runtimeconfig.json must sit BESIDE EvP.apx, because
# GrasshopperHost resolves them relative to its own module. Nothing here copies
# a Rhino runtime binary anywhere -- RhinoCommon and Grasshopper are compile-time
# references only (see the .csproj) and come from the user's installed Rhino.
set (GrasshopperManagedFolder "${CMAKE_CURRENT_LIST_DIR}/Sources/GrasshopperManaged")
set (GrasshopperManagedProject "${GrasshopperManagedFolder}/Tapioca.GrasshopperHost.csproj")
find_program (DOTNET_EXECUTABLE NAMES dotnet)
if (WIN32 AND DOTNET_EXECUTABLE AND EXISTS "${GrasshopperManagedProject}")
    file (GLOB GrasshopperManagedFiles CONFIGURE_DEPENDS
        "${GrasshopperManagedFolder}/*.cs" "${GrasshopperManagedFolder}/*.csproj")
    set (GrasshopperManagedStamp "${CMAKE_BINARY_DIR}/Tapioca.GrasshopperHost.stamp")
    add_custom_command (
        OUTPUT "${GrasshopperManagedStamp}"
        DEPENDS ${GrasshopperManagedFiles}
        COMMENT "EvP: publishing Tapioca.GrasshopperHost beside the .apx"
        COMMAND "${DOTNET_EXECUTABLE}" publish "${GrasshopperManagedProject}"
            -c Release -o "${CMAKE_BINARY_DIR}" --nologo
        COMMAND ${CMAKE_COMMAND} -E touch "${GrasshopperManagedStamp}"
        VERBATIM)
    add_custom_target (EvPGrasshopperManaged ALL DEPENDS "${GrasshopperManagedStamp}")
    set_target_properties (EvPGrasshopperManaged PROPERTIES FOLDER Resources)
    add_dependencies (${ADDON_LIB} EvPGrasshopperManaged)
else ()
    message (STATUS "EvP: the .NET SDK was not found; Tapioca.GrasshopperHost will not be built. "
                    "EvP.apx builds normally and reports the missing managed host if Rhino.Inside is used.")
endif ()
