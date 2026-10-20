# Rust geometry kernel: environment and compilation rules

**Status:** S1 structural-attribution increment, 2026-10-19. `tapioca-core-ffi` exports
`tapioca_version`, `tapioca_validate_mesh`, `tapioca_validate_snapshot`,
`tapioca_validate_provenance`, `tapioca_validate_attributed_mesh`,
`tapioca_validate_mesh_value`, `tapioca_validate_snapshot_value` and the initial
S2b `tapioca_transform_vectors`, `tapioca_compose_transforms` and
`tapioca_sample_polyline`, `tapioca_make_arc`, `tapioca_build_transform` and
`tapioca_project_to_plane`
primitives (additive ABI v1).
The C++ services wrapper and add-on/offline-suite archive link are enabled by
default. Startup reads/logs the ABI before callbacks/workers arm and refuses an
unsupported ABI; explicit CMake OFF is the legacy rollback path. Structural identity
views, immutable validation round-trips and complete detached value/catalog
validation are implemented. C++ engines remain the
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
Separate identity fixtures cover authored structural relationships, not geometric
contributor truth, terrain topology, persistence or catalog semantics.

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
or certified. The legacy facade must not be used as authored identity/D14 proof.
Bulk
validation belongs on workers; synchronous geometry needs measured D3 budgets.

### Structural authored provenance (separate gate)

`GeometryIdentity.hpp` owns the C++ session-value carriers. The bridge in
`GeometryRustIdentity.cpp` explicitly converts metadata into C POD arrays while
borrowing mesh coordinates and compatibility sidecars unchanged. Rust never
allocates authored IDs, normalizes native bindings, repairs input or retains views.
The geometry crate owns safe validation; the FFI crate alone owns foreign borrows.

The gate validates schema/session/revision, session-wide catalog ID uniqueness,
unique acyclic operation parents (preserving caller order), owner reachability, canonical uppercase native
GUIDs, explicit metadata UUID presence, UTF-8 roles, and checked ranges/references.
Support tables are explicit current or retained historical inputs. Every entity in
each declared domain has exactly one mapping with source/history contributors;
unknown discriminators, gaps, duplicates and invalid predecessors refuse.
Patch rows require nonzero object-scoped tokens; member rows may carry an optional
token with an owner. Semantic patch matching and allocator history remain C++ work.

C++ IDs remain semantic aliases of the existing editor's u64 `Id`; Rust uses
distinct transparent newtypes. Runtime catalog lookup rejects cross-domain IDs.
The gate preserves parent-list order, not a separately inferred chronological
ordering. Retained historical inputs may be at the current revision (including
revision-zero baselines). Native GUID reuse is not a global uniqueness rule;
semantic native ownership/reconciliation remains with the C++ session.
The owned C++ provenance request requires objects by default; genuinely
source-only primitives must explicitly opt out. Object-producing adapters must
keep that requirement enabled, rather than treat generic validation as proof.

`ValidateProvenance` accepts the primitive's declared counts/domains;
`ValidateAttributedMesh` fixes vertex+face domains (mask 5) and checks counts
against real mesh arrays. Complete snapshot/catalog validation is not implied.
Structural refusal returns `ERR_STATE` with diagnostic 1..4 (unattributable,
conflict, stale, unstable patch key). Success publishes 0. Transport errors and
caught panics preserve the prior diagnostic. All inputs remain immutable; no
Rust-generated geometry/identity output, persistence or native reconciliation is
claimed by this validation round-trip. No terrain consumer is enabled.

Advisory size review: `identity/mapping.rs` keeps the deterministic, ordered
validation pipeline and its private checks together; `identity/tests.rs` keeps
one shared borrowed fixture and its explicit mutation/refusal matrix together.
The larger tests are verbose POD fixtures grouped by binding, support-context and
mapping-kind contracts. Size/line advisories remain visible; no lint is suppressed.
The value tests likewise retain one shared explicit borrowed snapshot fixture and
its mutation/refusal matrix. A private native-GUID checker is shared by value and
identity validation so their canonicalization policy cannot drift.

## Complete detached values and catalogs

`ValidateMeshValue` and `ValidateSnapshotValue` are additive gates; legacy mesh
and snapshot calls retain their geometry-only contract. The complete view carries
all `geomsrv::Mesh`/`Snapshot` fields without transferring their ownership. Bounds
are not repaired: all axes must be finite and ordered and contain all vertices;
empty meshes may retain the canonical all-`+inf`/all-`-inf` empty box or a finite
ordered box. Wire masks use only three bits. Material/surface sidecars are
nonnegative; zero remains unknown/no-material, never a guessed reference.

All supplied catalog rows are checked, including unreferenced entries: positive
unique keys, finite transparency fractions in [0,1], UTF-8 text and canonical
uppercase non-nil surface GUIDs. Display names may be empty or nonunique and are
preserved exactly. Catalog order and material/surface numbering need not match.
Pool `externalReference` is opaque signed identity (including pen/zero values),
not a face-surface mapping and never a required surface-catalog reference.

By default, absent catalogs/references remain explicitly unknown; this supports
detached/type-filtered subsets. `requireCatalogs` requires positive face material
keys in `materialIdentity` and positive face surface keys in `surfaceAttributes`.
Zero references are exempt. Transparency stays sparse even in this mode; missing
does not mean opaque or glass. No catalog is reconstructed from another.

Scope is `all` or `selection`; complete-model implies `all`, not the reverse.
Detached values may have zero ID/stamp, including a complete direct capture.
Nonzero `expectedCaptureStamp` compares the retained model-watch stamp only.
It does not validate ID allocation, read host state, prove completeness or replace
the host publication/freshness check. Editor revisions and D14 provenance remain
separate. Consumer-specific source-face, winding/manifold and selection checks
still apply; complete-value success does not certify a takeoff or terrain result.

## Initial S2b affine-vector batch

`TransformVectors` borrows flat xyz f64 input and explicitly copies only the
small row-major 3x4 matrix descriptor. Points translate, directions do not, and
normals use the cofactor inverse transpose without normalization. Exactly singular
normal matrices retain the input normal, matching the documented C++ oracle.
Non-finite inputs, determinant/cofactors or results refuse. One Rust temporary
holds the completed output before copying to a provisional C++ vector; coordinates
and the orientation report publish together only on success. No allocation or
borrow escapes Rust and no input, identity, sidecar or winding is changed.

Compiled C++ fixtures compare the existing transform builders/application functions
against Rust for identity, translation, rotation, scaling, reflection, composition,
shear, survey coordinates, singular normals and seeded matrices. These are parity
tests, not a production family flip, D14 geometry-operation certification or a
benchmark. Bulk calls remain worker-classified. Plane/interval/curve/terrain value
conversions and massing-corpus capture remain outstanding for the full S2b gate.

The composition continuation folds a borrowed matrix chain in application order
from identity: `[a,b]` means `Compose(b, Compose(a, Identity()))` in the C++
oracle. The kernel allocates nothing, preserves the oracle's sum association,
accepts singular matrices, and refuses non-finite input or intermediate matrix
coefficients/determinants. The C ABI publishes one complete caller-owned matrix
only after success, with the existing panic/status/error envelope and explicit
input/output/error disjointness. The owned C++ facade copies only matrix PODs;
its result may alias an input because all conversion precedes publication. Bulk
chains remain worker-classified, not a measured synchronous latency guarantee.
Tests compare 64 seeded chains, order, empty identity, reflection/singularity,
overflow, malformed foreign shapes, overlap and refusal immutability. The affine
test module intentionally retains shared explicit matrix fixtures for vector and
composition semantics (623 lines); its size advisory is not suppressed.

### S2b transform builders

`BuildTransform` adds translation, right-handed rotation, scaling and mirroring
through the existing affine bridge. A tagged ABI-v1 request explicitly copies
origin/vector/radians and returns one complete row-major 3x4 matrix. Translation
consumes displacement only; rotation consumes pivot/axis/radians; scaling consumes
pivot/dimensionless factors; mirroring consumes plane origin/normal. Unused fields
are ignored, not normalized or reinterpreted. Unknown tags refuse. Rust uses a
typed enum internally rather than an unchecked discriminator.

The compiled `Transforms.cpp` oracle pins Rodrigues arithmetic, squared-norm
normalization, and pivot conjugation as ordered coordinate subtractions (not
`origin - Apply(linear, origin)`, which rounds differently). Zero scale factors,
zero/underflowed/overflowing direction lengths and non-finite used inputs refuse.
Finite coefficients/determinants and pivot intermediates are required. Nonzero
tiny scale factors with a determinant underflowing to zero retain the existing
affine gate's singular semantics: this is **not mathematical orientation-sign
recovery** or permission to bypass a future mesh/winding consumer gate.

Rust building is allocation-free, constant bounded work; C++ uses stack POD
copies and publishes only after success. Request/output/error storage must be
live, aligned and disjoint. Known error overlap wins even if the other record is
malformed, preventing diagnostic corruption. The existing shared panic/error
envelope and foreign-allocation/OOM/abort limitations apply. Tests cover all tags,
independent analytic points, survey pivots, 64 seeded builder sets, inverse/mirror
composition, unusable directions, determinant/pivot overflow, underflow semantics,
ignored fields and full matrix preservation. Ordinary compiled differential
matrices use absolute `1e-12`; survey-pivot cases allow absolute `1e-8 m` for
translation terms, never world-relative scaling. These are parity tolerances,
not measured performance or a global numerical tolerance policy.

No new dependency, bridge translation unit, production family flag or semantic ID
is introduced. The existing affine smoke and test-only refusal stub exercise the
new export. Full S2b/G1, authored mesh transforms, remaining value conversions,
benchmark evidence and terrain gates remain incomplete. Worker-classified until
D3 timings justify main-thread use, despite the bounded allocation-free kernel.

## S2b polyline measurement and batch sampling

`SamplePolyline` borrows flat xyz world-metre coordinates and finite length
fractions, never reinterpreting C++ vector objects. It measures 3D cumulative
lengths once in vertex order and samples in caller parameter order, using the
compiled `Curves.cpp` oracle's arithmetic/semantics. Fractions clamp to [0,1];
exact joins use the incoming segment, even a zero-length leading segment. Fully
coincident curves and selected segments whose squared length underflows use +X
tangents. Cumulative rounding can swallow a final tiny segment: endpoint selection
retains that C++ behavior rather than silently introducing a new numerical policy.
No implicit closure, welding, polygon validity, arc constructor or semantic ID
allocation is added. Non-finite coordinates/parameters and non-finite derived
lengths/samples refuse the entire batch; these are deliberate safety refusals,
not claimed parity for the C++ oracle's NaN/inf results.

Empty parameters measure only (at least two points still required). The C ABI
checks descriptors, input ranges and all four output regions for shape/overlap.
Only requested prefixes publish after complete success; refusals preserve total,
cumulative distances, positions and tangents. The facade uses provisional C++
arrays, so inputs may borrow prior result arrays safely. Rust owns three temporary
result vectors; no allocation or borrow crosses the boundary. Coordinates,
topology, authored IDs and provenance remain caller-owned and unchanged. Sampling
is not a D14 geometry-generation/attribution primitive or production family flip.
Work is O(vertices + samples * log(vertices)); bulk calls remain worker-classified.
No benchmark, main-thread budget or full S2b completion is claimed.

## S2b bounded arc tessellation

`MakeArc` explicitly copies centre/normal coordinates into the additive ABI-v1
`TapiocaArcRequest`; it never reinterprets C++ `Vector3` memory. Pure Rust owns
frame construction and tessellation, reusing `affine::Vec3`. World-metre/radian
inputs retain the C++ `Curves.cpp::MakeArc` adaptive seed (normalized `abs(z)<0.9`),
cross products, squared-norm normalization, sum association, signed sweep and
2..4096 segment limits. Angles are not reduced modulo a turn, and the closing
point of a full circle is evaluated, not snapped. Exactly `segments+1` points are
returned in sweep order. No analytic ArcPolyline, welding, ID allocation or
geometry-family switch is introduced; `Curves.cpp` remains authoritative.

Non-finite centre/normal/radius/angles, nonpositive radius, zero sweep, unusable
squared normal length (including underflow/overflow), and non-finite intermediate
angles/points refuse. This deliberately excludes the legacy oracle's accidental
successful NaN/inf output, not a production behavior repair. A complete Rust
temporary vector copies only the requested prefix into disjoint caller-owned
storage; the entire declared output capacity must be disjoint from request/error.
The C++ facade publishes its provisional vector only on success. Refusals/contained
panics preserve prior results. Foreign allocation validity and OOM/abort are not
recoverable. The bounded operation stays worker-classified until D3 measurements;
no performance or full-S2b/terrain acceptance is claimed. Compiled C++ differential
tests use absolute `1e-9 m` for ordinary/survey positions, with a dedicated tighter
tiny-radius check, plus plane/radius/winding invariants and seeded cases. No
bitwise cross-toolchain transcendental parity is claimed.

## S2b neutral plane values and batch projection

`PlaneValue` mirrors the existing legacy plane's origin, normal and optional
authored u without pulling `Edit/` into a geometry service. The bridge copies all
nine doubles explicitly; Rust derives a frame and projects borrowed flat xyz
points using the compiled `EditSnap.cpp::PlaneAxes/ProjectToPlane` and
`EditTypes.cpp::Normalize` as the finite-input oracle. Raw source fields return
exactly, including signed zero and unnormalized directions, separately from the
derived axes. This is not an editor `WorkingPlane` metadata/provenance carrier or
an authored identity allocation. Editor callers, state and production math remain
unchanged. An empty point batch still derives the frame and returns the value.

The legacy policy is explicit: normalization uses the squared norm and multiplies
by reciprocal length. A **finite** normal shorter than `1e-12` falls back to world
Z; a finite short/parallel authored u derives seed axes. Equality at `1e-12` is
accepted. Seed selection uses normalized `abs(normal.x)<0.9`, unlike arc
tessellation's normal-z rule. An accepted authored u uses `v=Cross(n,u)` without
additional normalization, exactly as the oracle. The threshold is inherited
dimensionless direction-length policy, not a global world-metre epsilon.

Malformed xyz, non-finite plane/point coordinates and non-finite intermediate
squared norms/products/differences/dot sums/results refuse. Thus finite tiny
direction fallbacks are preserved, but C++'s accidental NaN/inf fallbacks or
successful non-finite outputs are not reproduced. No welding, origin shift,
quantization, plane repair or policy unification with arc/transform frames occurs.

The additive ABI-v1 request borrows point storage; a complete Rust temporary
holds projected points and the frame/source value. Both outputs publish only
after full success, and only the requested point prefix is written. The entire
declared point capacity and required frame must be disjoint from each other and
all inputs/descriptors/error storage. Empty arrays may be null/zero; frame storage
is always required. Foreign validity/OOM/abort caveats and shared panic/error
envelope apply. C++ uses provisional arrays/frame, so inputs may safely borrow the
prior result's plane/points. No Rust allocation or reference escapes ownership.

Tests pin raw value bits, fallback boundaries, seed axes, authored tangents,
survey coordinates, 64 seeded frames/projections, idempotence, late arithmetic
refusal, unused output capacity, output-pair and input/descriptor/error overlaps.
Compiled frame parity uses absolute `1e-12`, projected points absolute `1e-9 m`, not relative
world-scale slack; raw source fields compare bitwise. Bulk O(points) calls remain
worker-classified until D3 latency/copy/allocation/peak-memory evidence exists.
This completes only the legacy neutral plane subset, not full S2b/G1 or terrain.

## CMake / Visual Studio (opt-in, standalone ABI link)

The standalone CMake entry needs no DevKit or Diligent:

```powershell
cmake -S AddOn/EvP/rust -B AddOn/EvP/rust/build
cmake --build AddOn/EvP/rust/build --target TapiocaRustKernel --config RelWithDebInfo
cmake --build AddOn/EvP/rust/build --target TapiocaRustAbiSmoke TapiocaRustWrapperFailure TapiocaRustIdentitySmoke TapiocaRustValueSmoke TapiocaRustAffineSmoke TapiocaRustPolylineSmoke TapiocaRustPolylineFailure TapiocaRustArcSmoke TapiocaRustArcFailure TapiocaRustPlaneSmoke TapiocaRustPlaneFailure --config RelWithDebInfo
ctest --test-dir AddOn/EvP/rust/build -C RelWithDebInfo --output-on-failure
```

`TapiocaRustKernel` invokes the full gate. `TapiocaRustAbiSmoke` uses
`TapiocaLinkRustCore` to build/link the aggregate release archive and verify both
C and C++ header consumers. `TapiocaRustWrapperFailure` separately links only
test stubs to prove version/status refusal and output preservation; never use
those stubs in a production target. `TapiocaRustIdentitySmoke` links real Rust and
the C++ converter, including revision zero, two GUIDs for one object and refusal
immutability. Stub tests also cover inconsistent status/diagnostic combinations.
`TapiocaRustValueSmoke` covers retained bounds, sparse catalogs, explicit unknowns
and independent capture stamps through the real C++ converter and Rust validator.
`TapiocaRustAffineSmoke` checks the new affine ABI and atomic publication; failure
stubs also reject inconsistent orientation reports without changing caller output.
`TapiocaRustPolylineSmoke` checks layouts and real measurement/sample conversion;
`TapiocaRustPolylineFailure` checks facade atomicity for status refusals even if a
test-only stub dirties provisional storage. The C smoke also calls the real export.
`TapiocaRustArcSmoke` checks arc layout, real export/facade publication and refusal;
`TapiocaRustArcFailure` proves prior results survive a refusing transport that has
dirtied provisional output. The C smoke independently calls the real arc export.
`TapiocaRustPlaneSmoke` checks frame layout, raw value preservation, real projection,
frame-only calls and aliased owned input; `TapiocaRustPlaneFailure` checks whole
result preservation even when a refusing stub dirties provisional frame/points.
The C smoke calls the real plane export and checks independent analytic output.
MSVC uses x64 and dynamic CRT `/MD` in **every**
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
