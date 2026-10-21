# Vendored Rust crates

Third-party Rust source used by `tapioca-geometry`, vendored by
`cargo vendor --locked --versioned-dirs vendor` and resolved only from this
directory (`.cargo/config.toml` replaces crates.io). Every build is
`--offline --locked`. Each crate directory keeps its upstream licence text and
the `.cargo-checksum.json` Cargo verifies.

The one direct dependency is `spade` (constrained Delaunay triangulation for the
terrain TIN), pinned `=2.15.1` with its default features (`std`). The table is the
complete transitive set; `tools/quality/check_rust.py` holds the same list and
fails when the lockfile, this directory or this table disagree with it.

| Crate | Version | Licence | Source | Needed by |
|---|---|---|---|---|
| spade | 2.15.1 | MIT OR Apache-2.0 | https://github.com/Stoeoef/spade | tapioca-geometry |
| robust | 1.2.0 | MIT OR Apache-2.0 | https://github.com/georust/robust | spade (exact predicates) |
| smallvec | 1.16.3 | MIT OR Apache-2.0 | https://github.com/servo/rust-smallvec | spade |
| num-traits | 0.2.19 | MIT OR Apache-2.0 | https://github.com/rust-num/num-traits | spade |
| autocfg | 1.5.1 | Apache-2.0 OR MIT | https://github.com/cuviper/autocfg | num-traits (build script) |
| hashbrown | 0.16.1 | MIT OR Apache-2.0 | https://github.com/rust-lang/hashbrown | spade (used only without `std`) |
| allocator-api2 | 0.2.21 | MIT OR Apache-2.0 | https://github.com/zakarumych/allocator-api2 | hashbrown |
| equivalent | 1.0.3 | Apache-2.0 OR MIT | https://github.com/indexmap-rs/equivalent | hashbrown |
| foldhash | 0.2.0 | Zlib | https://github.com/orlp/foldhash | hashbrown (default hasher) |

All licences are permissive and compatible with the project's GPL-3.0-or-later.
`foldhash` is Zlib rather than MIT/Apache-2.0.

Reviewed build script: `num-traits` runs `build.rs`, which uses `autocfg` to probe
the compiler for optional numeric support. No other vendored crate has one.

Not vendored by design: `faer` (lands with the S3b grading solve),
`cavalier_contours` (reference only) and any C++ CDT/Eigen fallback.

## Contract notes for the terrain code

- With `std`, spade uses `std::collections` hash maps (randomly keyed). Where it
  returns a collection gathered from one (for example a refinement result's
  excluded faces), its order differs between runs; callers sort before use.
- Constraint edges may only touch at endpoints. Crossing breaklines are split
  in `geometry::terrain` before they reach spade.
- Results are equal as triangulations (same vertices, edges and faces), not
  byte-identical across spade releases; the pin
  keeps them stable here.

## Updating

Change the pin in `geometry/Cargo.toml`, regenerate the lockfile, re-vendor, and
update this table and `THIRD_PARTY` in `tools/quality/check_rust.py` in the same
commit, with the reason.
