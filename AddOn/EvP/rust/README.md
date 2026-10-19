# Rust kernel foundation

The aggregate `tapioca-core-ffi` staticlib exports `tapioca_version` and shared
legacy geometry-only mesh/snapshot validators, with status/error handling and
panic containment. The add-on and offline C++ suite link it by default through a
thin `tapioca::geometry` services wrapper. Startup reads/logs ABI version 1; no
geometry consumer is switched. Separate provenance/attributed-mesh exports validate
schema-v1 structural identity, with an owned-C++ → POD → Rust validation round-trip.
These legacy validators still do not certify D14 or material catalogs. Existing
C++ geometry remains authoritative; no algorithm is migrated.

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
