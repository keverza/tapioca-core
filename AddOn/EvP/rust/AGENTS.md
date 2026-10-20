# Rust kernel coding rules

Read `README.md` and `../../../docs/guides/rust-kernel.md` first. This is an
**S1 legacy-ingress foundation** with a version/validation C ABI, not an authorization
to port or refactor C++ algorithms outside an approved capability/stage.
Before a feature, identify its owner, reusable primitives, input/output contract,
invariants, C++ parity oracle, and tests. Keep changes independently verifiable.

## Ownership and organization

- `geometry/` owns pure computation; `ffi/` may depend on `geometry/`, never the
  reverse. Cargo and `check_rust.py` enforce this crate-level dependency boundary.
- No Archicad, Diligent, ImGui, Windows, CPython, process, filesystem, network, or
  UI APIs in geometry. No callbacks into the host from algorithms.
- Do not create the proposed math/topology/terrain directory tree in advance.
  Add modules only for implemented responsibilities. Avoid generic `utils` and
  `algorithms` dumping grounds, mutually dependent modules, or speculative traits.
- Use Rust naming: `snake_case` functions/modules, `UpperCamelCase` types. Prefer
  concrete structs/enums/functions. Private by default, `pub(crate)` when needed;
  a `pub` item needs a consumer, documentation, and a stable contract justification.
- New crates, dependencies, public APIs, unsafe allowances, or cross-module edges
  need a concrete reason in the change and matching policy/test updates. Do not
  weaken workspace gates merely to make a feature compile.

## Review triggers (not arbitrary split mandates)

| Item | Guidance |
|---|---|
| Rust file | Target 200-500 lines; warn >600, review >1000; smaller is fine |
| Function | Target 15-60 lines; advisory Clippy review >100 |
| `impl` | Target 50-250 lines; human review >400 |
| Arguments | At most five; strict Clippy gate; use a meaningful request type, not a bag of unrelated options |
| Nesting | Target at most three; advisory Clippy review >4 |
| Complexity | Advisory Clippy review above 25 |
| `clone()` | Justify ownership meaning; measure copies/allocations in hot paths |

Never split a coherent algorithm merely for line counts. File/function/nesting
reviews do not fail CI; record the cohesion reason when a trigger is crossed.
`impl` size, module cohesion, and API necessity are human reviews, not parser claims.

## Hard geometry contracts

- Core `#![forbid(unsafe_code)]` is permanent. Borrow slices before cloning owned
  geometry; validate lengths, overflow, finite coordinates, indices, and geometric
  preconditions at ingress. Use checked access or iterator/chunk APIs, not unchecked
  indexing. Validation must match the operation, not impose manifoldness everywhere.
- World coordinates are **metres in f64** in the existing C++ `Geometry/Mesh.hpp`.
  Preserve its right-handed, Z-up frame and provenance/indices. Document local
  origins and conversions; never silently narrow survey coordinates to f32.
- State units in types/fields/docs; distinguish radians/degrees and metres/mm.
  Reuse typed IDs for topology domains when they exist; do not mix face/vertex IDs.
  Keep identity/topology separate from positions where the operation needs both.
- A tolerance policy belongs to the owning algorithm: explicit absolute/relative
  meaning, units, scale assumptions, and boundary semantics. No magic epsilon,
  uncontrolled floating equality, blanket float-comparison waiver, or a single
  global epsilon pretending to fit all algorithms. Preserve existing C++ tolerances
  during a parity port unless a behavior change is separately authorized.
- Errors use `Result`/typed enums. No production `unwrap`, `expect`, `panic!`,
  `todo!`, `unimplemented!`, or discarded error. Assertions in tests may use narrow
  `#[expect(clippy::..., reason = "...")]` allowances; never blanket-disable lints.
- Test valid indices, loop closure/orientation, hole semantics, and manifoldness
  **where required**. Include zero-length, collinear/near-collinear, self-intersecting,
  degenerate triangles, NaN/infinity, empty input, and georeferenced-coordinate cases.
- Deterministic geometry must not depend on hash iteration or worker scheduling.
  Specify stable ordering/tie-breaks or canonical equivalence; floating reductions
  need fixed order. Rust alone does not guarantee determinism or numerical robustness.
- Every bug fix needs a regression; algorithms need unit, parity, invariant/property,
  and curated real-building fixtures with provenance. Start seeded property loops
  without new dependencies. Fuzz/property crates require explicit approval.
- Benchmark hot changes against reproducible C++/Rust baselines: same fixture,
  compiler/profile, seeds, worker count, time, allocations/copies, peak memory.
  No performance claim before measurement; do not add a benchmark framework first.

## C ABI boundary

- `ffi/` is the only possible unsafe/ABI owner; `deny(unsafe_code)` is the default.
  Current exports are `tapioca_version`, legacy geometry-only mesh/snapshot
  validators, complete detached mesh/snapshot value validators, and separate
  structural provenance/attributed-mesh validators.
  S2b also exposes the implemented affine-vector batch, matrix-chain composition
   and polyline measurement/sampling batch, plus bounded arc tessellation; no production
  geometry family is switched by its parity harness.
  Geometry algorithms still require their own implementation and tests.
  New namespaces/symbols use `tapioca` or
  `geometry`, never `EvP`/`evp` (existing directory names are not new namespaces).
  Any narrow override must explain `SAFETY:`, alignment, pointer/length/overflow
  validation, aliasing, ownership, and lifetime. `repr(C)` records, fixed-width
  integers, explicit string encodings; never Rust/C++ containers or native `bool`.
- Borrow input arrays only for the call; pair allocation and deallocation in the
  same language. Opaque handles need explicit destruction and thread-use contracts.
  Foreign pointers cannot be proven valid by a null/length check alone.
- No panic/exception crosses the ABI. Keep `panic = "unwind"`, catch Rust panics
  inside each export, map them to status/error buffers; callbacks must not throw.
  OOM/abort/UB are not caught by `catch_unwind`; do not claim otherwise.
- Cancellation uses a documented non-throwing C callback or Rust-owned handle;
  never reinterpret a C++ atomic as a Rust atomic. Bound work and thread counts;
  no detached workers. C++ retains snapshot/gate/UI/SDK lifetimes and publishes
  completed results on the authorized thread with revision checks.
- Use coarse operations; no per-vertex FFI calls. The C++ compiler's
  `/Zc:wchar_t-` is why UTF-16 must be `uint16_t`, not `wchar_t`.

## Validation

From `core/`: `python tools/quality/check_rust.py`. This runs structure/secrets,
formatting, compilation, strict Clippy, advisory complexity, debug/release tests
and doctests, and release static-library compilation. `--build` retains policy,
secrets and pin checks but compiles only the release archive for CMake; it is not
the full gate. Neither mode builds/deploys the `.apx`. Run checker regressions
and the standalone C/C++ ABI smoke test for boundary/build changes. See the guide.
