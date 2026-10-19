# Rust kernel foundation

The aggregate `tapioca-core-ffi` staticlib exports only `tapioca_version`, with
status/error handling and panic-containment tests. A standalone C/C++ smoke test
links the archive; neither the add-on nor the full offline C++ suite uses it yet.
Geometry validators, attributed views and production linking are the remaining
S1 work. Existing C++ geometry remains authoritative; no algorithm is migrated.

```text
rust/
  geometry/   safe library; no dependencies or host/platform APIs
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
