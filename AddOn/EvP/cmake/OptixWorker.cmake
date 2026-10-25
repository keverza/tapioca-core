# OptixWorker.cmake -- find the build-time CUDA toolkit and OptiX SDK, and turn
# OptixOcclusion.cu into an embedded OptiX-IR header.
#
# Both are OPTIONAL and build-time only (HANDOFF-SunStudyRust.md §7.1 G3): end
# users need an NVIDIA driver, nothing else. Without them TapiocaOptixWorker is
# simply not built and the CPU and DXR paths are untouched.
#
# ⚠️ NOTHING GENERATED FROM THE SDK IS COMMITTED (decision 8). The .optixir and
# its header are produced in the build tree from our own .cu on every build.
#
#   tapioca_find_optix ()               sets TAPIOCA_OPTIX_FOUND, TAPIOCA_OPTIX_INCLUDE
#   tapioca_add_optix_ir (target root)  adds the generated header to `target`
#
# Override the SDK with -DTAPIOCA_OPTIX_SDK=<dir> or the OptiX_INSTALL_DIR
# environment variable; CUDA is found from CUDA_PATH (find_package(CUDAToolkit)).
#
# ⚠️ THE IR COMPILER DECIDES THE OLDEST DRIVER THAT CAN RUN THE WORKER. The
# driver's OptiX compiles the embedded OptiX-IR and refuses NVVM IR newer than
# it knows ("minor NvvmIRVersion newer than tool"). Measured 2026-10-25 on an
# RTX 4070 Ti: CUDA 13.4 IR fails on driver 610.88 (driver CUDA level 13.3)
# and loads on 621.01 (13.5); CUDA 11.8 IR loads on both. The rule: the IR
# toolkit's CUDA level must not exceed the driver's. -DTAPIOCA_OPTIX_NVCC=<nvcc.exe>
# picks the IR compiler separately from the toolkit whose headers and driver
# API library the host code uses; the oldest toolkit that compiles the program
# gives the widest driver range.

set (_TAPIOCA_OPTIX_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function (tapioca_find_optix)
    set (TAPIOCA_OPTIX_FOUND FALSE PARENT_SCOPE)
    if (NOT WIN32)
        return ()
    endif ()
    find_package (CUDAToolkit QUIET)
    if (NOT CUDAToolkit_FOUND OR NOT CUDAToolkit_NVCC_EXECUTABLE)
        message (STATUS "OptiX worker: no CUDA toolkit found; TapiocaOptixWorker will not be built")
        return ()
    endif ()
    set (sdk "${TAPIOCA_OPTIX_SDK}")
    if (NOT sdk AND DEFINED ENV{OptiX_INSTALL_DIR})
        set (sdk "$ENV{OptiX_INSTALL_DIR}")
    endif ()
    if (NOT sdk)
        file (GLOB candidates "C:/ProgramData/NVIDIA Corporation/OptiX SDK *")
        list (SORT candidates COMPARE NATURAL ORDER DESCENDING)
        list (LENGTH candidates count)
        if (count GREATER 0)
            list (GET candidates 0 sdk)
        endif ()
    endif ()
    if (NOT sdk OR NOT EXISTS "${sdk}/include/optix.h")
        message (STATUS "OptiX worker: no OptiX SDK found; TapiocaOptixWorker will not be built")
        return ()
    endif ()
    set (TAPIOCA_OPTIX_SDK "${sdk}" CACHE PATH "OptiX SDK root (build-time only)")
    # Default: the OLDEST installed toolkit that can emit OptiX-IR (11.7+).
    set (ir_nvcc "${CUDAToolkit_NVCC_EXECUTABLE}")
    file (GLOB toolkits "$ENV{ProgramFiles}/NVIDIA GPU Computing Toolkit/CUDA/v*")
    list (SORT toolkits COMPARE NATURAL)
    foreach (toolkit IN LISTS toolkits)
        string (REGEX MATCH "v([0-9]+\.[0-9]+)$" _ "${toolkit}")
        if (CMAKE_MATCH_1 AND NOT CMAKE_MATCH_1 VERSION_LESS 11.7 AND EXISTS "${toolkit}/bin/nvcc.exe")
            set (ir_nvcc "${toolkit}/bin/nvcc.exe")
            break ()
        endif ()
    endforeach ()
    set (TAPIOCA_OPTIX_NVCC "${ir_nvcc}" CACHE FILEPATH
         "nvcc that compiles the OptiX-IR; its age sets the minimum driver")
    set (TAPIOCA_OPTIX_FOUND TRUE PARENT_SCOPE)
    set (TAPIOCA_OPTIX_INCLUDE "${sdk}/include" PARENT_SCOPE)
    message (STATUS "OptiX worker: CUDA ${CUDAToolkit_VERSION}, OptiX SDK ${sdk}")
endfunction ()

# `sources_root` is the folder holding OptixWorker/ (the .cu includes
# "OptixWorker/OptixLaunchParams.h").
function (tapioca_add_optix_ir target sources_root)
    set (cu "${sources_root}/OptixWorker/OptixOcclusion.cu")
    set (out_dir "${CMAKE_CURRENT_BINARY_DIR}/optix_generated")
    set (ir "${out_dir}/OptixOcclusion.optixir")
    set (header "${out_dir}/OptixOcclusionIr.h")
    get_filename_component (host_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set (extra)
    # MSVC 14.4x's STL refuses CUDA older than 12.4; the device program uses
    # no STL, so an older toolkit is told to proceed.
    execute_process (COMMAND "${TAPIOCA_OPTIX_NVCC}" --version OUTPUT_VARIABLE nvcc_banner)
    string (REGEX MATCH "release ([0-9]+\.[0-9]+)" _ "${nvcc_banner}")
    set (nvcc_version "${CMAKE_MATCH_1}")
    if (nvcc_version VERSION_LESS 12.4)
        list (APPEND extra -allow-unsupported-compiler -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    endif ()
    message (STATUS "OptiX worker: OptiX-IR compiled by CUDA ${nvcc_version} (${TAPIOCA_OPTIX_NVCC})")
    add_custom_command (
        OUTPUT "${header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND "${TAPIOCA_OPTIX_NVCC}" -optix-ir -std=c++17 -arch=compute_75 ${extra}
            -ccbin "${host_dir}" -I "${TAPIOCA_OPTIX_INCLUDE}" -I "${sources_root}" -o "${ir}" "${cu}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${ir} -DOUTPUT=${header} -DNAME=kOptixOcclusionIr
            -P "${_TAPIOCA_OPTIX_MODULE_DIR}/EmbedBinary.cmake"
        DEPENDS "${cu}" "${sources_root}/OptixWorker/OptixLaunchParams.h"
        COMMENT "OptiX worker: compiling OptixOcclusion.cu to OptiX-IR"
        VERBATIM)
    target_sources (${target} PRIVATE "${header}")
    target_include_directories (${target} PRIVATE "${out_dir}" "${TAPIOCA_OPTIX_INCLUDE}")
    target_link_libraries (${target} PRIVATE CUDA::cuda_driver)
endfunction ()
