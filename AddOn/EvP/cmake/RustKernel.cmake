# Optional Rust gate plus the aggregate archive link helper. No geometry consumer
# or add-on link is enabled merely by including this file.
set (TAPIOCA_RUST_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
function (TapiocaAddRustKernel)
    if (TARGET TapiocaRustKernel)
        return ()
    endif ()
    find_package (Python3 3.11 REQUIRED COMPONENTS Interpreter)
    find_program (TAPIOCA_RUSTUP NAMES rustup rustup.exe
        HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
    if (NOT TAPIOCA_RUSTUP)
        message (FATAL_ERROR "Rust checks requested, but rustup is missing. Run tools/build/Setup-Rust.ps1 -InstallRustup.")
    endif ()

    get_filename_component (_evp "${TAPIOCA_RUST_CMAKE_DIR}/.." ABSOLUTE)
    set (_repo "${_evp}/../..")
    set (_targetArgs)
    if (WIN32)
        set (_targetArgs --target x86_64-pc-windows-msvc)
    endif ()
    # Always run Cargo: Cargo owns source/manifests/config/lockfile invalidation.
    # This also avoids stale archives with Visual Studio's multi-config generator.
    add_custom_target (TapiocaRustKernel ALL
        COMMAND "${Python3_EXECUTABLE}" "${_repo}/tools/quality/check_rust.py"
            ${_targetArgs}
            --target-dir "${CMAKE_BINARY_DIR}/rust"
        WORKING_DIRECTORY "${_evp}/rust"
        COMMENT "Tapioca: checking and compiling the Rust foundation"
        VERBATIM USES_TERMINAL)
endfunction ()

# Link the same release archive in every C++ configuration, using dynamic CRT
# on MSVC (/MD). Existing add-on link calls use CMake's plain signature; pass
# PLAIN there rather than mixing plain and keyword signatures on one target.
function (TapiocaLinkRustCore target)
    if (NOT TARGET TapiocaCoreFfi)
        find_package (Python3 3.11 REQUIRED COMPONENTS Interpreter)
        find_program (TAPIOCA_RUSTUP NAMES rustup rustup.exe
            HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
        if (NOT TAPIOCA_RUSTUP)
            message (FATAL_ERROR "Rust archive requested, but rustup is missing. Run tools/build/Setup-Rust.ps1 -InstallRustup.")
        endif ()
        get_filename_component (_component "${TAPIOCA_RUST_CMAKE_DIR}/.." ABSOLUTE)
        get_filename_component (_repo "${_component}/../.." ABSOLUTE)
        set (_workspace "${_component}/rust")
        set (_targetArgs)
        if (WIN32)
            if (NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
                NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
                message (FATAL_ERROR "The Tapioca Rust archive requires the x64 MSVC C/C++ ABI (not ARM64 or x86).")
            endif ()
            set (_targetArgs --target x86_64-pc-windows-msvc)
            set (_archive "${CMAKE_BINARY_DIR}/rust/x86_64-pc-windows-msvc/release/tapioca_core_ffi.lib")
        else ()
            set (_archive "${CMAKE_BINARY_DIR}/rust/release/libtapioca_core_ffi.a")
        endif ()
        file (GLOB_RECURSE _rustSources CONFIGURE_DEPENDS
            "${_workspace}/geometry/src/*.rs" "${_workspace}/ffi/src/*.rs")
        # Cargo owns incremental rebuilds; always invoke it to cover manifests,
        # config and checker policy as well as CMake's explicitly tracked sources.
        add_custom_target (TapiocaRustCoreBuild
            COMMAND "${Python3_EXECUTABLE}" "${_repo}/tools/quality/check_rust.py"
                --build ${_targetArgs} --target-dir "${CMAKE_BINARY_DIR}/rust"
            DEPENDS ${_rustSources} "${_workspace}/Cargo.toml" "${_workspace}/Cargo.lock"
                "${_workspace}/rust-toolchain.toml" "${_workspace}/.cargo/config.toml"
                "${_workspace}/geometry/Cargo.toml" "${_workspace}/ffi/Cargo.toml"
                "${_repo}/tools/quality/check_rust.py"
            BYPRODUCTS "${_archive}"
            WORKING_DIRECTORY "${_workspace}"
            COMMENT "Tapioca: building the aggregate release Rust archive"
            VERBATIM USES_TERMINAL)
        add_library (TapiocaCoreFfi STATIC IMPORTED GLOBAL)
        set_target_properties (TapiocaCoreFfi PROPERTIES IMPORTED_LOCATION "${_archive}")
        add_dependencies (TapiocaCoreFfi TapiocaRustCoreBuild)
        if (WIN32)
            set_property (TARGET TapiocaCoreFfi PROPERTY INTERFACE_LINK_LIBRARIES
                "advapi32;bcrypt;ntdll;userenv;ws2_32")
        elseif (UNIX)
            find_package (Threads REQUIRED)
            set_property (TARGET TapiocaCoreFfi PROPERTY INTERFACE_LINK_LIBRARIES
                "Threads::Threads;${CMAKE_DL_LIBS};m")
            if (NOT APPLE)
                set_property (TARGET TapiocaCoreFfi APPEND PROPERTY INTERFACE_LINK_LIBRARIES "rt;util")
            endif ()
        endif ()
    endif ()
    if (ARGC GREATER 1 AND ARGV1 STREQUAL "PLAIN")
        target_link_libraries (${target} TapiocaCoreFfi)
    else ()
        target_link_libraries (${target} PRIVATE TapiocaCoreFfi)
    endif ()
endfunction ()
