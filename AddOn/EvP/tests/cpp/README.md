# L2 — offline C++ tests for `Sources/AddOn/Geometry/`

The host-free tier of `docs/guides/testing.md`. **No Archicad, no DevKit, no
`.apx`.** The default Rust bridge requires Python 3.11+ and the pinned Rust/Cargo
toolchain (see `docs/guides/rust-kernel.md`). It compiles the real engine sources — never a copy — so a green run is a
statement about the code that ships.

## One-time setup

Core C++ reference entries below and the text stack listed later are provisioned
under `AddOn/reference/`, which is **gitignored**. See the catalog/provisioning
guide for the complete pinned dependency set:

| Dep | How to get it |
|---|---|
| `reference/nanort-release` | already required by the `.apx` build |
| `reference/googletest` | copy the source drop that ships inside `reference/cpp-httplib-master/test/gtest/` (headers + `src/`), or any GoogleTest release with the same `include/` + `src/` layout |

The target compiles `src/gtest-all.cc` + `src/gtest_main.cc` only — the rest of `src/` is
`#include`d by `gtest-all.cc`.

```powershell
.\Invoke-CppTests.ps1                # build + ctest
.\Invoke-CppTests.ps1 -Sanitize      # same, under MSVC AddressSanitizer
.\Invoke-CppTests.ps1 -Filter "SliceEngine.*"
.\Invoke-CppTests.ps1 -NoRust       # explicit legacy/no-Cargo build; no Rust parity claim
```

Or by hand (this is also the Linux/CI path — see `docs/guides/testing.md` §8.7):

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

`TAPIOCA_LINK_RUST_GEOMETRY` defaults ON. Explicit OFF omits the wrapper/parity
tests rather than silently succeeding without Rust. The PowerShell script sets
this option from `-NoRust` on every run; an ordinary invocation restores ON.
Every MSVC test dependency uses the add-on's `/MD` CRT/STL ABI, including Debug
and the text stack, to link the aggregate release Rust archive consistently.

## G1 mesh-preparation differential tests

`GeometryRustMeshPrep.*` calls the real Rust archive through owned services and
compares the compiled `VertexWeld`, `TriangleDegeneracy`, `WireframeEdges` and
`Aabb` oracles. Fixtures cover the flat/smooth unit cube, captured native
collinear fan coordinates from `test_triangledegeneracy.cpp`, survey placement,
empty/unused vertices, all eight wire masks, body resets, halfway normal keys,
i32 boundary keys, first-value retention and complete corner correspondence.
Seeded cases pin f32 accumulation, stable ordering and 4,000 near-roundoff
classifications; explicit nextafter values straddle the `8*DBL_EPSILON` boundary.
Comparisons are exact, not a relaxed geometry tolerance.

From `core/`:

```powershell
powershell -NoProfile -File AddOn/EvP/tests/cpp/Invoke-CppTests.ps1 -Filter 'GeometryRustMeshPrep.*:SmoothNormals.*:NormalHelpers.*:VertexWeld.*:TriangleDegeneracy.*:WireframeEdges.*'
```

The standalone Rust CMake project additionally builds real C11/C++20 record/link
smoke and malicious failure-stub tests (`TapiocaRustMeshPrepSmoke` /
`TapiocaRustMeshPrepFailure`). Rust tests cover safe refusal and full-capacity
alias/error preflight with atomic multi-buffer publication. No production
extractor is switched. This is not full capture replay, D14 authoring, native
geometry parity or performance acceptance. Measurements remain outstanding;
bulk mesh preparation stays worker-classified.

## Opt-in composition measurements (S2b)

This is a small executable and standard-library Python recorder, **not a benchmark
framework**, test-suite performance threshold, native integration test or production
consumer flip. It calls compiled `Transforms.cpp::Compose`, raw
`tapioca_compose_transforms`, and `GeometryRustAffine.cpp::ComposeTransforms` on
identical seeded finite near-identity matrices. The C++ baseline does not perform
the Rust boundary's input/refusal checks; that difference is part of the measured
call paths. Correctness checks use the existing absolute `1e-12` parity bound.

From `core/`, with CMake on PATH (use the installed Visual Studio CMake if needed):

```powershell
cmake -S AddOn/EvP/tests/cpp -B AddOn/EvP/tests/cpp/build/benchmark -G "Visual Studio 18 2026" -A x64 -T v143 -DTAPIOCA_BUILD_GEOMETRY_BENCHMARKS=ON -DTAPIOCA_LINK_RUST_GEOMETRY=ON -DEVP_SANITIZE=OFF
cmake --build AddOn/EvP/tests/cpp/build/benchmark --config RelWithDebInfo --target TapiocaCompositionBench
AddOn/EvP/tests/cpp/build/benchmark/RelWithDebInfo/TapiocaCompositionBench.exe --self-test
python tools/benchmark_composition.py --executable AddOn/EvP/tests/cpp/build/benchmark/RelWithDebInfo/TapiocaCompositionBench.exe --cmake-cache AddOn/EvP/tests/cpp/build/benchmark/CMakeCache.txt --output test-results/composition-benchmark.json
python -m pytest tools/test_benchmark_composition.py -q
```

Use a separate build tree as shown; the normal tests keep benchmarks **OFF**.
Rust OFF or sanitizer ON configurations cannot enable this target. It reuses
existing Clipper2/GLM only because the compiled transform TU references the real
`GeometryEngine.cpp::Unit`; no reference computation is copied or stubbed. The
Rust archive still follows the pinned locked/offline release CMake policy.
Linux/macOS source support exists, but only Windows x64 was locally exercised.
The runner requires Python 3.11+ and the already-installed pinned rustup toolchain
to record `rustc -vV`; it does not install tools or access Archicad.

Default measurement is nine fresh-process samples per lane/size, 256 calls per
batch, one calling worker, eight warmups and matrix counts 0/1/16/256/4096. First
lane rotates between samples. Native CLI caps chain count at 4096 and iterations
at 10000; Python caps samples at 31. Self-test covers all 15 lane/size combinations
with 128 iterations to avoid zero-duration sub-clock-tick batches. Timings are
instrumented steady-clock batch averages, not individual-call percentiles or a
production deadline. Output is consumed through a volatile checksum; every full
matrix is checked before timing and the selected path again after timing.

The report pins source commit/dirty status, executable SHA256, CMake flags/toolset,
actual C++/Rust compiler versions, machine/core count, seed, stable FNV-1a hash of
little-endian f64 fixture bits, raw samples and min/median/max. Unknown/missing,
mislabelled, non-finite, inconsistent-fixture/configuration or non-optimized
measurements refuse rather than become evidence. No CPU affinity, scheduler,
background-load or power-policy isolation is claimed.

**Measurement limits matter:**

- The executable replaces ordinary C++ `new/new[]` only in this executable and
  counts calling-thread calls/requested bytes inside the measured loop. It does
  **not** intercept Rust, `malloc`, aligned/system allocations, allocator overhead
  or other threads. The nonempty facade makes one observed ordinary-new request
  per call. MSVC large-vector padding is included in measured requested bytes,
  not silently reported as logical payload. The probe perturbs measured timing.
- The reported ABI input copy volume (`96*N` bytes per nonempty facade call) is
  a **source-derived model**, not instrumented traffic or a count of all matrix,
  descriptor/result copies. Raw ABI uses a prebuilt POD fixture outside timing;
  the owned facade performs its real conversion within timing.
- Peak RSS is the OS **whole-process lifetime high-water working set**, including
  executable startup, both fixture representations, all-lane correctness checks
  and selected-lane warmup. Before/after high-water values are not per-operation
  peak live heap, allocation totals or memory attributable to one backend.
- Composition stays **worker-classified**. This partial performance evidence
  does not close full allocation/copy/peak-heap/D3 budgets, measure other
  primitives or justify a production consumer switch.

## What is covered

| File | Engine | Anchor cases |
|---|---|---|
| `test_slice.cpp` | `SliceEngine` | mid-box cut is a unit square; tangency nudge at the base; type/guid filters |
| `test_clash.cpp` | `ClashEngine` | overlap, containment symmetry, exact gap distance, touching faces, `ClashAll` gap widening |
| `test_query.cpp` | `QueryEngine` | ray hit distance, `maxDist`, direction normalisation, pierce enter/exit, truncation keeps nearest, closest point, k-nearest, concurrent reads |
| `test_spatial.cpp` | `SpatialQueries` | box/sphere/polygon selection, touching-face convention, invalid AABB never matches |
| `test_serializer.cpp` | `MeshSerializer` | msgpack round-trip through an **independent** decoder, tag-width boundaries, truncated-buffer rejection |
| `test_render.cpp` | `RenderEngine` | image dimensions, centre-hit depth, background/foreground partition, thread count does not change the image |
| `test_geometry_rust_parity.cpp` | `tapioca::geometry` Rust wrapper | version/shape/finite/index/sidecar ingress, immutable arrays, snapshot GUIDs; not authored identity or catalog certification |

Every engine also has a **degenerate-input** case: empty mesh, single triangle,
zero-area, sliver, duplicate vertices, NaN, Inf. Those assert *defined behaviour and no
crash*, not a particular answer.

`GeometryExtractor.cpp` is deliberately not built here — it is the only file in
`Geometry/` that pulls the DevKit. It stays thin glue and is covered at L5.

## Three things to know before trusting a green run

**1. Scene text is part of the default gate.** FreeType, HarfBuzz, msdfgen and
msdf-atlas-gen are configured for every test build, and
`test_scenetextatlas`, `test_scenetextlayout` and
`test_scenetextlayoutcache` run with the rest of the offline suite. A green run
therefore covers the same pinned text stack shipped in the `.apx`.

**2. These fixtures are synthetic.** `CLAUDE.md` says never validate a geometry
algorithm on synthetic data alone, and that still holds. What is here are *analytic*
anchors (a unit box's slice perimeter is 4 because of arithmetic, not because a previous
run said so) and *degenerate* crash cases. Replaying a **real captured dump** is L3 and
is not done yet: it needs a mesh-level snapshot fixture, and this clone has none —
`dump/` is empty and `%LOCALAPPDATA%\EvP\dumps\` holds only element-detail JSON. The
decoder in `test_serializer.cpp` is the reader such a fixture would load with.

**3. One test is DISABLED because it fails.**
`QueryEngine.DISABLED_OutOfRangeIndicesDoNotReadOutOfBounds` — a mesh whose triangle
indices point past the end of its vertex array makes the nanort BVH build read out of
bounds (ASan-confirmed heap-buffer-overflow at `QueryEngine.cpp:85`). It is unreachable
today because every Snapshot is built by `GeometryExtractor` and nothing deserializes a
Snapshot back in. It becomes live the moment a snapshot is loaded from a dump. The full
reasoning and the one-line fix are in the comment above the test.

Run it with `--gtest_also_run_disabled_tests` to reproduce.

## Adding a test

Put it in the file for the engine it exercises. Use the builders in `MeshFixtures.hpp`
rather than hand-rolling vertex arrays; add a new builder there when a shape is needed
twice. Prefer an answer you can derive on paper over one recorded from a previous run —
a golden value captured from the code under test only detects change, not error.
