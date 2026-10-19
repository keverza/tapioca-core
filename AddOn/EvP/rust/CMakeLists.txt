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
