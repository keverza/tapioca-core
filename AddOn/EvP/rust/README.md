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
C++ geometry remains authoritative; no algorithm is migrated.

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
