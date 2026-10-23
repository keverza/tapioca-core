# Rust geometry kernel: environment and compilation rules

**Status:** S1 structural-attribution increment, 2026-10-19. `tapioca-core-ffi` exports
`tapioca_version`, `tapioca_validate_mesh`, `tapioca_validate_snapshot`,
`tapioca_validate_provenance`, `tapioca_validate_attributed_mesh`,
`tapioca_validate_mesh_value`, `tapioca_validate_snapshot_value` and the initial
S2b `tapioca_transform_vectors`, `tapioca_compose_transforms` and
`tapioca_sample_polyline`, `tapioca_divide_polyline`, `tapioca_make_arc`, `tapioca_build_transform` and
`tapioca_project_to_plane` and `tapioca_copy_attributed_polygon`
primitives, plus G1 `tapioca_weld_corners` / `tapioca_analyze_mesh` (additive ABI v1).
S3a.2 adds `tapioca_terrain_triangulate` / `tapioca_terrain_split_mesh` with
validated input provenance and complete kernel support mappings (see below).
The C++ services wrapper and add-on/offline-suite archive link are enabled by
default. Startup reads/logs the ABI before callbacks/workers arm and refuses an
unsupported ABI; explicit CMake OFF is the legacy rollback path. Structural identity
views, immutable validation round-trips and complete detached value/catalog
validation are implemented. C++ engines remain the
production geometry authorities; no algorithm or consumer flag is switched.

## G1 mesh-preparation parity services

S4 native terrain ingress additionally exposes `tapioca_terrain_surface` through
`ExtractTerrainSurface`. Its input is an attributed native height-field solid
(vertex/face domains=5); it selects faces with the existing field upward rule
and merges only exactly equal xy/z render vertices. Unequal heights at one xy,
duplicate faces and inconsistent/non-manifold edge incidence refuse. It does
not certify arbitrary overlapping sheets or repair a surface. Query/fill uses
the existing terrain output records, with rows 18 (each output vertex/input
vertex contributor) and 19 (output face/input face), unchanged existing layouts.
Full-capacity alias preflight and atomic publication apply. No IDs are allocated.
`PrepareTerrainSurface` builds exact/merged provenance rows in a fresh table and
retains its complete native parent capture; `RunTerrainDesign` can do this on
its worker before building the band. Native capture and host freshness stay in
C++; there is no C++ surface-selection or weld algorithm.

`GeometryRustMeshPrep.hpp` exposes two coarse opt-in calls without switching
`GeometryExtractor`. The pure oracles are `VertexWeld.cpp`, `Aabb.hpp`,
`TriangleDegeneracy.hpp` and `WireframeEdges.cpp`, compiled into `EvPGeomTests`.
No new dependency, callback, host API, feature flag or second geometry authority.

- `WeldCorners`: ordered Add calls partitioned by nondecreasing body offsets,
  starting at zero and ending at corner count (empty bodies allowed). Key is
  `(sourceVertex, round(givenNormal*1e4))`, halfway **away** from zero, with i32
  representability required. Source vertex is an opaque signed local key.
  First position and first given normal win; positions are never proximity-welded
  or averaged. Every corner returns its output vertex, so all contributors survive.
  Native GUIDs, semantic IDs, authored lineage and sidecars remain C++ owned.
- `AnalyzeMesh`: shape/finite/index validation, exact tight bounds including unused
  **supplied** vertices, normal usability, smooth fallback normals, roundoff-zero
  flags and wire masks. No vertex/triangle is deleted/reordered. Existing normals
  must be empty or correctly shaped/finite; unusable lengths are reported, not
  repaired in the source. Fallback preserves C++ f32 face-contribution narrowing
  and f32 accumulation, then squared-norm normalization only above `1e-12`.
  The roundoff test scales edges and uses `8*DBL_EPSILON`, not an area tolerance.
  Three-component scaled hypot is compared against the compiled Windows oracle,
  including nextafter boundaries and seeded near-collinear cases; no Linux result
  is inferred. Wire bits are `v1-v2`, `v2-v0`, `v0-v1`; canonical u64 keys are
  sorted globally or within one checked range per triangle. Keys and triangles
  must use the **same** vertex domain, not pre-weld SDK indices after welding.

Welding does not implement SDK polygon-normal normalization/fallback, fan emission,
source-body bounds or sidecar publication. Analysis bounds must not overwrite
retained capture bounds that span un-emitted body vertices.

`MeshExtraction.hpp/.cpp` now contains the actual SDK-free C++ production
normalization/fan/emission seam used by `GeometryExtractor`. The offline
`test_geometry_rust_extraction.cpp` replay composes the existing Rust services
against that oracle: one candidate-fan analysis, one ordered body-partitioned
weld, then one emitted-mesh analysis. C++ still normalizes SDK corner normals.
Every emitted Add occurrence maps back to its normalized source corner; corners
that never emit are explicitly marked absent. Visibility is translated from
source-body keys to each triangle's welded domain with polygon-local ranges.
Whole `Mesh` equality includes bitwise positions/normals/bounds, GUID/type,
triangle order and every material/surface/body/polygon/wire sidecar. Source-body
bounds are retained, including un-emitted vertices and all-omitted first corners.

The fixtures are curated, including 64 deterministic seeds and reused native
collinear coordinates with authored sidecars; they are **not** complete SDK
captures. The legacy normal length/first-three fallback and final whole-mesh
smooth fallback remain C++. No new ABI export, Rust algorithm, dependency or
production Rust caller is added. Complete native source-body/corner/material
capture parity, non-finite/overflow safety differences, performance measurements
and full G1/S2b acceptance remain open. Replay is test-only and worker-classified.

Foreign input/output/error preflight covers every descriptor and full declared
capacity before borrowing. Rust owns temporary derived arrays and publishes every
prefix/count/summary together; any refusal/contained unwind leaves all outputs
unchanged. Canonical empty bounds remain infinity envelopes. NaN/infinity,
unrepresentable normal keys and nonfinite derived arithmetic refuse, rather than
preserving accidental C++ invalid-number successes. Existing panic/OOM/abort and
foreign-allocation-validity caveats apply. C++ explicitly copies corner/range PODs,
borrows flat arrays, and publishes provisional owned results only on success.
This allocates/copies; no allocation-free or synchronous latency claim. Bulk stays
worker-classified pending reproducible timing/allocation/copy/peak-heap measurement.

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

Opt-in composition measurement commands and measurement limits are documented in
[`tests/cpp/README.md`](../../AddOn/EvP/tests/cpp/README.md#opt-in-composition-measurements-s2b).
`TapiocaCompositionBench` is test-only and defaults OFF; it adds no Rust crate,
public ABI or production caller. Timing, scoped C++ allocation requests and
process-lifetime peak RSS are separate from still-unmeasured Rust allocations,
total copy traffic and operation peak heap. Composition remains worker-classified.

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

### S2b polyline division

`tapioca_divide_polyline` and the owned `DividePolyline` facade in the existing
`GeometryRustPolyline.cpp` bridge implement the existing `Curves.cpp` operation:
N **segments**, not positions, yield N+1 positions with ends or N-1 without.
Count is 1..100000; a single segment without ends has no interior position.
The additive ABI-v1 request is a flat xyz borrowed view, `int32_t` segment count
and explicit 0/1 `uint32_t` end policy (x64 size 24, offsets 16/20). No per-point
FFI calls, new bridge TU, crate, dependency or rollback-filter expansion.

Safe Rust `polyline::divide_polyline` reuses measurement and sampling, including
3D arc-length parameterization, incoming-segment joins, repeated points,
coincident/tiny curves and accumulated-length endpoint fallback. Parameters use
the C++ `double(index)/double(count)` order. No implicit closure, endpoint snap,
welding, geometric repair, ID generation or attribution is added. Future authored
consumers still need attributable outputs or refusal; this is geometry-only parity.
Ordinary C++ differentials use absolute `1e-12`, survey cases absolute `1e-9 m`,
never world-relative slack. Repeated Rust results compare exactly.

All supplied coordinates must be finite, including for empty division. For the
empty case only, no length arithmetic is evaluated: finite endpoints whose unused
subtraction/length would overflow can return empty, matching the C++ early return.
For nonempty output, any non-finite used arithmetic refuses instead of copying
the C++ oracle's accidental NaN/inf successes. Tangents are evaluated as in C++
but not returned. Invalid flags, counts, shape, pointers/capacity, non-finite input
or used arithmetic return ARG; contained unwind uses the existing INTERNAL status.

The ABI preflights descriptor, point pool, entire writable output capacity and
error storage. Known overlap wins over malformed peer storage, including nested
point/error alias with null/overflowing output. Live initialized input and live
aligned exclusive writable outputs remain caller obligations; null arrays require
zero count/capacity, OOM/abort/foreign invalid memory/UB are not recoverable. Rust
computes a complete temporary coordinate vector before the prefix is copied.
The C++ facade publishes a provisional vector by move and safely accepts its prior
result as input. Refusals preserve the whole prior result, even with a dirty stub.

Source-level storage is one Rust cumulative-distance vector and one output vector
for nonempty division, plus the C++ provisional result. The existing measurement
return's empty sample/tangent vectors do not allocate; no explicit parameter
vector is materialized. These are source observations, **not measured allocation,
copy or peak-memory counts**. Work is O(vertices + samples*log(vertices)); the
compiled C++ oracle remeasures per position. No performance improvement or
synchronous budget is claimed from that structural difference. Worker by default;
the composition-only benchmark does not classify division.

Tests include both policies at 1/2/8/100000 segments, uneven/survey/nonplanar,
repeated/degenerate/explicit-closed/self-crossing/tiny curves, rounded cumulative
endpoints, 64 seeded survey cases and a tessellated oblique arc. Rust checks empty
unused-overflow versus nonempty refusal; FFI covers layout, uninitialized/prefix
publication, invalid count/flag/shape/numeric/capacity/pointers and alias precedence.
Existing standalone C smoke, C++ polyline smoke and dirty-refusal stub targets
exercise the new symbol without adding an executable. Full S2b/G1, value carriers,
D14 generation, broader live parity and H1/terrain gates remain incomplete.

## S2b attributed single-contour polygon value copy

`GeometryRustPolygon.hpp::AttributedPolygon` pairs the existing
`GeometryEngine.hpp::Polygon` carrier with the existing complete owned
`ProvenanceRequest`. `CopyAttributedPolygon` lives beside the shared explicit
catalog converter in `GeometryRustIdentity.cpp`: no additional bridge translation
unit or CMake rollback filter is needed. It flattens C++ `Vector3` coordinates
explicitly, then calls additive ABI-v1 `tapioca_copy_attributed_polygon`. Rust
`geometry::polygon` owns the value contract and reuses `identity` validation; this
new one-way module edge is required to refuse unattributable values before copy,
not a second provenance engine or a geometry-to-FFI dependency.

The contract is a single contour in world-metre f64, right-handed Z-up: empty or
at least three stored xyz vertices, all finite. Exactly domains mask 7 represent
N vertices, N implicit successor edges (last to first) and one contour face; empty
values have zero counts in all domains. Patches are absent. An explicitly stored
closing vertex is retained as an extra vertex/edge, even if that produces a
zero-length closing edge. Order, winding, signed zero, repeated vertices and
survey coordinates are copied **bitwise**, with no arithmetic/tolerance, welding,
orientation reversal, closure removal, quantization or planarity repair. One- and
two-vertex contours are rejected by this carrier contract, not polygon modelling.

The shared schema-v1 validator checks complete mappings, current framing,
catalogs, DAG/ordered parents, native GUID/metadata/roles and retained historical
support. Default object ownership still fails closed; explicitly source-only
values may use the existing `requireObject=false` policy. Raw coordinates return
from Rust; C++ copies the **entire original provenance request unchanged** into
the provisional result. There is no newly returned Rust identity table, new
table ID, operation, predecessor remap or inferred ancestry. This is the same
value/entity ordering, not a generated Boolean/remesh result. Structural success
does not prove geometric contributor truth, valid polygon topology, hole roles,
planarity or terrain suitability. Collinear, coincident, nonplanar and
self-intersecting values intentionally round-trip; modelling gates remain separate.

FFI preflights the descriptor, points and all eleven provenance pools against
the entire caller-owned output capacity and error storage before borrowing or
publishing. Known nested error/output overlap takes precedence over a malformed
peer pool or malformed output shape. Numeric/transport errors return ARG,
attribution errors STATE, and the shared panic envelope contains unwind only.
No output prefix publishes until
validation and the Rust temporary coordinate copy succeed. Null arrays require
zero count/capacity. Live initialized foreign inputs and live aligned exclusive
writable outputs remain caller obligations; OOM/abort/invalid memory is not
recoverable. The C++ whole value publishes atomically and supports self-aliasing.

The kernel allocates one coordinate vector after validation, in addition to the
existing provenance validator's working collections. The facade owns flat input,
provisional output, reconstructed `Vector3` storage, explicit ABI catalogs and a
retained-table copy. No allocation/copy/peak-memory or latency measurement is
claimed. Work is O(points + provenance validation), worker-classified until D3
benchmarks, not a synchronous budget. No new crate, production consumer, editor
state, semantic ID allocation, Boolean/offset or terrain implementation is added.

Tests pin exact bits, source-only opt-out, empty/explicit closure/degenerate
values, all point/provenance-pool writable overlaps, malformed memory/shape,
numeric and attribution refusals. Compiled C++ tests include the independently
read saved slab contour and 64 seeded survey contours, full retained-table
equality (including >2^53 IDs, ordered DAG parents, native links and exact
historical predecessors), self-aliasing and atomic refusal. Existing standalone
C/C++ identity smoke and refusing stubs exercise the new symbol, without adding
another executable or a test-only production export. Full S2b remains incomplete.

## S3a.1 terrain TIN (`geometry::terrain`, Rust only)

`terrain::triangulate` builds a constrained Delaunay TIN over spot points,
boundary/hole rings and breaklines with the vendored `spade`; no `spade` type is
public. S3a.2 exports the implemented operations through POD carriers and the
existing services facade (below). Plan xy and z are world metres in `f64`.

- **Pre-split:** proper crossings between constraint segments get a vertex before
  triangulation (plain `f64` sweep); both segments' interpolated heights must
  agree within the request's `z_tolerance`. Touching/overlapping constraints are
  left to `spade`'s exact predicates. Rings crossing rings refuse.
- **Insertion:** every vertex first (distinct input points in order, then
  crossings by segment pair), then constraints via the non-panicking
  `try_add_constraint`; a refused or gapped constraint path is
  `TriangulationConstraint`.
- **Domain:** each face gets a winding depth (boundaries minus holes around it),
  computed topologically: 0 outside the triangulation (1 when no boundary is
  given), +/-1 across each ring edge by its direction and the ring's orientation.
  No point-in-polygon test is used (centroids misclassify slivers). Depth 1 is
  kept, 0 excluded; two depths for one face, any other depth, or a ring edge
  without exactly one kept side is `HoleClassificationFailed`.
- **Result and identity:** canonical CCW triangles (lowest vertex first, sorted),
  per-input-point output vertex, crossing records naming their two segments,
  one constraint-edge row per `(constraint, segment)` in path order with a kept
  flag, and each kept face's bounding boundary ring.
- **Proof helpers:** `extract_edges` and `verify_constraints` check the result
  against its request without `spade`; `triangulate` runs the verifier before
  returning, so `BreaklineUnenforced` refuses instead of returning a bridged TIN.

Duplicate positions merge only within `z_tolerance`; zero-length segments,
collinear input or an empty kept domain are `TriangulationDegenerate`. Tests
cover rings, holes, islands, crossings, T-junctions, overlaps, tampered results,
georeferenced coordinates, 64 seeded breakline networks on a plane (area, Euler
characteristic, heights, repeatability) and a digest pinned to `spade` 2.15.1
that catches hash-ordered output across runs. `refinement: None` retains that
unrefined path and digest.

### Bounded surface-preserving refinement

`Request::refinement: Some(Refinement)` opts into a minimum plan angle in degrees
(finite, 0..=30; zero disables angle refinement), optional positive finite maximum
plan area in square metres, and a `u32` maximum added-vertex count. Invalid settings
return `RefinementParameters`; exhaustion returns `RefinementLimit` with no partial
TIN. Angle/area settings are spade refinement targets, not unconditional quality
guarantees at acute input constraints or floating-point resolution limits.

Before `spade::refine`, every original TIN edge is retained as a private constraint.
Sampling old heights alone would not preserve a nonplanar surface if a diagonal
flipped. These private edges are not authored breaklines and do not affect domain
classification. No `exclude_outer_faces` or hash-ordered excluded-face list is used:
the whole convex hull is refined (and charged against the budget), then the existing
topological winding classifier alone keeps boundaries/holes/islands.

One original CDT snapshot is retained for spatial point location. Split original
edges are recovered by fixed-edge graph paths: original vertices are junctions,
Steiner edge vertices have two fixed neighbours. This avoids exact collinearity
assumptions for rounded midpoints on oblique survey edges. Paths rebuild every
authored `(constraint, segment)` row, in its original direction, including overlaps.
No constraints are reinserted after refinement. Edge Steiner z is linear on the
original edge (dominant-coordinate parameter); interior z is barycentric in the
original face, never sampled from an already refined surface.

`Tin::steiner` is in insertion order after inputs/crossings. Each record names all
original `SegmentRef`s, sorted/deduplicated, or an original edge's endpoints or
canonical CCW face corners. Thus excluded-domain vertices also have support.
Existing input/crossing indices are unchanged; `verify_constraints` still runs
before publication. Tests cover nonplanar surface samples, all support variants,
overlapping breaklines, holes/islands, invalid options/budget exhaustion, 32 seeded
surfaces and 16 oblique crossing networks at survey coordinates, repeatability and
a separate refined digest pinned to spade 2.15.1. No ABI/export or production caller
is added. The snapshot, graph and mapping arrays allocate; refinement remains
worker-classified, with no latency/allocation/peak-heap claim (S3a.3 owns measurement).

`terrain::split_mesh` makes a closed straight property line a path of edges of
an existing plan-view mesh. Arcs (any nonzero per-segment arc angle) refuse with
`PropertyLineArcUnsupported` before anything else; a malformed or non-simple
line, a line vertex off the mesh, a crossing of a mesh boundary edge or an
uncovered region inside the line refuse with `PropertyLineOutside`.

- Input vertices keep their indices; new vertices (line vertices inside a face
  or on an edge, then crossings in line order) take z from the mesh surface, so
  the surface never changes. Every mesh-edge predicate is evaluated low vertex
  to high, so neighbouring triangles agree.
- Triangles the line does not touch are emitted verbatim in input order; each
  straddled triangle is rebuilt with `triangulate` from its corners, its edge
  vertices and the line pieces inside it. Every output triangle names its parent
  and an inside flag (winding depth across the line edges, 0 outside the mesh).
- Mapping: per line vertex its output vertex, per new vertex its origin (line
  vertex or crossing segment) and host (edge or face), and line edges by segment
  in path order. Each line edge must border exactly one inside triangle.

Tests: generic and on-edge/through-vertex lines on grids, a line along mesh edges
(mesh returned identical), arc refusal order, seven outside cases, a line around
and across a mesh hole, malformed meshes, and 48 seeded star lines over seeded
TINs (parent areas preserved, untouched triangles verbatim, heights on the
surface, inside area equal to the line's area, conforming boundary, closed line
path, repeatability).

## S3a.2 terrain ABI and owned C++ facade

`GeometryRustTerrain.hpp/.cpp` now adds `TriangulateTerrain` and `SplitTerrainMesh`
beside its existing general services; the facade is not renamed. C++ borrows flat
world-metre arrays, explicitly copies constraint descriptors and catalogs, and
publishes one owned `TerrainResult`. `Edit/` integration still belongs to S4.

The two additive ABI-v1 exports accept a `TapiocaTerrainOutput` descriptor. Because
crossings/refinement/splitting have data-dependent sizes, `queryOnly=1` computes the
same result and publishes only scalar/row counts; all capacities must be zero.
The caller then allocates its own arrays and calls with `queryOnly=0`. The second
call **recomputes** (no cache/handle/retained borrow); unchanged inputs yield the
same sizes and ordering. Full-capacity preflight precedes foreign slice borrowing.
Insufficient capacity/refusal/contained unwind preserves every count and output
array, including unused tails. On success only result prefixes are written. Shared
UTF-8 error/status and foreign-allocation/OOM/abort caveats apply.

Input provenance is mandatory, validated by the existing schema-v1 validator:

- **TIN**, domains `3`: input point vertices and constraint segments flattened in
  constraint order as edges (closed rings have N segments, open breaklines N-1).
  Face/patch counts are zero. No single-contour value carrier is used for holes.
- **Split**, domains `7`: baseline mesh vertices **then** property-line vertices;
  property-line segments as edges; baseline triangle faces; zero patches. The
  facade retains `inputPointCount` so line vertex i resolves vertex N+i.
- Source IDs, operation DAG/ordered parents, object owners, native GUID/metadata
  bindings and retained support tables cross as the existing POD pools. Structural
  missing/conflicting/stale provenance returns `ERR_STATE` with the named schema
  diagnostic; transport/geometry errors return `ERR_ARG`. Default object ownership
  remains fail-closed; a source-only primitive must explicitly opt out.

`TerrainResult::inputProvenance` retains the **entire unchanged input** request.
`mappings` is a lossless tagged-POD encoding of kernel input remaps, face boundary,
crossings, constraint edge paths, all Steiner supports, split parents/inside flags,
new-vertex origin/host and line vertex/edge maps. Exact row fields/kinds 1..11 are
documented in `GeometryRustFfi.h`. Crossings, constraint edges and Steiner segment
rows additionally name the flattened input-provenance edge index. Original Steiner
edge/face corners resolve recursively through input/crossing rows. These supports
are not semantic IDs or a newly minted output `ProvenanceTable`: C++ session/DAG
consumers resolve support against their retained catalog, allocate persistent
identity and enforce unambiguous output ownership at integration. Bare geometry
cannot be returned without the mapping; no ID, GUID or patch token is invented.

The facade shares the existing explicit catalog converter through the internal
`GeometryRustIdentityAbi.hpp`; no second identity validator/converter or bridge
translation unit is added. Query/fill counts and support tags are checked before
whole-result publication. Inputs may borrow the prior owned result. Dirty-refusal
stubs cover both calls, query/fill disagreement and invalid tags. Query/fill doubles
geometry computation and allocates temporary geometry/support arrays; no latency,
allocation-free or peak-heap claim. Both services remain worker-classified.

Rust boundary tests compare the safe kernel and POD outputs (including refinement
and split mappings), cover every provenance pool/output pair, malformed peer ranges,
uninitialized output, capacity/mode refusal, and unwind before publication. A
failing-first regression protects recognizable input pools from diagnostic writes
even when the output descriptor is unreadable. C/C++ smoke calls both real exports;
`EvPGeomTests` compares the facade with raw ABI and independent area/surface/parent
invariants, full retained catalogs, holes/crossings, 24 seeded survey cases, atomic
refusal and self-publication. This is not native terrain/UI acceptance or a geometry
family flip. S3a.3 still owns curated native-terrain fixtures and reproducible
runtime/allocation/copy/peak-memory evidence.

## S3a.3 neutral landscape fixture increment

The shared hand-authored `rust/tests/fixtures/landscape.terrain` corpus covers
wall sides, a steep curb, stair treads, crossing path sides, courtyard/island and
overlap/T-junction features at survey coordinates. Rust integration tests and C++
raw-ABI/facade differentials use the same inputs and independent analytic area,
piecewise-surface and split-parent oracles. Every fixture must include actual
untouched remote triangles, not vacuous preservation checks. No native capture,
feature producer, patch re-evaluation or second C++ terrain algorithm is implied.

`terrain_measure` (standalone Rust example) and opt-in `TapiocaTerrainBench` measure
the pure kernel and complete two-pass owned facade. `tools/benchmark_terrain.py`
checks geometry digests/counts across fresh processes and records raw timings,
source/binary identity, requested-allocation counters and scoped memory/copy data.
See [`rust/tests/fixtures/README.md`](../../AddOn/EvP/rust/tests/fixtures/README.md)
for reproducible commands, exact workload and probe limitations. Full S3a.3 remains
open: ordinary C++ new and kernel-only requested heap are not combined facade
Rust+C++ peak heap, and a source copy ledger is not measured memory traffic.

### S3a.3 topology cost slice

The private classifier now builds one sorted edge/face-corner incidence vector,
with cached incidence spans for each face/local edge. Winding and region flood
reuse that index; authored constraint rows query the same result for kept flags,
instead of constructing another whole-mesh map. Original face-corner ordinals
preserve the old ordered-multimap traversal, including repeated incidences and
edges with more than two neighbours. No manifoldness shortcut, centroid test or
hash-dependent ordering replaces the winding checks. The index and temporary
face list are released after output flags/owners are copied, before independent
constraint verification allocates its proof sets.

Three additional unit tests compare both query forms against the former
`BTreeMap` representation and exhaust kept masks, including missing/duplicate/
nonmanifold incidences. Existing terrain/refinement/split digests and the shared
analytic corpus remain the geometry/support oracle. This changes no public Rust
API, ABI, provenance schema, two-pass ownership contract or production caller.

An **ignored, opt-in** private-stage test replays the orchestration through the
same owners and asserts complete `Tin` equality with `triangulate`. It emits JSON
stage timings only from the test binary; no clock/profiling API or dependency is
added to shipped geometry. Run separately from end-to-end measurement:

```powershell
# From AddOn/EvP/rust, release only; no concurrent benchmark/build/test process.
cargo test -p tapioca-geometry terrain::profile::terrain_stages --release --locked --offline -- --ignored --nocapture
```

The stage test uses the six fixtures plus fixed grid, three warmups and nine
samples. It is diagnostic attribution, not a performance assertion or substitute
for the uninstrumented-call probes. End-to-end reports and exact remaining limits
are in the dated private S3a.3 evidence; full acceptance still requires the facade
runtime target and combined heap/copy evidence. Both terrain services stay workers.

## S3b.0 `faer` vendoring

`geometry` depends on `faer =0.24.4` with **no default features** and only
`sparse-linalg`: no `rayon` (threads), `rand`, `npy` or `std`, so the
factorization is sequential and its maths is `libm`'s. Its closure is vendored
like `spade`'s and `check_rust.py` holds it exactly, keyed by `(crate, version)`
because `equator`/`equator-macro` are present at two versions. The policy also
names every reviewed build script and proc-macro, parses licence expressions atom
by atom (`OR`, `AND`, legacy `/`), lists the published crates whose MIT text had
to be supplied from their upstream repository, and declares `faer`'s embedded
notices for code it ports (SuiteSparse AMD/COLAMD and LAPACK, BSD-3-Clause; Eigen,
MPL-2.0, for a dense SVD Tapioca does not use). See `rust/vendor/THIRD-PARTY.md`.

## S3b.1 grading solve (`geometry::grading`, Rust only)

`solve_sparse_ls` minimizes `sum weight * (a . x - target)^2` over borrowed rows
(a term pool addressed by `Row::first/count`), with `Fixed` unknowns eliminated
rather than penalized. `solve_constrained_ls` adds `Bound`s (`lower <= x <= upper`,
cut/fill limits) and `Difference` limits (`min <= x_a - x_b <= max`, slope limits).
`reduce_slope` is a consumer: the nearest heights (least squares) whose every
distinct triangle edge has grade at most a `Slope`, with chosen vertices held.
`slope::Slope` is the one slope value type (angle, percent, rise:run, height over
a run) and converts to a gradient only at the point of use; vertical is infinite.

`faer` only factorizes: Tapioca assembles the reduced normal equations (merging
duplicate triplets by a stable sort), owns every constraint and refusal, and
refines the solve against its own assembled matrix to `Limits::residual`. All
constraints involve one or two unknowns, so they form a graph with a ground node:

- **Feasibility is exact.** Shortest paths over the constraint graph give a
  feasible start at or below the caller's start, or prove a negative cycle:
  `SolveInconsistent`. Contradictory fixed values are refused the same way.
- **Groups, not penalties.** Active limits merge unknowns into groups with exact
  offsets along a spanning tree, so every subproblem is symmetric positive
  definite and of the same reduced form.
- **Three phases, one budget.** `Limits::max_iterations` counts factorized solves.
  A cold primal-dual active set settles in a few factorizations when constraints
  are bounds; it gives up as soon as its flip count stops shrinking. On a steep
  mesh, tight edges of one triangle close cycles, which a cold working set cannot
  resolve; an operator-splitting (ADMM) estimate is then polished at tightening
  accuracies. A primal active set is the exact fallback. Exhaustion is
  `SolveNotConverged`; nothing partial is returned.
- **Certified optimality.** A result is returned only when no limit is violated
  beyond `Limits::feasibility` and non-negative multipliers on the tight limits
  balance the gradient. Where tight limits close cycles the multipliers are not
  unique, so this is decided by a maximum flow, not by spanning-tree flows.

Determinism: fixed orders everywhere; bit-reproducible on one machine (`faer`
selects SIMD kernels by CPU feature). Measure with
`cargo run --release --offline --locked -p tapioca-core-ffi --example grading_measure`
(10k unknowns): one factorization for an unconstrained solve (14-21 ms median),
six for 3621 binding cut/fill bounds (about 80 ms), seven plus the split for a
slope limit with about 7100 binding edges (about 260 ms median, 284 ms p95). The
grading services are worker-classified; no ABI or production caller uses them yet.

## S3b.2 terrain field, local operators and cut/fill (`geometry::terrain`)

`terrain::field::Field` samples a mesh's **upward** faces (unit normal z above
0.001; a closed terrain solid's sides and bottom are not the surface). Heights
name their original triangle; on a shared edge the lowest triangle index wins.
`gradient_at` takes central differences at `+-step`, one-sided where a side
leaves the surface and the face plane where both do, and says which per axis;
`slope_at` is `atan(|gradient|)`, `aspect_at` the downslope bearing clockwise from
+y. `raster` + `Raster::bilinear` resample on a regular node grid; `bounds` and
`coverage` describe what the field covers. Lookups use a uniform plan grid.

`terrain::edit::edit_terrain` moves **only vertex heights**, so indices, plan
positions and triangles keep their identity, and reports per vertex the edit that
moved it, the changed faces with their owning edits, and per edit how many
vertices it moved and how many a daylight extension or the maximum distance cut
off (the dangling-edge diagnostic). Targets are a point (flat radius), curve
(band), area ring (level or flatten to the inside mean), path (band plus a
crossfall that twists about the centreline toward a named hand) or another
surface. A falloff transition uses `1 - SmootherStep(d / blur)`; a daylight
transition marches separate cut and fill slopes (any `Slope`, vertical allowed)
with per-side extensions, absolute mode only. `Effect::Cut` keeps `min`,
`Effect::Fill` `max`; strength scales the displacement; overlapping edits keep the
largest displacement (lower index on ties). Vertices outside or on the property
ring, or inside or on a protected ring, never move. Optional Gaussian relaxation
(`sigma = radius / 3`, renormalized) covers vertices near moved ones.

`terrain::volume::cut_fill` samples a regular grid over the existing bounds (or a
domain) at cell centres, or at `k x k` sub-cell centres, with partial edge cells
and `diff = proposed - existing`. Samples outside a property ring are reported
as context, not dropped. For an edited surface on the same mesh, each sample's
difference is the barycentric blend of owned vertex displacements, so per-edit
`net` sums exactly to the total. The opt-in `terrain_volume` example reads a
plain-text mesh pair for external oracle comparison; on the private analysis
oracle's identical grids it agrees to 1e-14 relative with identical counts.
All three services are worker-classified.

## S3b.3 grading and terrain ABI and C++ facade

Four additive ABI v1 exports, each a single compute into caller-sized outputs (no
query mode, no retained handle), with the S3a.2 preflight/atomicity contract
shared through `ffi::pools`:

| Export | Outputs | Facade |
|---|---|---|
| `tapioca_grading_solve` | one value per unknown + summary (objective, iterations, factorizations, active) | `SolveGrading` |
| `tapioca_terrain_sample` | per `x y` query: height, face, normal, gradient and method per axis, slope, aspect, flags; field summary | `SampleTerrain` |
| `tapioca_terrain_edit` | heights, owners (`UINT32_MAX` none), face-owner rows (at most three per triangle, count published), one report per edit | `EditTerrain` |
| `tapioca_terrain_volume` | site, context, cell counts, per-edit rows | `MeasureTerrainVolumes` |

Records are `f64`-first with explicit `reserved` words; their sizes are pinned by
Rust `size_of` tests and `rust/tests/abi_header.cpp`. The edit export requires
schema-v1 input provenance over the mesh's vertices and faces (`domains = 5`) and
retains the catalog in the facade result; attribution refusals are `STATE`,
everything else `ARG`. Surface targets are separate mesh views whose own pools are
preflighted in a second pass before any borrow. Edit geometry (curves, paths,
rings, clip and protected rings) is one flat xyz pool addressed by scalar range.
The facade declarations live in `GeometryRustTerrain.hpp`; implementations are
`GeometryRustGrading.cpp` and `GeometryRustTerrainField.cpp`, excluded from the
add-on when Rust is not linked. No `Edit/` caller exists yet.

## S4.2 hybrid band ABI and C++ facade

`tapioca_terrain_hybrid_build` is additive: its request, counts and output
records are new, so the pinned `TapiocaTerrainRequest`/`TapiocaTerrainOutput`
stay unchanged. It follows the query/fill contract of
`tapioca_terrain_triangulate` (query publishes only counts with zero
capacities; fill recomputes and publishes atomically). Input provenance is the
split's (`domains = 7`: mesh then region-ring vertices, ring segments as edges,
base triangles as faces); stations are authored parameters without entity rows.

| Output | Meaning |
|---|---|
| `points`, `base` | proposed xyz and base height per output vertex (plan identical) |
| `owners` | `0` where the band changed the height, `UINT32_MAX` otherwise |
| `triangles`, `mappings` | rebuilt mesh; rows 9 (split vertex) and 12..17 (region face, region line vertex, crossing support, line vertex, line edge, ring vertex) |
| `lines` | per design line: band edge, shoulder end, daylight (root outward), end section or crowned centreline |
| `sides` | per station `[left, right]` daylight root count, first and last root |
| `site`, `context` | exact triangle-sum cut/fill inside / outside the region |

Record sizes are pinned in `terrain/tests/band.rs` and `rust/tests/abi_header.cpp`.
The facade is `BuildHybridTerrain` in `GeometryRustHybridTerrain.hpp`; it never
reinterprets a row, it only checks kinds and query/fill agreement. P1 limits
stay explicit: whole-region rebuild per call, constant daylight root count per
side, unconstrained end caps counted in the site total, and no height-error
refinement between stations.

S4.3 adds the crown: `Camber::Crown` in `terrain::design` falls both halves away
from the centreline (`-g * |lateral|`), and `terrain::hybrid` then adds the
centreline as a breakline (`crossfallHand = 3`, line kind 5) with a centre
vertex in each end section, so the crease is a mesh edge and the triangle-sum
volume stays exact (crowned Appendix A: fill 23.04, cut 30.5067 m³ over 40 m).
`EditTerrainDesign` gives each station the cross-section range in force there and
runs a range change over a short explicit taper (`TerrainDesignOptions::taper`,
0.5 m) ending at the range start. A zero-length step would make the band
discontinuous; its end faces need corner fans, which belong to composition.

**Located conflicts (S4.3).** `terrain::hybrid::conflict` names where a band
cannot be built as authored, each with station, side, position, magnitude and a
suggested fix: `DaylightNotReached`, `DaylightBeyondLimit`, `VerticalFace` and
`DaylightTopology` are hard (the build still refuses on the first, with its
existing code), `EdgeContradictsEffect` is soft (the band is held at the ground
there and `Built::conflicts` says where). `hybrid::assess` lists all of them
without building. The edit operator's soft tier gains `Edited::clips`, one row
per vertex behind the unchanged `extension_clipped`/`limit_clipped` counters.

Both reach C through additive query/fill exports that reuse the existing request
records, so the refusal contract (no write on refusal) and every pinned output
record are unchanged: `tapioca_terrain_hybrid_conflicts` (succeeds for a band the
build refuses) and `tapioca_terrain_edit_clips`. `AssessHybridTerrain`,
`HybridTerrainResult::conflicts`, `TerrainEditResult::clips` and
`EditTerrainResult::clips` carry them in C++; `AssessTerrainDesign` reports them in
path stations, and a refused `EvaluateTerrainDesign` names its first place.

## S4.3 fixed-height controls (Rust kernel checkpoint)

`terrain::hybrid::build_locked(request, Locked { lines, points })` adds fixed
xyz points and open xyz polylines to the same region TIN as the path band.
`retriangulate_region_with_points` inserts isolated controls without making
zero-length constraints; every input point maps to an output vertex, including
coincident points. Existing region/build entrypoints use empty control pools.
Multiway crossing records map by their actual `Crossing::vertex`, not by a
presumed appended offset (several segment pairs can share an existing vertex).

Locked lines precede the design lines and constrain every split edge. Heights
interpolate linearly along each control segment, with supplied endpoint bits
retained. `Built::locked` lists output vertices, exact fixed heights and **all**
point/segment contributors; `LineKind::Locked` locates each input line. These
locators are not authored IDs: a future wrapper resolves them through retained
input provenance. Locked height ownership is separate from band ownership.

Conflicting controls or grading that misses a control by more than the request's
absolute metre tolerance return `HybridError::LockedConflict`, carrying the
control(s), candidate vertex, world position, signed height mismatch and suggested
fix. Compatible controls keep the first input height (lines, then points), and
the proposed surface takes that height exactly. Daylight vertices have a stronger
bitwise base-preservation contract: even a within-tolerance difference refuses.
Malformed/non-finite pools, unusable interpolation and overflowing differences
refuse `LockedParameters`; boundary/outside controls retain the region refusals.

These are constraints, not independent grading tools: an incompatible control
does not generate an unauthored ramp, clamp the band or launch a solve. The whole
candidate is discarded on refusal. Base-edge constraints and exact triangle-sum
volumes remain in force. Controls are strictly inside the rebuild ring.

**Not exposed through C yet.** Every pinned ABI record/export stays unchanged;
the old hybrid encoder explicitly refuses fixed-control ancestry rather than
discarding it. Additive ABI/facade transport with required control provenance,
all-conflict assessment, document persistence/UI, locked surfaces and enforcement
in the separate soft-edit/solve lanes remain downstream. No native or performance
acceptance is implied. The existing between-station chord-error limit remains.

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
