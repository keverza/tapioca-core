# Compile the scaffold only. Linking and a C ABI require a separate feature task.
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
    # Always run Cargo: Cargo owns source/manifests/config/lockfile invalidation.
    # This also avoids stale archives with Visual Studio's multi-config generator.
    add_custom_target (TapiocaRustKernel ALL
        COMMAND "${Python3_EXECUTABLE}" "${_repo}/tools/quality/check_rust.py"
            --target x86_64-pc-windows-msvc
            --target-dir "${CMAKE_BINARY_DIR}/rust"
        WORKING_DIRECTORY "${_evp}/rust"
        COMMENT "Tapioca: checking and compiling the unlinked Rust kernel scaffold"
        VERBATIM USES_TERMINAL)
endfunction ()
