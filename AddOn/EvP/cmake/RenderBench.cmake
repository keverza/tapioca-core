# TapiocaRenderBench: the viewer's renderer as a standalone executable (tools/RenderBench).
#
# ⚠️ EXCLUDE_FROM_ALL: the add-on build never builds it. Build it on purpose:
#   cmake --build build_29 --config RelWithDebInfo --target TapiocaRenderBench
# It lives in this tree, not a project of its own, because it links the Diligent libraries this
# configure already builds -- a second tree would compile Diligent a second time.
#
# ⚠️ NO DEVKIT. The sources below are the render half of ArchViz only; a source that reaches
# ACAPI does not belong in the list, and the link failing is how that is found out.

set (RENDER_BENCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools/RenderBench")
set (RENDER_BENCH_SOURCES
    "${RENDER_BENCH_DIR}/RenderBench.cpp"
    "${RENDER_BENCH_DIR}/ObjScene.cpp"
    "${RENDER_BENCH_DIR}/SunVisibility.cpp"
    "${RENDER_BENCH_DIR}/BenchStubs.cpp")
# The renderer's own sources, one path per line relative to Sources/AddOn, in Sources.txt beside
# the bench -- the closure of what DiligentScene links against, grown one unresolved symbol at a
# time (tools/RenderBench/grow_sources.py does it mechanically).
file (STRINGS "${RENDER_BENCH_DIR}/Sources.txt" RENDER_BENCH_LIST REGEX "^[^#].*\\.(cpp|c)$")
foreach (src ${RENDER_BENCH_LIST})
    list (APPEND RENDER_BENCH_SOURCES "${AddOnSourcesFolderAbsolute}/${src}")
endforeach ()
set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RENDER_BENCH_DIR}/Sources.txt")
list (REMOVE_DUPLICATES RENDER_BENCH_SOURCES)

add_executable (TapiocaRenderBench EXCLUDE_FROM_ALL ${RENDER_BENCH_SOURCES})
target_compile_features (TapiocaRenderBench PRIVATE cxx_std_20)
target_include_directories (TapiocaRenderBench PRIVATE ${AddOnSourcesFolder} "${RENDER_BENCH_DIR}")
target_include_directories (TapiocaRenderBench SYSTEM PRIVATE ${GLM_INC} ${NANORT_INC} ${EARSTRIP_INC}
    "${DILIGENT_DIR}/DiligentTools/ThirdParty/stb")
target_compile_definitions (TapiocaRenderBench PRIVATE NOMINMAX TAPIOCA_RENDER_BENCH=1)
target_link_libraries (TapiocaRenderBench PRIVATE ${DILIGENT_PROBE_LIBS} TapiocaClipper2 d3dcompiler dxguid gdi32 dcomp dbghelp)
# ⚠️ /FORCE:UNRESOLVED, AND WHY THAT IS SAFE HERE. DiligentScene owns layers for features the
# bench never feeds -- Grasshopper previews, diagrams, sun studies, point clouds -- and their deep
# dependencies (node graphs, massing, editors) would drag most of the add-on in. The layers
# themselves are compiled (Sources.txt); only that tail is left unresolved, and a call into it
# jumps to address zero -- a crash, never a wrong picture. LNK4006/4088 list what was forced.
if (MSVC)
    target_link_options (TapiocaRenderBench PRIVATE /FORCE:UNRESOLVED)
endif ()
if (MSVC)
    target_compile_options (TapiocaRenderBench PRIVATE /W3 /MP /EHsc /wd4996 -D_CRT_SECURE_NO_WARNINGS)
endif ()
foreach (cfg DEBUG RELEASE RELWITHDEBINFO)
    set_target_properties (TapiocaRenderBench PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/RenderBench")
endforeach ()
# The bundled DXC beside the executable: DiligentDeviceFactory finds both by the module's own path.
add_custom_command (TARGET TapiocaRenderBench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/dxcompiler.dll" "${CMAKE_BINARY_DIR}/dxil.dll"
            "${CMAKE_BINARY_DIR}/RenderBench/")
