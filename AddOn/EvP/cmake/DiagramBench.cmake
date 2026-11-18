# TapiocaDiagramBench: the viewer's exploded massing diagram as a standalone executable (#201,
# tools/DiagramBench). It replays a diagram capture and renders it through DiligentScene's own
# DrawDiagram, so a spacing or a style is checked without Archicad.
#
# ⚠️ EXCLUDE_FROM_ALL: the add-on build never builds it. Build it on purpose:
#   cmake --build build_29 --config RelWithDebInfo --target TapiocaDiagramBench
# It reuses RenderBench's renderer closure (tools/RenderBench/Sources.txt and BenchStubs.cpp) and
# adds the diagram's pure sources: replay, snapshot, publication, layout and the building groups.
#
# ⚠️ NO DEVKIT, and /FORCE:UNRESOLVED for the same reason as RenderBench: the scene owns layers the
# bench never feeds. A call into that tail jumps to address zero -- a crash, never a wrong picture.

set (DIAGRAM_BENCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools/DiagramBench")
set (DIAGRAM_BENCH_RENDER_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools/RenderBench")
set (DIAGRAM_BENCH_SOURCES
    "${DIAGRAM_BENCH_DIR}/DiagramBench.cpp"
    "${DIAGRAM_BENCH_RENDER_DIR}/BenchStubs.cpp"
    "${AddOnSourcesFolderAbsolute}/ArchViz/DiagramLayout.cpp"
    "${AddOnSourcesFolderAbsolute}/ArchViz/DiagramSnapshot.cpp"
    "${AddOnSourcesFolderAbsolute}/ArchViz/DiagramReplay.cpp"
    "${AddOnSourcesFolderAbsolute}/ArchViz/DiagramPublication.cpp"
    "${AddOnSourcesFolderAbsolute}/ArchViz/MassingBuildings.cpp"
    "${AddOnSourcesFolderAbsolute}/ArchViz/MassingMetrics.cpp"
    "${AddOnSourcesFolderAbsolute}/Geometry/PlanarTriangulation.cpp"
    "${AddOnSourcesFolderAbsolute}/NodeGraph/Json.cpp")
file (STRINGS "${DIAGRAM_BENCH_RENDER_DIR}/Sources.txt" DIAGRAM_BENCH_LIST REGEX "^[^#].*\\.(cpp|c)$")
foreach (src ${DIAGRAM_BENCH_LIST})
    list (APPEND DIAGRAM_BENCH_SOURCES "${AddOnSourcesFolderAbsolute}/${src}")
endforeach ()
list (REMOVE_DUPLICATES DIAGRAM_BENCH_SOURCES)

add_executable (TapiocaDiagramBench EXCLUDE_FROM_ALL ${DIAGRAM_BENCH_SOURCES})
target_compile_features (TapiocaDiagramBench PRIVATE cxx_std_20)
target_include_directories (TapiocaDiagramBench PRIVATE ${AddOnSourcesFolder} "${DIAGRAM_BENCH_DIR}")
target_include_directories (TapiocaDiagramBench SYSTEM PRIVATE ${GLM_INC} ${NANORT_INC} ${EARSTRIP_INC}
    "${DILIGENT_DIR}/DiligentTools/ThirdParty/stb")
target_compile_definitions (TapiocaDiagramBench PRIVATE NOMINMAX TAPIOCA_RENDER_BENCH=1)
target_link_libraries (TapiocaDiagramBench PRIVATE ${DILIGENT_PROBE_LIBS} TapiocaClipper2 d3dcompiler dxguid gdi32 dcomp
    dbghelp)
if (MSVC)
    target_link_options (TapiocaDiagramBench PRIVATE /FORCE:UNRESOLVED)
    target_compile_options (TapiocaDiagramBench PRIVATE /W3 /MP /EHsc /wd4996 -D_CRT_SECURE_NO_WARNINGS)
endif ()
foreach (cfg DEBUG RELEASE RELWITHDEBINFO)
    set_target_properties (TapiocaDiagramBench PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg}
        "${CMAKE_BINARY_DIR}/DiagramBench")
endforeach ()
add_custom_command (TARGET TapiocaDiagramBench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/dxcompiler.dll" "${CMAKE_BINARY_DIR}/dxil.dll"
            "${CMAKE_BINARY_DIR}/DiagramBench/")
