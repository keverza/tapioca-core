# Rust kernel foundation

The aggregate `tapioca-core-ffi` staticlib exports `tapioca_version` and shared
legacy geometry-only mesh/snapshot validators, with status/error handling and
panic containment. The add-on and offline C++ suite link it by default through a
thin `tapioca::geometry` services wrapper. Startup reads/logs ABI version 1; no
geometry consumer is switched. Separate provenance/attributed-mesh exports validate
schema-v1 structural identity, with an owned-C++ → POD → Rust validation round-trip.
Complete detached value/catalog validators are separate opt-in exports, including
retained bounds and capture-stamp assertions. Legacy validation still certifies
neither D14 nor catalogs. Existing
C++ geometry remains authoritative in production; parity primitives do not flip consumers.

The initial S2b affine-vector batch is available for differential tests through
`tapioca_transform_vectors`: 3x4 f64 positions/directions/inverse-transpose normals
with atomic caller-owned output and orientation reporting. No production consumer
is switched. This is not full S2b, a mesh/provenance transform, or a latency claim.

The S2b continuation adds `tapioca_compose_transforms`: a chain of row-major
3x4 matrices in application order, folded from identity. Rust computes without
allocation and publishes one complete caller-owned matrix only on success.
Non-finite input or intermediate coefficients/determinants refuse; singular
matrices remain valid. C++ differential tests use the existing `Compose` oracle.
No production consumer is switched, and bulk chains stay worker-classified until
measured. No mesh/provenance, builder or terrain capability is implied.

The next S2b primitive, `tapioca_sample_polyline`, measures flat xyz polylines and
samples a batch of finite, clamped arc-length fractions. Cumulative lengths,
positions and tangents publish together into disjoint caller-owned buffers only
after complete success. Input coordinates stay borrowed and unchanged. C++
`Curves.cpp` remains the production authority and compiled parity oracle, including
incoming-segment joins, repeated vertices and squared-norm underflow fallbacks.
Non-finite/overflowing arithmetic refuses. No implicit closure, arc construction,
semantic IDs, polygon repair or latency guarantee; bulk requests use workers.

`tapioca_divide_polyline` adds segment-count/end-inclusion parity with the
existing C++ `DividePolyline`, reusing the same safe measurement/sampling code.
N segments (1..100000) yield N+1 positions with ends, N-1 without; one segment
without ends returns empty after shape/count/finite checks without evaluating
unused lengths. Other calls require finite used arithmetic, preserve C++ joins,
repeated/zero-length/explicit closing vertices and squared-norm fallbacks, and
publish the whole xyz prefix atomically. No new attribution or production caller;
bulk requests remain worker-classified, not covered by composition benchmarks.

`tapioca_make_arc` adds bounded S2b tessellation parity with C++ `MakeArc`: an
adaptive right-handed frame, signed radians (including multiple turns), 2..4096
segments and one evaluated endpoint per segment plus the start. It does not snap
full-circle closure or introduce an analytic `ArcPolyline` carrier. Non-finite
coordinates/angles and unusable squared normal lengths/overflow refuse atomically.
One Rust temporary point vector copies into provisional caller-owned output; no
allocation/borrow escapes. Worker by default until measured; no consumer flip.

`tapioca_build_transform` completes the existing affine builder subset: translation,
right-handed Rodrigues rotation, nonzero per-axis scaling and plane mirroring.
The tagged request preserves C++ pivot-subtraction order and squared-norm
normalization. Only selected fields are consumed; unused fields are ignored.
Finite coefficients/determinant and ordered pivot sums are required; failures
preserve the caller's complete prior matrix. Nonzero factors whose determinant
underflows to zero retain the existing affine semantics, not a new orientation
sign-recovery policy. Rust building allocates nothing; the existing C++ facade
copies only POD inputs/output. This is not an attributed mesh transform, a
production consumer switch or a measured synchronous budget.

`tapioca_project_to_plane` adds a neutral origin/normal/authored-u plane value,
source-preserving frame derivation and flat xyz batch projection. The finite-input
oracle is the existing editor's `PlaneAxes`/`ProjectToPlane`, including its
`length < 1e-12` world-Z/seed fallbacks, reciprocal-multiply normalization and
normal-x seed rule (not the arc frame rule). Raw input fields round-trip exactly,
including signed zero; they are not replaced by normalized directions. Empty
points request only frame/value output. Non-finite input/derived arithmetic
refuses atomically. Geometry services do not include or depend on `Edit/`; its
state, WorkingPlane provenance and production callers remain unchanged. Bulk
requests stay worker-classified until measured; this is not full S2b acceptance.

`tapioca_copy_attributed_polygon` round-trips the existing C++ single-contour
`Polygon` value through flat xyz, using the shared structural provenance validator.
Every coordinate bit/order/duplicate/explicit closure survives. Finite empty or
at-least-three-vertex values require complete vertex/implicit-edge/face attribution
(N/N/1, all zero for empty; exactly domains mask 7). The C++ facade retains the
entire validated input table unchanged; Rust does not create IDs, table IDs or
lineage. Missing/conflicting/stale attribution refuses before coordinate output.
This is not planarity, Boolean, hole-role or geometric-contributor validation:
degenerate/nonplanar/self-intersecting values may copy. Worker by default, no
production consumer flip. See `GeometryRustPolygon.hpp` for the owned value API.

The G1 mesh-preparation batch adds `tapioca_weld_corners` and
`tapioca_analyze_mesh`, exposed by `GeometryRustMeshPrep.hpp`. Welding partitions
ordered corners by body offsets, uses the legacy source/rounded-normal key and
returns **every** corner-to-output-vertex mapping. First position/normal wins;
no proximity weld, coordinate averaging, source-node rewrite or semantic IDs.
Normals are supplied as given; SDK polygon normalization/fan emission remains
C++. Analysis preserves vertices/triangles and returns tight supplied-vertex
bounds, original-normal usability, f32 area-weighted fallback normals, roundoff
flags and barycentric wire masks. Visibility keys use the same vertex-index
domain as the supplied triangles, not a pre-weld source domain. Tight bounds
must not replace capture bounds spanning un-emitted body vertices.

Both ABI calls validate all ranges and **full capacities** before borrowing,
compute into Rust-owned temporaries, then publish all output prefixes/counts
together. Refusal or caught unwind preserves outputs. The C++ facade uses
provisional owned vectors and explicit POD conversion. Geometry/identity/face
sidecars remain caller-owned. No production extractor is switched; full G1/S2b
acceptance, native SDK-capture parity and performance budgets remain separate.
These are worker operations, not synchronous latency claims; no new dependency.

```text
rust/
  geometry/   safe borrowed mesh and identity validation; no dependencies or host APIs
  ffi/        aggregate staticlib + rlib; depends only on geometry; C ABI envelope
  tests/      standalone C/C++ header and archive-link smoke test
```

One-way ownership is deliberate: algorithms cannot import the boundary. Only one
aggregate staticlib will ever be linked; the rlib exists for Rust tests/doctests.
Do not create unused math/terrain/topology modules to fill out a proposed layout.

From `core/`:

```powershell
powershell -NoProfile -File tools/build/Setup-Rust.ps1 -InstallRustup
python tools/quality/check_rust.py
python tools/quality/check_rust.py --build
python -m pytest tools/quality/tests/test_check_rust.py -q
```

Rustup installation is explicit, hash-verified, and does not modify the global
default or user PATH. The toolchain is pinned by `rust-toolchain.toml`. Re-run
setup without `-InstallRustup` on machines already using Rust.

`--build` is the CMake archive path (structure/secrets + pinned offline release
build), not a substitute for the full gate. See [coding rules](AGENTS.md) and
the [build/contracts guide](../../../docs/guides/rust-kernel.md).

`TAPIOCA_LINK_RUST_GEOMETRY=OFF` is the explicit CMake legacy/no-Cargo rollback
path; it excludes the Rust wrapper/parity tests and the startup handshake. It is
not a successful attributed-validator path and cannot enable terrain consumers.
