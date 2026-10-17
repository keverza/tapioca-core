# Rust kernel scaffold

No geometry implementations, C exports, C++ wrappers, or runtime integration exist
here. Existing C++ remains authoritative. This workspace only establishes build,
dependency, lint, ownership, and test rules for future approved kernel tasks.

```text
rust/
  geometry/   safe library; no dependencies or host/platform APIs
  ffi/        staticlib + rlib scaffold; depends only on geometry; no exports
```

One-way ownership is deliberate: algorithms cannot import the boundary. Only one
aggregate staticlib will ever be linked; the rlib exists for Rust tests/doctests.
Do not create unused math/terrain/topology modules to fill out a proposed layout.

From `core/`:

```powershell
powershell -NoProfile -File tools/build/Setup-Rust.ps1 -InstallRustup
python tools/quality/check_rust.py
python -m pytest tools/quality/tests/test_check_rust.py -q
```

Rustup installation is explicit, hash-verified, and does not modify the global
default or user PATH. The toolchain is pinned by `rust-toolchain.toml`. Re-run
setup without `-InstallRustup` on machines already using Rust.

See [coding rules](AGENTS.md) and the [build/contracts guide](../../../docs/guides/rust-kernel.md).
