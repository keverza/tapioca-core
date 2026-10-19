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
