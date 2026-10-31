cmake_minimum_required (VERSION 3.24)
project (TapiocaUIPlayground LANGUAGES C CXX)
if (NOT WIN32 OR NOT MSVC)
    message (FATAL_ERROR "UIPlayground requires Windows and MSVC, but no Archicad or DevKit.")
endif ()
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
get_filename_component (EVP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component (REF_DIR "${EVP_ROOT}/../reference" ABSOLUTE)
set (ADDON_DIR "${EVP_ROOT}/Sources/AddOn")
set (DILIGENT_DIR "${REF_DIR}/DiligentEngine-master")
set (IMGUI_DIR "${DILIGENT_DIR}/DiligentTools/ThirdParty/imgui")
if (NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message (FATAL_ERROR "Provision reference dependencies first: tools/build/provision-reference.ps1 -FromUpstream")
endif ()

# The renderer and the reloadable panels MUST use one GImGui, including internal API/data.
# Define this target before DiligentTools so its renderer never compiles a private ImGui.
add_library (imgui SHARED
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp" "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    ImGuiFiles.cpp)
set_target_properties (imgui PROPERTIES OUTPUT_NAME TapiocaImGui WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories (imgui SYSTEM PUBLIC "${IMGUI_DIR}"
    "${DILIGENT_DIR}/DiligentTools/Imgui/interface")
target_compile_definitions (imgui PUBLIC IMGUI_USER_CONFIG="ImGuiDiligentConfig.h" IMGUI_DEFINE_MATH_OPERATORS
    PRIVATE "IMGUI_API=__declspec(dllexport)" INTERFACE "IMGUI_API=__declspec(dllimport)")
target_link_libraries (imgui PRIVATE dwmapi gdi32)

foreach (option DILIGENT_NO_DIRECT3D12 DILIGENT_NO_VULKAN DILIGENT_NO_OPENGL DILIGENT_NO_WEBGPU
    DILIGENT_NO_ARCHIVER DILIGENT_NO_RENDER_STATE_PACKAGER)
    set (${option} ON CACHE BOOL "" FORCE)
endforeach ()
foreach (option DILIGENT_BUILD_TESTS DILIGENT_INSTALL_CORE DILIGENT_INSTALL_TOOLS DILIGENT_BUILD_FX
    DILIGENT_BUILD_SAMPLES DILIGENT_ENABLE_DRACO)
    set (${option} OFF CACHE BOOL "" FORCE)
endforeach ()
set (DILIGENT_MSVC_RELEASE_COMPILE_OPTIONS "" CACHE STRING "" FORCE)
if (POLICY CMP0177)
    set (CMAKE_POLICY_DEFAULT_CMP0177 NEW)
endif ()
# Diligent's backend detection uses try_compile, so ATL must also reach compiler checks.
file (GLOB atlHeaders "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/MSVC/*/atlmfc/include/atlbase.h")
if (NOT atlHeaders)
    message (FATAL_ERROR "Install the Visual Studio C++ ATL component (Diligent D3D11 requires it).")
endif ()
list (SORT atlHeaders COMPARE NATURAL ORDER DESCENDING)
list (GET atlHeaders 0 atlHeader)
get_filename_component (atlInclude "${atlHeader}" DIRECTORY)
string (APPEND CMAKE_CXX_FLAGS " /I\"${atlInclude}\"")
add_subdirectory ("${DILIGENT_DIR}/DiligentCore" "${CMAKE_BINARY_DIR}/DiligentCore" EXCLUDE_FROM_ALL)
add_subdirectory ("${DILIGENT_DIR}/DiligentTools" "${CMAKE_BINARY_DIR}/DiligentTools" EXCLUDE_FROM_ALL)
# Only the 2D renderer is needed. Upstream's optional Utils/gizmo extensions
# redeclare ImGui functions without dllimport and cannot join the shared ABI.
get_target_property (rendererSources Diligent-Imgui SOURCES)
list (FILTER rendererSources EXCLUDE REGEX "(ImGuiUtils|imGuIZMO)\\.cpp$")
set_property (TARGET Diligent-Imgui PROPERTY SOURCES "${rendererSources}")

include (ProductionSources.cmake)
list (REMOVE_ITEM PLAYGROUND_PRODUCTION_SOURCES "${ADDON_DIR}/ArchViz/MassingAreaCalculations.cpp")
add_library (PlaygroundModel STATIC Fixture.cpp "${ADDON_DIR}/NodeGraph/Json.cpp"
    "${ADDON_DIR}/ArchViz/MassingAreaCalculations.cpp")
target_include_directories (PlaygroundModel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${ADDON_DIR}")
target_link_libraries (PlaygroundModel PUBLIC imgui)
add_library (TapiocaUIPlaygroundUI SHARED UiModule.cpp Workspace.cpp ${PLAYGROUND_PRODUCTION_SOURCES})
target_link_libraries (TapiocaUIPlaygroundUI PRIVATE PlaygroundModel imgui)
target_compile_definitions (TapiocaUIPlaygroundUI PRIVATE TAPIOCA_UI_MODULE=1)

add_executable (TapiocaUIPlayground WIN32 Host.cpp Reload.cpp)
target_link_libraries (TapiocaUIPlayground PRIVATE PlaygroundModel imgui Diligent-Imgui
    Diligent-Common Diligent-GraphicsEngineD3D11-static Diligent-GraphicsTools d3d11 dxgi shell32)
target_compile_definitions (TapiocaUIPlayground PRIVATE
    TAPIOCA_PLAYGROUND_SOURCE=L"${CMAKE_CURRENT_SOURCE_DIR}"
    TAPIOCA_PLAYGROUND_BUILD=L"${CMAKE_BINARY_DIR}"
    TAPIOCA_CMAKE=L"${CMAKE_COMMAND}"
    TAPIOCA_FONT=L"${REF_DIR}/noto-sans-font-v2.015/hinted/ttf/NotoSans-Regular.ttf")
add_dependencies (TapiocaUIPlayground TapiocaUIPlaygroundUI)
foreach (target PlaygroundModel TapiocaUIPlaygroundUI TapiocaUIPlayground)
    target_compile_definitions (${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options (${target} PRIVATE /W4 /EHsc /utf-8 /Zc:__cplusplus)
endforeach ()

enable_testing ()
add_library (PlaygroundRejectedModule SHARED tests/RejectedModule.cpp)
target_include_directories (PlaygroundRejectedModule PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable (PlaygroundTests tests/PlaygroundTests.cpp Reload.cpp)
target_link_libraries (PlaygroundTests PRIVATE PlaygroundModel imgui)
target_compile_definitions (PlaygroundTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies (PlaygroundTests TapiocaUIPlaygroundUI PlaygroundRejectedModule)
add_test (NAME PlaygroundTests COMMAND PlaygroundTests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
