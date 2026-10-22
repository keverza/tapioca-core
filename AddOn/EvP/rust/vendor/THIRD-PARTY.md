# Vendored Rust crates

Third-party Rust source used by `tapioca-geometry`, vendored by
`cargo vendor --locked --versioned-dirs vendor` and resolved only from this
directory (`.cargo/config.toml` replaces crates.io). Every build is
`--offline --locked`. Each crate directory keeps its upstream licence text and
the `.cargo-checksum.json` Cargo verifies.

There are two direct dependencies:

- `spade` (S3a.0: constrained Delaunay triangulation for the terrain TIN), pinned
  `=2.15.1` with its default features (`std`);
- `faer` (S3b.0: sparse Cholesky for the grading solve), pinned `=0.24.4` with
  **no default features** and only `sparse-linalg`. That excludes `rayon`
  (threads), `rand`, `npy` and `std` (and with it `private-gemm-x86`), so the
  factorization is sequential and its maths is `libm`'s.

The table is the complete transitive set; `tools/quality/check_rust.py` holds the
same list and fails when the lockfile, this directory or this table disagree with
it. `equator`/`equator-macro` are legitimately present at two versions (`faer`
uses 0.6, `nano-gemm` 0.2).

| Crate | Version | Licence | Source | Needed by |
|---|---|---|---|---|
| spade | 2.15.1 | MIT OR Apache-2.0 | https://github.com/Stoeoef/spade | tapioca-geometry |
| robust | 1.2.0 | MIT OR Apache-2.0 | https://github.com/georust/robust | spade (exact predicates) |
| smallvec | 1.16.3 | MIT OR Apache-2.0 | https://github.com/servo/rust-smallvec | spade |
| num-traits | 0.2.19 | MIT OR Apache-2.0 | https://github.com/rust-num/num-traits | spade, faer, faer-traits, num-complex, qd |
| autocfg | 1.5.1 | Apache-2.0 OR MIT | https://github.com/cuviper/autocfg | num-traits (build script) |
| hashbrown | 0.16.1 | MIT OR Apache-2.0 | https://github.com/rust-lang/hashbrown | spade (used only without `std`) |
| allocator-api2 | 0.2.21 | MIT OR Apache-2.0 | https://github.com/zakarumych/allocator-api2 | hashbrown |
| equivalent | 1.0.3 | Apache-2.0 OR MIT | https://github.com/indexmap-rs/equivalent | hashbrown |
| foldhash | 0.2.0 | Zlib | https://github.com/orlp/foldhash | hashbrown (default hasher) |
| faer | 0.24.4 | MIT | https://codeberg.org/sarah-quinones/faer | tapioca-geometry |
| faer-traits | 0.24.0 | MIT | https://codeberg.org/sarah-quinones/faer | faer |
| bitflags | 2.13.2 | MIT OR Apache-2.0 | https://github.com/bitflags/bitflags | raw-cpuid |
| bytemuck | 1.25.2 | Zlib OR Apache-2.0 OR MIT | https://github.com/Lokathor/bytemuck | dyn-stack, faer, faer-traits, gemm-common, num-complex, pulp, qd |
| cfg-if | 1.0.5 | MIT OR Apache-2.0 | https://github.com/rust-lang/cfg-if | pulp |
| dyn-stack | 0.13.2 | MIT | https://codeberg.org/sarah-quinones/dyn-stack | faer, faer-traits, gemm, gemm-common and its kernels |
| dyn-stack-macros | 0.1.3 | MIT | https://github.com/kitegi/dynstack/ | dyn-stack (proc-macro) |
| equator | 0.2.2 | MIT | https://github.com/sarah-ek/equator/ | nano-gemm |
| equator | 0.6.0 | MIT | https://github.com/sarah-ek/equator/ | faer |
| equator-macro | 0.2.1 | MIT | https://github.com/sarah-ek/equator/ | equator 0.2 (proc-macro) |
| equator-macro | 0.6.0 | MIT | https://github.com/sarah-ek/equator/ | equator 0.6 (proc-macro) |
| gemm | 0.19.0 | MIT | https://github.com/sarah-ek/gemm/ | faer (dense supernodal blocks) |
| gemm-c32 | 0.19.0 | MIT | https://github.com/sarah-ek/gemm/ | gemm |
| gemm-c64 | 0.19.0 | MIT | https://github.com/sarah-ek/gemm/ | gemm |
| gemm-common | 0.19.0 | MIT | https://github.com/sarah-ek/gemm/ | gemm and its kernels |
| gemm-f32 | 0.19.0 | MIT | https://github.com/sarah-ek/gemm/ | gemm |
| gemm-f64 | 0.19.0 | MIT | https://github.com/sarah-ek/gemm/ | gemm |
| generativity | 1.2.1 | MIT OR Apache-2.0 | https://github.com/CAD97/generativity | faer, faer-traits |
| libm | 0.2.16 | MIT | https://github.com/rust-lang/compiler-builtins | faer, faer-traits, gemm-common, pulp, qd |
| nano-gemm | 0.2.2 | MIT | https://github.com/sarah-ek/nano-gemm/ | faer (small dense kernels) |
| nano-gemm-c32 | 0.2.1 | MIT | https://github.com/sarah-ek/nano-gemm/ | nano-gemm |
| nano-gemm-c64 | 0.2.1 | MIT | https://github.com/sarah-ek/nano-gemm/ | nano-gemm |
| nano-gemm-codegen | 0.2.1 | MIT | https://github.com/sarah-ek/nano-gemm/ | nano-gemm kernels (build scripts) |
| nano-gemm-core | 0.2.1 | MIT | https://github.com/sarah-ek/nano-gemm/ | nano-gemm and its kernels |
| nano-gemm-f32 | 0.2.1 | MIT | https://github.com/sarah-ek/nano-gemm/ | nano-gemm |
| nano-gemm-f64 | 0.2.1 | MIT | https://github.com/sarah-ek/nano-gemm/ | nano-gemm |
| num-complex | 0.4.6 | MIT OR Apache-2.0 | https://github.com/rust-num/num-complex | faer, faer-traits, gemm, nano-gemm, pulp |
| once_cell | 1.21.4 | MIT OR Apache-2.0 | https://github.com/matklad/once_cell | gemm-common |
| paste | 1.0.15 | MIT OR Apache-2.0 | https://github.com/dtolnay/paste | gemm, gemm-common, pulp (proc-macro) |
| proc-macro2 | 1.0.107 | MIT OR Apache-2.0 | https://github.com/dtolnay/proc-macro2 | equator-macro, quote, syn |
| pulp | 0.22.3 | MIT | https://github.com/sarah-quinones/pulp/ | faer, faer-traits, gemm-common, qd (SIMD dispatch) |
| pulp-wasm-simd-flag | 0.1.1 | MIT | https://github.com/sarah-quinones/pulp/ | pulp |
| qd | 0.8.0 | MIT | https://github.com/sarah-quinones/qd/ | faer-traits |
| quote | 1.0.47 | MIT OR Apache-2.0 | https://github.com/dtolnay/quote | equator-macro, syn |
| raw-cpuid | 11.6.0 | MIT | https://github.com/gz/rust-cpuid | gemm, gemm-common, pulp (CPU feature detection without `std`) |
| reborrow | 0.5.5 | MIT | https://github.com/sarah-ek/reborrow/ | faer, faer-traits, pulp |
| seq-macro | 0.3.6 | MIT OR Apache-2.0 | https://github.com/dtolnay/seq-macro | gemm, gemm-common (proc-macro) |
| syn | 2.0.119 | MIT OR Apache-2.0 | https://github.com/dtolnay/syn | equator-macro |
| unicode-ident | 1.0.27 | (MIT OR Apache-2.0) AND Unicode-3.0 | https://github.com/dtolnay/unicode-ident | proc-macro2, syn |
| version_check | 0.9.5 | MIT/Apache-2.0 | https://github.com/SergioBenitez/version_check | pulp (build-time) |

All licences are permissive and compatible with the project's GPL-3.0-or-later.
`foldhash` is Zlib; `bytemuck` offers Zlib among its alternatives; `unicode-ident`
additionally carries the Unicode-3.0 licence for its generated tables (it is a
build-time dependency of the proc-macros and is not linked into the archive).

**Embedded notices in `faer`.** Its manifest licence is MIT, but it ports code from
other projects and ships their notices, declared in `EMBEDDED_NOTICES`:
`COPYING.SUITE_SPARSE.AMD.BSD` (BSD-3-Clause; the AMD fill-reducing ordering the
grading solve's Cholesky uses), `COPYING.SUITE_SPARSE.COLAMD.BSD` (BSD-3-Clause;
sparse LU/QR), `COPYING.LAPACK.BSD` (BSD-3-Clause; dense rotations, pivoted QR,
Schur) and `COPYING.EIGEN.MPL2` (MPL-2.0; divide-and-conquer SVD, not used by
Tapioca). BSD-3-Clause and MPL-2.0 are compatible with GPL-3.0-or-later; MPL-2.0's
file-level terms apply to those ported files.

**Supplied licence texts.** These published crates omit the MIT text their licence
requires to accompany copies, so `LICENSE` was supplied from the same upstream
repository (outside Cargo's checksums, which only cover published files):
`faer` and `faer-traits` from codeberg.org/sarah-quinones/faer at tag
`faer-v0.24.4`; `equator` 0.2.2 and `equator-macro` 0.2.1 from `equator` 0.6.0;
`pulp-wasm-simd-flag` from `pulp` 0.22.3; and the seven `nano-gemm*` crates from
github.com/sarah-ek/nano-gemm `main` (the repository publishes no tags).
`SUPPLIED_LICENCES` in `check_rust.py` lists them.

Reviewed build scripts (each runs on the build machine, so each is named in
`BUILD_SCRIPTS`): `num-traits` probes the compiler through `autocfg`; `libm` sets
cfgs from the environment; `paste`, `proc-macro2` and `quote` run
`rustc --version`; `pulp` and `nano-gemm-{c32,c64,f32,f64}` write generated
kernels to `OUT_DIR`. None reaches the network or writes outside `OUT_DIR`.

Reviewed proc-macros (`PROC_MACROS`): `dyn-stack-macros`, `equator-macro` (both
versions), `paste` and `seq-macro`. They expand code at compile time only.

Not vendored by design: `cavalier_contours` (reference only) and any C++
CDT/Eigen fallback.

## Contract notes for the terrain code

- With `std`, spade uses `std::collections` hash maps (randomly keyed). Where it
  returns a collection gathered from one (for example a refinement result's
  excluded faces), its order differs between runs; callers sort before use.
- Constraint edges may only touch at endpoints. Crossing breaklines are split
  in `geometry::terrain` before they reach spade.
- Results are equal as triangulations (same vertices, edges and faces), not
  byte-identical across spade releases; the pin
  keeps them stable here.

## Contract notes for the grading code

- `faer` supplies the sparse factorization only. `geometry::grading` owns the
  objective, the constraint elimination, the active set and every refusal.
- `SparseColMat::sp_cholesky` uses `faer`'s AMD fill-reducing ordering, which is
  deterministic for a fixed sparsity pattern; without the `rayon` feature the
  global parallelism is sequential. Triplets are assembled in a fixed order.
- `pulp` selects SIMD kernels at run time from the CPU's features, so results
  are bit-reproducible on one machine, not guaranteed across CPU generations.

## Updating

Change the pin in `geometry/Cargo.toml`, regenerate the lockfile, re-vendor, and
update this table and `THIRD_PARTY` in `tools/quality/check_rust.py` in the same
commit, with the reason. `cargo vendor` deletes this file and the supplied
`LICENSE` files; restore them before committing (the checker fails until you do).
