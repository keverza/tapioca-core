# Rust geometry kernel: environment and compilation rules

**Status:** build-only scaffold, 2026-10-17. No migration/refactor is implemented.
The C++ engines remain the only production implementations. No Rust code is linked
into `EvPAddOn`/`Tapioca.apx`, and no C ABI functions are exposed.

## Location and boundaries

`AddOn/EvP/rust` is a standalone Cargo workspace outside `Sources/AddOn`, so the
existing C++ tier scanner does not mistake Rust modules for C++ feature folders.
Its equivalent service boundary is enforced by `tools/quality/check_rust.py`:

```text
future C++ adapter (not created)
    -> tapioca-geometry-ffi (staticlib, unsafe denied by default)
        -> tapioca-geometry (safe library, unsafe forbidden)
```

Only the local FFI-to-geometry dependency is approved; no third-party/test/build
crates or build scripts. Rust has no SDK/rendering/platform dependency. New
dependencies require a policy change with necessity, version, license, source and
offline-provisioning justification. Cargo.lock is tracked. Source modules are
added only when an actual operation is implemented; no speculative module tree.

Read `AddOn/EvP/rust/AGENTS.md` for ownership, units, tolerances, deterministic
outputs, geometry ingress validation, FFI, and testing requirements. Those semantic
contracts are reviewed/tested per algorithm, not purportedly proven by Clippy.

## Machine setup (Windows x64)

Use the add-on's existing MSVC v143 x64 toolset and Windows SDK. Rust does not
replace or upgrade them. From `core/`:

```powershell
powershell -NoProfile -File tools/build/Setup-Rust.ps1 -InstallRustup
python tools/quality/check_rust.py
```

The setup script downloads official rustup only with explicit `-InstallRustup`,
verifies the published SHA-256, installs pinned Rust **1.99.1**, rustfmt, Clippy,
and `x86_64-pc-windows-msvc`. It leaves the user PATH/global default unchanged.
Run without that switch when rustup is already present. The checker locates rustup
on PATH or under CARGO_HOME/`~/.cargo`, then runs the exact pinned toolchain with
the workspace as cwd (Cargo config lookup is cwd-sensitive).

Only initial toolchain installation needs the network. All normal Cargo gates use
`--locked --offline`; the initial workspace has no downloaded crate dependencies.
Inherited RUSTFLAGS, CARGO_ENCODED_RUSTFLAGS, RUSTUP_TOOLCHAIN and RUSTC_BOOTSTRAP
are rejected by the gate so they cannot silently change compilation policy.

The MSVC target explicitly uses dynamic CRT (`-crt-static`) to match
`CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL` (`/MD`, including add-on Debug).
No `target-cpu=native`, fast-math, mixed-language LTO, or panic=abort. Release keeps
debug information; final mixed C++/Rust PDB stepping and the `.apx` link are a
future FFI acceptance test, not claimed by this scaffold build.

## Daily gates

```powershell
python tools/quality/check_rust.py
python -m pytest tools/quality/tests/test_check_rust.py tools/quality/tests/test_check_secrets.py -q
```

The checker runs, in order:

1. Workspace/dependency/unsafe-default checks, pre-staging Rust secret/provider scan.
2. `cargo fmt --all -- --check`.
3. `cargo check --workspace --all-targets --locked --offline`.
4. `cargo clippy --workspace --all-targets --locked --offline -- -D warnings`.
5. Advisory Clippy `--force-warn` pass for function size (>100), complexity (>25),
   nesting (>4); these warnings remain visible without becoming hard failures.
6. `cargo test --workspace --locked --offline`, also in release (includes doctests).
7. Release `tapioca-geometry-ffi` staticlib compilation.

Cargo.toml selects pedantic plus panic/unwrap/expect/index/float/clone safety lints;
clippy.toml configures argument threshold 5. Narrow `#[expect(..., reason = "...")]`
is preferred over blanket suppression; review any production exception. File
length >600 warns; >1000 requests cohesion review but does not fail. `impl` >400,
public API need, hot clones, and module cycles are human reviews. Do not claim a
regex scanner proves Rust lifetimes or geometry invariants.

`--structure-only` runs Python checks without requiring/installing Rust; missing
Rust on the full gate is an error, never a green skip. `--target
x86_64-pc-windows-msvc` makes the target explicit; otherwise native-host tests run,
including Linux. `--target-dir <path>` redirects outputs. The initial crates have
zero algorithm tests; a green run proves tooling only, not geometry correctness.

## CMake / Visual Studio (opt-in, compile only)

The standalone CMake entry needs no DevKit or Diligent:

```powershell
cmake -S AddOn/EvP/rust -B AddOn/EvP/rust/build
cmake --build AddOn/EvP/rust/build --target TapiocaRustKernel --config RelWithDebInfo
```

It invokes the same full gate for x64 MSVC. Archives are under
`<binary-dir>/rust/x86_64-pc-windows-msvc/release/`.

An ordinary `.apx` build does **not** require Rust. After native configuration,
optionally expose the same compile/check target in its solution:

```powershell
cmake -S AddOn/EvP -B AddOn/EvP/build_29 -DTAPIOCA_BUILD_RUST_KERNEL=ON
cmake --build AddOn/EvP/build_29 --target TapiocaRustKernel --config RelWithDebInfo
```

Reset with `-DTAPIOCA_BUILD_RUST_KERNEL=OFF`. ON adds a standalone ALL target, not a
link dependency of EvPAddOn; building the specific EvPAddOn target does not invoke
it. `Build-AddOn29.ps1` also runs cheap Rust structure checks in its usual quality
gate, without requiring rustup. No changes to the offline C++ suite or its user
work are needed. Do not build/deploy the full add-on while Archicad is running.

## CI and future kernel acceptance

`rust-kernel.yml` runs the same gate on Windows x64 MSVC and Linux, with Python
checker regressions, no SDK and no Archicad. Versions come from the checked-in
toolchain file rather than a floating stable action. It creates no release assets.

Before the first real algorithm: approve a coarse interface and ownership model;
add C++ parity fixtures, seeded invariant/property checks, degeneracy/large-world
cases, deterministic canonicalization and a time/allocation/memory baseline. A
future exported operation additionally needs layout/lifetime tests, malformed
input rejection, panic containment, cancellation/revision tests, and an actual
MSVC `.apx` link/debug/rollback drill. No scaffold rule authorizes changing existing
outputs or replacing SDK/rendering code.
