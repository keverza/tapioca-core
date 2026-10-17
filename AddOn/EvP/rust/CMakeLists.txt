cmake_minimum_required (VERSION 3.16)
project (TapiocaRustScaffold NONE)

# A standalone compile-only entry point, requiring no Archicad SDK or renderer.
include ("${CMAKE_CURRENT_LIST_DIR}/../cmake/RustKernel.cmake")
TapiocaAddRustKernel ()
