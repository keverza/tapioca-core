# L2 — offline C++ tests for `Sources/AddOn/Geometry/`

The host-free tier of `docs/guides/testing.md`. **No Archicad, no DevKit, no Python, no
`.apx`.** It compiles the real engine sources — never a copy — so a green run is a
statement about the code that ships.

## One-time setup

Two dependencies, both under `AddOn/reference/`, which is **gitignored** — a fresh clone
has neither:

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
```

Or by hand (this is also the Linux/CI path — see `docs/guides/testing.md` §8.7):

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## What is covered

| File | Engine | Anchor cases |
|---|---|---|
| `test_slice.cpp` | `SliceEngine` | mid-box cut is a unit square; tangency nudge at the base; type/guid filters |
| `test_clash.cpp` | `ClashEngine` | overlap, containment symmetry, exact gap distance, touching faces, `ClashAll` gap widening |
| `test_query.cpp` | `QueryEngine` | ray hit distance, `maxDist`, direction normalisation, pierce enter/exit, truncation keeps nearest, closest point, k-nearest, concurrent reads |
| `test_spatial.cpp` | `SpatialQueries` | box/sphere/polygon selection, touching-face convention, invalid AABB never matches |
| `test_serializer.cpp` | `MeshSerializer` | msgpack round-trip through an **independent** decoder, tag-width boundaries, truncated-buffer rejection |
| `test_render.cpp` | `RenderEngine` | image dimensions, centre-hit depth, background/foreground partition, thread count does not change the image |

Every engine also has a **degenerate-input** case: empty mesh, single triangle,
zero-area, sliver, duplicate vertices, NaN, Inf. Those assert *defined behaviour and no
crash*, not a particular answer.

`GeometryExtractor.cpp` is deliberately not built here — it is the only file in
`Geometry/` that pulls the DevKit. It stays thin glue and is covered at L5.

## Three things to know before trusting a green run

**1. The scene-text tests do not run by default.** `EVP_TEXT_RENDERING` is `OFF`
while that feature is half-finished, so FreeType, HarfBuzz, msdfgen and
msdf-atlas-gen are not configured and `test_scenetextatlas`,
`test_scenetextlayout` and `test_scenetextlayoutcache` are not built. The
configure step says so. This keeps an in-progress feature from turning the gate
red for everyone else — the node-graph tests have nothing to do with text — but
it does mean a green run says nothing about scene text. Picking that work back
up starts with:

```powershell
.\Invoke-CppTests.ps1   # after: cmake -S . -B build -DEVP_TEXT_RENDERING=ON
```

The `.apx` build is unaffected and still builds the text stack.

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
