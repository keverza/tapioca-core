# S3a.3 neutral terrain corpus

`landscape.terrain` is **hand-authored requirements data**, not a PLN/SDK capture,
Land Kit output, native acceptance, or generated expected mesh. No external data
or third-party code is embedded. It exercises the future feature-producer seam
without implementing the wall/curb/stair/path UI or patch re-evaluation.

All coordinates are local metres, right-handed Z-up. Both readers add exactly
`(500000, 6000000, 0)`, retain f64, and use absolute `1e-8 m` height-consistency
tolerance. Assertions use absolute `1e-7 m` height / `1e-7 m²` area tolerance,
not world-coordinate-relative slack. Rings close implicitly; roles are boundary
`1`, hole `2`, open breakline `3`. The format is whitespace-delimited, with full
`#` comment lines; counts precede points/constraints/line. `refinement_max_area=0`
disables refinement; positive values enable area-only refinement, budget 20000.

| Case | Independent oracle | Domain / line area (m²) |
|---|---|---|
| `wall_sides` | 3 m elevation ramp across two sides 0.2 m apart | 400 / 36 |
| `steep_curb` | 0.18 m curb over 0.15 m, area refinement; no sawtooth/bridging | 400 / 36 |
| `stair_treads` | Three 0.2 m riser ramps of width 0.1 m, with level treads | 400 / 64 |
| `crossing_paths` | Four proper side-curve crossings on z=0.1x+0.05y | 400 / 64 |
| `courtyard_island` | 20² minus 6² plus 2², excluded courtyard and kept island | 368 / 9 |
| `overlap_junction` | Overlapping breaklines and T junction, complete support, refinement | 400 / 36 |

Each case has a remote spot at x=18, beyond the property line, forcing at least
one wholly untouched triangle. Preservation assertions explicitly require that
coverage; they cannot pass vacuously on a coarse site-spanning triangulation.
Property-line z is ignored, deliberately different from most baseline heights.

`geometry/tests/terrain_corpus.rs` reads this data via `include_str!` and the
test-only safe parser in `tests/terrain_fixture.rs`; production geometry has no
filesystem/API dependency. C++ `terrain_fixture.hpp` reads the **same file** through
a source-specific CMake definition. `EvPGeomTests` checks the facade against raw
ABI geometry/support rows for both TIN and split, analytic areas and centroid
surface heights, every ordered feature-edge path and flattened edge ancestry,
closed property-line paths and inside adjacency, original-parent surface/areas,
untouched triangles verbatim, and exact repeatability/catalog retention.
Deliberately corrupted terrain and split heights must fail the independent checks.
These are not two independent terrain implementations; no C++ CDT is introduced.

## Repeatable measurement (from `core/`)

Builds and tests are separate from timing; do not run builds/tests concurrently
with the measurement driver. Use the pinned toolchain, offline and locked:

```powershell
$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"
Push-Location AddOn/EvP/rust
cargo build -p tapioca-core-ffi --example terrain_measure --release --locked --offline
Pop-Location
cmake -S AddOn/EvP/tests/cpp -B AddOn/EvP/tests/cpp/build -DTAPIOCA_LINK_RUST_GEOMETRY=ON -DTAPIOCA_BUILD_GEOMETRY_BENCHMARKS=ON
cmake --build AddOn/EvP/tests/cpp/build --config RelWithDebInfo --target TapiocaTerrainBench
python tools/benchmark_terrain.py --kernel AddOn/EvP/rust/target/release/examples/terrain_measure.exe --facade AddOn/EvP/tests/cpp/build/RelWithDebInfo/TapiocaTerrainBench.exe --cmake-cache AddOn/EvP/tests/cpp/build/CMakeCache.txt --iterations 21 --processes 3 --output terrain-measurement.json
```

Run Cargo **from `AddOn/EvP/rust/`** as shown so it discovers the pinned toolchain
and workspace-local `.cargo/config.toml`. Never fall back to downloading dependencies.
Use `Find-CMake.ps1` if CMake is not on PATH. Linux removes `.exe` and the config
subdirectory for a single-config build; Linux execution is not yet evidence.

The workload is a deterministic 100×100 survey grid, **10000 vertices, 19602
triangles (approximately 20k), 500 two-point existing-edge breakline polylines**.
It has no random seed, crossings, holes or refinement. The small corpus exercises
those semantics separately; the grid is not a worst-case crossing/refinement
latency claim. Both languages construct the same points/indices and the runner
requires exact output geometry FNV-1a digest/count equality across lanes/processes.
The report records exact binary/source SHA-256, compiler/configuration and all
raw samples, including failures of the proposed 100 ms p95 target.

Timing: one worker, three warmups, fresh result on each call; construction and
validation included, destruction/digest excluded. The facade includes both Rust
passes, explicit catalog/constraint conversion, mapping conversion and catalog
retention. There is no legacy C++ terrain baseline, CPU isolation or synchronous
latency guarantee. Upper-middle sample median, nearest-rank p95.
The Rust example's counting flag is off during timing, but the standalone
allocator delegation/flag branch remains; it is not an instrumentation-free binary.

Allocation probes run separately, untimed. The example alone installs a delegating
`System` allocator: alloc/alloc_zeroed/realloc calls, sum of requested sizes and
peak live requested sizes, excluding pre-existing input and expected result.
Each interval must return to zero; a known 4096-byte allocation sanity-checks it.
This excludes allocator overhead and transient old+new storage during a moving
reallocation. C++ measures ordinary `new/new[]` only (separately sanity-checked),
**not Rust/malloc/aligned allocation**. OS peak RSS is process-lifetime working set,
including warmups and retained baseline, **not combined per-call peak heap**.

Actual copy traffic and combined facade heap remain explicitly `null`. The copy
ledger counts only source-derived mandatory fill payload and mapping conversion,
not measured memcpy traffic, retained catalog/descriptor/internal/reallocation
copies. These limits keep **full S3a.3 acceptance open** even when the timing target
passes. Probes, parsers and measurement binaries never change the production ABI,
allocator, dependencies, editor integration or worker classification.
