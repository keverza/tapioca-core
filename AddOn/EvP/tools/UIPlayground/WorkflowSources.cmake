# Reuse the offline project's DevKit-free editor kernel, never duplicate its UI.
file (READ "${EVP_ROOT}/tests/cpp/CMakeLists.txt" offline)
string (REPLACE "\$\{GEOM_DIR\}" "${ADDON_DIR}/Geometry" offline "${offline}")
string (REGEX MATCH "set \\(EVP_EDIT_SOURCES[^)]*\\)" editorBlock "${offline}")
string (REGEX MATCHALL "\"[^\"]+\\.cpp\"" editorPaths "${editorBlock}")
foreach (source IN LISTS editorPaths)
    string (REPLACE "\"" "" source "${source}")
    string (REPLACE "\$\{EVP_ROOT\}" "${EVP_ROOT}" source "${source}")
    list (APPEND workflowSources "${source}")
endforeach ()
file (GLOB editorHud CONFIGURE_DEPENDS "${ADDON_DIR}/ArchViz/HudVirtualEditor*.cpp")
# Debug export is a host service: the mock never writes an external dump or starts a worker.
list (FILTER editorHud EXCLUDE REGEX "HudVirtualEditorDebug\\.cpp$")
list (APPEND workflowSources ${editorHud})
foreach (source
    ArchViz/DiligentHudAnalysisPage.cpp ArchViz/DiligentHudSunStudy.cpp ArchViz/DiligentHudSunRun.cpp
    ArchViz/DiligentHudSunControls.cpp ArchViz/DiligentHudVisibility.cpp ArchViz/DiligentHudViewpoint.cpp
    ArchViz/DiligentHudControls.cpp ArchViz/AnalysisTabs.cpp
    ArchViz/AnalysisPath.cpp ArchViz/AnalysisProfile.cpp ArchViz/EditLayers.cpp
    ArchViz/EditLayersPlacement.cpp ArchViz/EditLandscapeLayers.cpp ArchViz/RoofSlopeCanvas.cpp
    ArchViz/AnalysisLiveRerun.cpp
    ArchViz/MatrixMath.cpp ArchViz/PointGumball.cpp ArchViz/HudGumball.cpp
    Geometry/Fillet2D.cpp Geometry/GeometryEngine.cpp Geometry/SurfaceCriteria.cpp Geometry/PlanarTriangulation.cpp
    Geometry/QueryEngine.cpp
    SunStudy/SunStudyBackend.cpp SunStudy/SunStudyBounds.cpp SunStudy/SunStudyPreset.cpp
    SunStudy/SunStudyStepAtlas.cpp SunStudy/SunStudyRoles.cpp SunStudy/SunStudyLimits.cpp SunStudy/ViewpointStudy.cpp)
    list (APPEND workflowSources "${ADDON_DIR}/${source}")
endforeach ()
foreach (source IN LISTS workflowSources)
    if (NOT EXISTS "${source}")
        message (FATAL_ERROR "Missing workflow production source: ${source}")
    endif ()
    file (RELATIVE_PATH relative "${EVP_ROOT}" "${source}")
    string (FIND "${offline}" "/${relative}\"" covered)
    if (covered EQUAL -1 AND NOT relative MATCHES "Sources/AddOn/ArchViz/DiligentHud(AnalysisPage|SunStudy|SunRun|Visibility)\\.cpp$")
        message (FATAL_ERROR "Workflow source has neither offline nor explicit playground smoke coverage: ${relative}")
    endif ()
endforeach ()
set (clipper "${REF_DIR}/Clipper2-main/CPP/Clipper2Lib")
add_library (PlaygroundClipper STATIC "${clipper}/src/clipper.engine.cpp" "${clipper}/src/clipper.offset.cpp"
    "${clipper}/src/clipper.rectclip.cpp" "${clipper}/src/clipper.triangulation.cpp")
target_include_directories (PlaygroundClipper SYSTEM PUBLIC "${clipper}/include")
target_compile_definitions (PlaygroundClipper PUBLIC CLIPPER2_MAX_DECIMAL_PRECISION=8)
add_library (PlaygroundWorkflows STATIC ${workflowSources})
target_include_directories (PlaygroundWorkflows PRIVATE "${ADDON_DIR}" "${ADDON_DIR}/Geometry")
target_include_directories (PlaygroundWorkflows SYSTEM PRIVATE "${REF_DIR}/glm-master" "${REF_DIR}/nanort-release"
    "${REF_DIR}/earstrip-main")
target_link_libraries (PlaygroundWorkflows PUBLIC imgui PlaygroundClipper)
target_compile_options (PlaygroundWorkflows PRIVATE /Gy /EHsc /utf-8 /Zc:__cplusplus /W0)
target_compile_definitions (PlaygroundWorkflows PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries (TapiocaUIPlaygroundUI PRIVATE PlaygroundWorkflows)
target_link_options (TapiocaUIPlaygroundUI PRIVATE /OPT:REF /INCREMENTAL:NO)
