# Rust geometry kernel: environment and compilation rules

**Status:** S1 legacy-ingress increment, 2026-10-19. `tapioca-core-ffi` exports
`tapioca_version`, `tapioca_validate_mesh` and `tapioca_validate_snapshot` (ABI v1).
The C++ services wrapper and add-on/offline-suite archive link are enabled by
default. Startup reads/logs the ABI before callbacks/workers arm and refuses an
unsupported ABI; explicit CMake OFF is the legacy rollback path. Attributed
identity views/round-trip remain an incomplete S1 gate. C++ engines remain the
production geometry authorities; no algorithm or consumer flag is switched.

## Location and boundaries

`AddOn/EvP/rust` is a standalone Cargo workspace outside `Sources/AddOn`, so the
existing C++ tier scanner does not mistake Rust modules for C++ feature folders.
Its equivalent service boundary is enforced by `tools/quality/check_rust.py`:

```text
Geometry/GeometryRustTerrain.hpp (tapioca::geometry services wrapper)
    -> tapioca-core-ffi (aggregate staticlib, unsafe denied by default)
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
debug information. Actual `.apx` build/load evidence is recorded in the owning
stage; a standalone link alone is not native-load or mixed-language PDB evidence.

## Daily gates

```powershell
python tools/quality/check_rust.py
python tools/quality/check_rust.py --build
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
7. Release `tapioca-core-ffi` staticlib compilation.

`--build` is the CMake path: it retains architecture/secrets checks, the exact
toolchain pin and environment-override refusal, then runs only the locked/offline
release aggregate build. It does not run fmt/Clippy/tests and does not replace the
full gate. It is mutually exclusive with `--structure-only`.

Cargo.toml selects pedantic plus panic/unwrap/expect/index/float/clone safety lints;
clippy.toml configures argument threshold 5. Narrow `#[expect(..., reason = "...")]`
is preferred over blanket suppression; review any production exception. File
length >600 warns; >1000 requests cohesion review but does not fail. `impl` >400,
public API need, hot clones, and module cycles are human reviews. Do not claim a
regex scanner proves Rust lifetimes or geometry invariants.

`--structure-only` runs Python checks without requiring/installing Rust; missing
Rust on the full gate is an error, never a green skip. `--target
x86_64-pc-windows-msvc` makes the target explicit; otherwise native-host tests run,
including Linux. `--target-dir <path>` redirects outputs. Tests cover generic
mesh shape/finite/index/sidecar ingress plus the version/status/error boundary.
They do not prove terrain topology, authored identity or catalog semantics.

## C ABI envelope and legacy ingress

`Sources/AddOn/Geometry/GeometryRustFfi.h` is valid C and C++. Status is a fixed
`int32_t`: `OK=0`, `ERR_ARG=1`, `ERR_STATE=2`, `ERR_INTERNAL=3`. `tapioca_version`
returns status and writes ABI version 1 through an output pointer. Every export
uses `catch_unwind`; this does not recover OOM/abort, aborting panic hooks or
invalid foreign memory. Caught panic payloads are deliberately leaked so their
destructors cannot unwind through C. No test-only panic export is shipped.

Buffers are caller-owned, exclusively writable, nonoverlapping and call-scoped.
Nonempty buffers require valid aligned allocations; arithmetic/null/alignment
checks cannot establish memory validity. Per-call error text is UTF-8, NUL
terminated (when capacity is nonzero), truncated only at a character boundary,
and cleared on success. Null with zero capacity discards text, never the status.
Malformed error storage returns `ERR_ARG` without touching it; detected overlap
writes neither buffer. A failed call leaves its version output unchanged. The
version read is pure synchronous constant work; this is not a terrain benchmark.

The legacy mesh/snapshot validators use length-delimited UTF-8 and pointer/count
views; counts are scalar elements, not xyz tuples. They borrow coordinates,
normals, triangles and all existing sidecars without repair/mutation. Mesh ingress
checks whole triples, finite positions/normals, index bounds, optional sidecar
lengths, and paired positive 1-based Modeler source indices. Empty meshes and
degenerate triangles are valid at this generic gate; operation-specific topology
requirements belong to each algorithm. Synthetic/empty GUIDs remain valid;
snapshot validation refuses duplicate nonempty GUIDs with `ERR_STATE`.

Snapshot conversion owns only small view descriptors. Material/surface catalogs,
bounds and capture freshness remain outside this increment and are not discarded
or certified. The facade must not be used as authored identity/D14 proof: those
views and provenance checks are still required before terrain consumers. Bulk
validation belongs on workers; synchronous geometry needs measured D3 budgets.

## CMake / Visual Studio (opt-in, standalone ABI link)

The standalone CMake entry needs no DevKit or Diligent:

```powershell
cmake -S AddOn/EvP/rust -B AddOn/EvP/rust/build
cmake --build AddOn/EvP/rust/build --target TapiocaRustKernel --config RelWithDebInfo
cmake --build AddOn/EvP/rust/build --target TapiocaRustAbiSmoke --config RelWithDebInfo
ctest --test-dir AddOn/EvP/rust/build -C RelWithDebInfo --output-on-failure
```

`TapiocaRustKernel` invokes the full gate. `TapiocaRustAbiSmoke` uses
`TapiocaLinkRustCore` to build/link the aggregate release archive and verify both
C and C++ header consumers. MSVC uses x64 and dynamic CRT `/MD` in **every**
configuration, including Debug, matching Rust's `-crt-static`. Archives are under
`<binary-dir>/rust/x86_64-pc-windows-msvc/release/` on Windows and
`<binary-dir>/rust/release/` on native Linux. Cargo owns incremental invalidation;
CMake tracks owned Rust sources/manifests and invokes Cargo on every link build.
The add-on and full offline test target use the same helper. The native target's
existing plain CMake link signature uses the helper's `PLAIN` argument; other
targets use `PRIVATE`. All C++ test libraries, including text dependencies, use
the add-on's `/MD` CRT/STL ABI even in Debug. Debug compilation/stack-check flags
remain enabled, but the debug CRT and its STL iterator-debug ABI are not used.

An ordinary `.apx` or offline C++ build now requires Python 3.11+ and the pinned
Rust/Cargo toolchain. `TAPIOCA_LINK_RUST_GEOMETRY` defaults ON. A missing enabled
toolchain is a configuration/build error, never a green skip. To explicitly
select the legacy/no-Rust rollback build:

```powershell
cmake -S AddOn/EvP -B AddOn/EvP/build_29 -DTAPIOCA_LINK_RUST_GEOMETRY=OFF
cmake -S AddOn/EvP/tests/cpp -B AddOn/EvP/tests/cpp/build -DTAPIOCA_LINK_RUST_GEOMETRY=OFF
```

OFF excludes the services wrapper, Rust parity tests and native startup handshake;
it does not certify Rust validators or permit attributed/terrain consumers. Set
the option back ON to restore the bridge. The additional full quality-gate target
remains independently opt-in:

```powershell
cmake -S AddOn/EvP -B AddOn/EvP/build_29 -DTAPIOCA_BUILD_RUST_KERNEL=ON
cmake --build AddOn/EvP/build_29 --target TapiocaRustKernel --config RelWithDebInfo
```

`TAPIOCA_BUILD_RUST_KERNEL=OFF` disables only the full fmt/Clippy/test build target;
it does **not** disable the enabled archive dependency. `Build-AddOn29.ps1` runs
cheap Rust structure checks first, then CMake compiles the enabled archive using
`--build`. Run the full gate separately before committing. Do not build/deploy
the add-on while Archicad is running. For native load/regression testing use
`tools/live_session.py` and the automated live workflow, never manual lifecycle
control or an unverified existing instance.

## CI and future kernel acceptance

`rust-kernel.yml` runs the full gate and standalone C/C++ smoke test on Windows
x64 MSVC and Linux, with Python checker regressions, no SDK and no Archicad.
Versions come from the checked-in
toolchain file rather than a floating stable action. It creates no release assets.
Windows also builds/runs the Debug C/C++ consumer against the same release Rust
archive; Linux execution is a CI requirement, not locally verified on Windows.

Before the first real algorithm: approve a coarse interface and ownership model;
add C++ parity fixtures, seeded invariant/property checks, degeneracy/large-world
cases, deterministic canonicalization and a time/allocation/memory baseline. A
geometry export additionally needs layout/lifetime tests, malformed
input rejection, panic containment, cancellation/revision tests, and an actual
MSVC `.apx` link/debug/rollback drill. No foundation rule authorizes changing existing
outputs or replacing SDK/rendering code.
