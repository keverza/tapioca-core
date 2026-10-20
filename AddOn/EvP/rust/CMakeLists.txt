cmake_minimum_required (VERSION 3.16)
set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
project (TapiocaRustFoundation C CXX)

# Standalone foundation checks and mixed C/C++ ABI link, no SDK or renderer.
include ("${CMAKE_CURRENT_LIST_DIR}/../cmake/RustKernel.cmake")
TapiocaAddRustKernel ()

add_executable (TapiocaRustAbiSmoke tests/abi_smoke.c tests/abi_header.cpp)
set_target_properties (TapiocaRustAbiSmoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustAbiSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry")
TapiocaLinkRustCore (TapiocaRustAbiSmoke)
enable_testing ()
add_test (NAME TapiocaRustAbiSmoke COMMAND TapiocaRustAbiSmoke)

# Explicit test-only ABI stubs exercise wrapper refusal paths without shipping
# a panic/version-override export or linking a second production authority.
add_executable (TapiocaRustWrapperFailure tests/abi_wrapper_failure.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustTerrain.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustIdentity.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustValue.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustAffine.cpp")
set_target_properties (TapiocaRustWrapperFailure PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustWrapperFailure PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry")
add_test (NAME TapiocaRustWrapperFailure COMMAND TapiocaRustWrapperFailure)

# Real owned-C++ -> borrowed-POD -> Rust identity validation, without the SDK.
add_executable (TapiocaRustIdentitySmoke tests/identity_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustTerrain.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustIdentity.cpp")
set_target_properties (TapiocaRustIdentitySmoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustIdentitySmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry")
TapiocaLinkRustCore (TapiocaRustIdentitySmoke)
add_test (NAME TapiocaRustIdentitySmoke COMMAND TapiocaRustIdentitySmoke)

add_executable (TapiocaRustValueSmoke tests/value_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustValue.cpp")
set_target_properties (TapiocaRustValueSmoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustValueSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
TapiocaLinkRustCore (TapiocaRustValueSmoke)
add_test (NAME TapiocaRustValueSmoke COMMAND TapiocaRustValueSmoke)

add_executable (TapiocaRustAffineSmoke tests/affine_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustAffine.cpp")
set_target_properties (TapiocaRustAffineSmoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustAffineSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
TapiocaLinkRustCore (TapiocaRustAffineSmoke)
add_test (NAME TapiocaRustAffineSmoke COMMAND TapiocaRustAffineSmoke)

add_executable (TapiocaRustPolylineSmoke tests/polyline_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustPolyline.cpp")
set_target_properties (TapiocaRustPolylineSmoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustPolylineSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
TapiocaLinkRustCore (TapiocaRustPolylineSmoke)
add_test (NAME TapiocaRustPolylineSmoke COMMAND TapiocaRustPolylineSmoke)

add_executable (TapiocaRustPolylineFailure tests/polyline_wrapper_failure.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustPolyline.cpp")
set_target_properties (TapiocaRustPolylineFailure PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustPolylineFailure PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
add_test (NAME TapiocaRustPolylineFailure COMMAND TapiocaRustPolylineFailure)

add_executable (TapiocaRustArcSmoke tests/arc_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustArc.cpp")
set_target_properties (TapiocaRustArcSmoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustArcSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
TapiocaLinkRustCore (TapiocaRustArcSmoke)
add_test (NAME TapiocaRustArcSmoke COMMAND TapiocaRustArcSmoke)

add_executable (TapiocaRustArcFailure tests/arc_wrapper_failure.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustArc.cpp")
set_target_properties (TapiocaRustArcFailure PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustArcFailure PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
add_test (NAME TapiocaRustArcFailure COMMAND TapiocaRustArcFailure)

add_executable (TapiocaRustPlaneSmoke tests/plane_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustPlane.cpp")
set_target_properties (TapiocaRustPlaneSmoke PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustPlaneSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
TapiocaLinkRustCore (TapiocaRustPlaneSmoke)
add_test (NAME TapiocaRustPlaneSmoke COMMAND TapiocaRustPlaneSmoke)

add_executable (TapiocaRustPlaneFailure tests/plane_wrapper_failure.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustPlane.cpp")
set_target_properties (TapiocaRustPlaneFailure PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustPlaneFailure PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
add_test (NAME TapiocaRustPlaneFailure COMMAND TapiocaRustPlaneFailure)

add_executable (TapiocaRustMeshPrepSmoke tests/mesh_prep_smoke.c tests/mesh_prep_smoke.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustMeshPrep.cpp")
set_target_properties (TapiocaRustMeshPrepSmoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustMeshPrepSmoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn"
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry")
TapiocaLinkRustCore (TapiocaRustMeshPrepSmoke)
add_test (NAME TapiocaRustMeshPrepSmoke COMMAND TapiocaRustMeshPrepSmoke)

add_executable (TapiocaRustMeshPrepFailure tests/mesh_prep_wrapper_failure.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn/Geometry/GeometryRustMeshPrep.cpp")
set_target_properties (TapiocaRustMeshPrepFailure PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories (TapiocaRustMeshPrepFailure PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Sources/AddOn")
add_test (NAME TapiocaRustMeshPrepFailure COMMAND TapiocaRustMeshPrepFailure)
