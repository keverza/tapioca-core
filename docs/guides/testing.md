# EvP Testing Plan

*How to test a Python-hosting C++ Archicad add-on when **you cannot run Archicad** and
the host is the biggest source of the hardest bugs. Written against this repo's actual
seams, not a generic checklist. Read `../../historical source docs/evp-command-system-plan.md` §4–5 first — this doc
extends its "offline first, in-Archicad only when forced" stance into a layered plan.*

---

## 0. The one constraint that shapes everything

**Hosted CI can never launch Archicad** — it has no headless mode (needs an interactive
desktop + OpenGL). So *cloud* CI is offline-only, and the generic advice "run an in-host
integration suite in CI" does not apply as written. **But** a Windows box with AC29 + a
license *can* run in-host tests automatically, driven by an external process — that is L5/§8.
The plan therefore uses a two-tier strategy:

- **Push as much as physically possible below the ACAPI line** (L1–L4) so it runs in plain
  pytest / a `cl.exe` harness with no host, on any runner including hosted CI.
- **Automate the in-host layer separately** on a self-hosted/interactive Windows session
  (§8), and make each host run *also* pay for itself by capturing a **dump fixture** that
  turns the observation into a permanent offline regression test — so a bug seen once in the
  host never needs the host to catch it again.

This is already the repo's culture (`DumpTestCase`, probe commands, `test_*.py` next to the
domain math). The plan below names the layers, says what is missing, and puts numbers on
the gaps.

---

## 1. The layers (and where this repo actually stands)

| Layer | What it covers | Runs where | Status today |
|---|---|---|---|
| **L1 Pure Python** | domain math, `_scanner`, `transaction` batching, `_env`, `paths` | pytest, no host | **strong** — 7 `test_*.py` suites + `test_env.py` |
| **L2 Pure C++** | `Geometry/` engines (slice/clash/query/serialize/render/spatial) | offline `cl.exe`/GoogleTest harness | **BUILT 2026-07-26** — `tests/cpp/`, 69 tests, green plain and under ASan. One DISABLED test records a real ASan finding (§2.1) |
| **L3 Dump-replay** | port algorithms against real captured cases | pytest over `DumpTestCase` JSON/`.npz` | **partial** — fixtures exist, not all ported cmds covered |
| **L4 ABI boundary** | `EvP.apx ↔ EvPPy.dll` `extern "C"` contract | offline two-binary link test | **untested in isolation** |
| **L5 In-Archicad** | gate, undo/redo, lifecycle, palette, save/load, real ACAPI writes | **automated external driver** over EvP HTTP + Archicad JSON, on a Windows box with AC29 + license (§8); probe commands for the visual residue | **manual today**; can be automated — the two channels + smoke scripts already exist |
| **L6 Static/sanitizer/CI** | clang-tidy, `/W4`, ASan/UBSan on L2 | CI on push | **NONE — no CI, no sanitizer build** |

**Two testable channels already exist** for L5 (this is what makes in-host automation possible
without a human clicking) — see §8:
- **Archicad JSON API** (per-instance port, main thread): reaches real ACAPI, including writes.
- **EvP HTTP** (`127.0.0.1:19191`, worker threads): reads, `/render`, `/screenshot`.

The generic review's items 1–2 (split logic out, unit-test pure C++) are **the single
highest-value missing piece here** because L2 is real, sizeable (~1.5 kLOC of pure
compute), and completely untested. Start there.

---

## 2. L2 — the biggest win: offline C++ unit tests for `Geometry/`

**Finding that makes this cheap:** every engine in `Sources/AddOn/Geometry/` is **already
ACAPI-free**. Verified — zero `ACAPI_*` calls, zero DevKit includes:

```
SliceEngine.cpp  ClashEngine.cpp  QueryEngine.cpp
MeshSerializer.cpp  RenderEngine.cpp  SpatialQueries.cpp
```

They depend only on `<vector>`/`<cmath>`/… + `nanort.h` + local `Mesh.hpp`. Only
`GeometryExtractor.cpp` pulls the DevKit (`Model.hpp`, `ACAPinc.h`) — that stays the thin
glue and is **not** unit-tested at L2.

This is the split the generic review's step 1 asks for — **it is already done in the source
tree**; nobody has pointed a test framework at it. Action:

1. **Framework: doctest or GoogleTest**, a *separate* CMake target (`EvPGeomTests`) that
   compiles the real `Geometry/*.cpp` (never a copy) plus `Mesh.hpp`, linking only nanort.
   No DevKit, no Python, no `.apx`. It builds and runs on the CI box **and on Linux** (see
   §5) precisely because it is host-free.
2. **What to assert** (from the review's list, made concrete for these engines):
   - **Boundary/degenerate meshes:** empty mesh, single triangle, zero-area/sliver
     triangle, duplicate verts, NaN/Inf coordinate → no crash, defined result.
   - **Slice correctness (`SliceEngine`):** plane above/below/through a known box → expected
     section polygon; plane exactly coplanar with a face (the classic off-by-epsilon).
   - **Clash (`ClashEngine`):** touching-but-not-overlapping AABBs, fully-contained,
     shared-face, far-apart → correct boolean; large N for the broad phase.
   - **Ray/query (`QueryEngine`/`SpatialQueries` over nanort):** ray misses, grazes an
     edge, hits back-face, originates inside → stable hit/no-hit.
   - **Serialize round-trip (`MeshSerializer`):** `serialize → deserialize == identity`
     (the review's serialization item); truncated/garbage buffer → clean failure, no OOB
     read.
3. **Fixtures, not synthetic-only:** the standing rule (`CLAUDE.md`) is *validate geometry
   against real dumps*. Feed these tests the `.npz`/JSON meshes from `DumpTestCase`
   (roof_geometry_dump, apartment_testcase) via a tiny loader, **plus** a few hand-built
   degenerate meshes for the boundary cases a real dump won't contain.

Definition of done for L2: a `EvPGeomTests` target that `ctest` runs green offline, one
test file per engine, at least the degenerate-input case per engine.

### 2.1 Status — BUILT 2026-07-26

`AddOn/EvP/tests/cpp/` (see its `README.md`). **69 tests, green plain and under MSVC
ASan.** Definition of done above is met: one file per engine, degenerate-input case per
engine, serialize round-trip.

Shape, where it differs from the sketch above:

- **A standalone CMake project**, not a target in the add-on's `CMakeLists.txt` — that
  one hard-fails without the DevKit, which would defeat the whole point. `Invoke-CppTests.ps1`
  wraps configure/build/ctest; `-Sanitize` switches to the ASan build.
- **GoogleTest, vendored** to `AddOn/reference/googletest/` (copied from the full source
  drop already sitting in cpp-httplib's test folder — no download, no submodule).
- **Serialize round-trip goes through an INDEPENDENT decoder** hand-written from the
  msgpack spec in `test_serializer.cpp`, not through msgpack-c and not through a mirror
  of the writer. A round-trip that shares an implementation with the writer only proves
  self-consistency; this one can fail when the writer emits something a real msgpack
  decoder cannot read — which is the failure that would actually reach the Python client.
- **The suite was mutation-checked**, not just observed to pass: deliberately breaking
  the slice nudge, the clearance distance and the mesh map arity each failed exactly the
  tests that should have caught them. An all-green first run of tests written against
  unseen code is not evidence on its own.
- **ASan on MSVC needs two things nobody warns you about:** the `/fsanitize=address` flag
  must be on *every* target in the link including gtest (STL container annotations change
  the ABI — otherwise `LNK2038 annotate_string`), and `clang_rt.asan_dynamic-x86_64.dll`
  is not on PATH, so the exe dies with `0xC0000135` before `main()` and looks like a
  crash. `Invoke-CppTests.ps1 -Sanitize` handles both.

**Two honest limits on a green run:**

1. **The fixtures are synthetic.** The standing rule (never validate geometry on
   synthetic data alone) is *not* satisfied by this layer and was never going to be —
   L2 gives analytic anchors and degenerate crash cases. Real-dump replay is L3 (§3) and
   is **still open**: it needs a mesh-level snapshot fixture, and there is none in the
   repo (`dump/` is empty; `%LOCALAPPDATA%\EvP\dumps\` holds only element-detail JSON).
   Capturing one snapshot dump is the single highest-value next step for this layer, and
   `test_serializer.cpp`'s decoder is already the reader it would load with.
2. **ASan found a real out-of-bounds read**, recorded as a DISABLED test:
   `QueryEngine.DISABLED_OutOfRangeIndicesDoNotReadOutOfBounds`. A Mesh whose triangle
   indices point past its vertex array makes the nanort BVH build read off the end
   (`QueryEngine.cpp:85` → `nanort.h:939`). Unreachable today — every Snapshot is built
   by `GeometryExtractor`, and nothing deserializes a Snapshot back in — but it becomes
   live the moment a snapshot is loaded from a dump, i.e. exactly when item 1 gets done.
   Do the bounds check over `faces` in the QueryEngine constructor before, or with, the
   dump-replay work.

---

## 3. L1/L3 — keep the Python discipline, close the coverage gaps

Already the strong layer. Rules to hold and gaps to fill:

- **Every ported command's domain math gets a `test_*.py` beside it**, run against a
  **real dump**, not synthetic input (synthetic already missed a shipped bug here — see
  `CLAUDE.md`). Cross-check current ports against fixtures: ensure MassingFeasibility,
  ApartmentGraph, NumberingAlongPolyline, StorySliceOverlay, SelectionOutline each replay a
  captured case, not just constructed geometry.
- **`_scanner` is the palette's gatekeeper** — it decides whether a command folder even
  loads. Keep the offline scan (`python -c "... _scanner.scan_file(...)"` from `CLAUDE.md`)
  as a **pre-sync gate** and mirror it in CI so a malformed `command.py` can never reach the
  palette.
- **`transaction.py` batching + rollback semantics** are pure Python on the compose side —
  unit-test that N writes collapse to one envelope and that an `ok:false` step marks the
  batch for rollback. The *replay* half lives in C++ (`ApiDispatcher.cpp:150-227`) and is an
  L5 concern (§4).
- **`paths.py`** — the one place output location is decided. Test the three buckets
  (log/output/dump), rotation cap, and stamp-uniqueness of dumps, so the `dump`/`output`
  drift that `CLAUDE.md` warns about can't regress.

---

## 4. L4/L5 — the parts that genuinely need the host, and how to make them cheap

These map to the review's items 3, 6, 10 — the lifetime/ownership/re-entrancy bugs unit
tests can't reach. They can't run in *hosted* CI, but with a Windows box that has AC29 + a
license they **can be driven automatically** by an external test process (§8 is the full
harness spec). The pieces below split into: things the external driver asserts on
automatically (writes, undo, lifecycle over the two channels), and the visual residue that
stays a probe with a human "NOW LOOK".

**L4 — the ABI boundary (`EvPPyApi.h`).** The `.apx ↔ EvPPy.dll` seam is `extern "C"` +
POD + `uint16_t*`, with mismatched `/Zc:wchar_t` on the two sides — a textbook place for
silent memory corruption. Build a **tiny offline harness** that links a stub `.apx`-side
caller against the real `EvPPy.dll` export surface and round-trips wide strings and PODs
across the boundary. This needs no Archicad and catches marshaling/ownership breakage the
moment `EvPPyApi.h` changes. (Do **not** widen the ABI to make it testable — test it as-is.)

**L5 — in-Archicad, driven from outside.** Each of these is a `pytest` case in the §8
harness that calls the live add-on over the two channels and asserts on returned JSON; the
add-on side can stay a probe command that the test invokes via `/evp/call`:

- **Gate & threading (review item 10, re-entrancy/callback order):** a `GateStressProbe`
  that fires many `Invoke`/`Post` jobs, mixes reads (no gate) with writes, and asserts none
  captured-by-reference survives a timeout (the documented use-after-free). The external
  driver can hammer it concurrently from several client threads. This is the closest we get
  to TSan for the gate — see §5 on why real TSan can't reach it.
- **Undo/redo & transaction replay (review items 3, 10):** an `UndoReplayProbe` that runs a
  batched write, invokes host undo, redo, and re-runs — asserting the model returns to each
  known state and that a mid-batch `ok:false` rolls the **whole** batch back
  (`ApiDispatcher.cpp:193`). Fully assertable over the API: read element GUIDs/counts before
  and after each step. This is the only place transaction *replay* is exercised.
- **Lifecycle & stress (review items 3, 6):** a `LifecycleProbe` the driver calls in a loop
  of **hundreds/thousands** of iterations while polling `/health` memory (the lifecycle
  scripts already measure held bytes), plus a scripted project open→save→close→switch cycle.
  Long-running host + repeated ops is where leaks surface; the repeat count is a test param.
- **The visual residue stays human.** Where the assertion is "which way does the arrow point"
  or "is the ridge line right", the driver captures `/render`/`/screenshot` PNGs for a
  golden-image compare where stable, and otherwise ends in the **"NOW LOOK / REPORT" block**
  (`CLAUDE.md`). Everything logs to `%LOCALAPPDATA%\EvP\logs\`, never trapped in an alert.

**The dump-back rule (turns L5 into L3):** whenever a probe surfaces a real case, capture it
with `DumpTestCase` so the next regression check runs offline. Every in-host bug becomes an
offline fixture — this is how the review's "automate regression testing" (item 8) actually
works when you can't automate the host.

---

## 5. L6 — sanitizers, static analysis, CI (and where the generic advice breaks)

**Correction to the generic review:** its sanitizer/Valgrind guidance assumes a Linux build
of the whole add-on. **You cannot link the Archicad DevKit or `.apx` on Linux**, and the
gate/undo/host bugs live *above* the ACAPI line — so LSan/Valgrind-on-the-whole-plugin and
"TSan the threading" are **not available for the host-coupled code**. What *is* available,
and worth doing:

- **ASan/UBSan on the L2 `EvPGeomTests` target.** Because `Geometry/` is host-free, compile
  that target with `-fsanitize=address,undefined` (clang/gcc on Linux, **or** MSVC
  `/fsanitize=address`) and run it in CI. This gives real buffer-overflow/UAF/UB coverage on
  the numeric core — the code most likely to do raw pointer/index math over nanort buffers.
  **TSan** only pays off if an engine is internally multithreaded (`QueryEngine` uses
  `<thread>`/`<mutex>`) — target it there, not the gate.
- **The gate's thread-safety is *not* reachable by TSan here** (it's ACAPI-coupled, Windows,
  no host in CI). It is covered by the L5 `GateStressProbe` + the by-value-capture rule
  reviewed at read time. Say this plainly rather than pretending a sanitizer covers it.
- **Static analysis that runs with no host:** `clang-tidy` and `cppcheck` over
  `Geometry/` + `EvPPy/` (skip the DevKit-heavy translation units where headers won't
  parse offline). Add MSVC `/W4 /permissive-` on the real build; treat new warnings as work,
  not noise (review item 5) — but introduce `-Werror`/`/WX` *incrementally*, scoped to
  `Geometry/` first, so the existing DevKit-driven warnings don't block every build.
- **Release-like builds (review item 7):** the shipped config is already `RelWithDebInfo`
  (`Build-AddOn29.ps1`) — good; keep at least one CI build in an optimized config so lifetime/
  timing bugs that hide in Debug surface.

**CI, from zero.** There is no CI today. Minimum viable pipeline, all host-free:

1. `pytest` on L1/L3 (Python domain math + dumps).
2. `_scanner` scan-gate over every `Commands/*/command.py`.
3. Build + `ctest` the L2 `EvPGeomTests` target, once plain and once ASan/UBSan.
4. `clang-tidy`/`cppcheck` on `Geometry/` + `EvPPy/`.

The full `.apx` build stays on the user's Windows box against the DevKit; CI validates
everything that doesn't need it. Note the environment mismatch honestly: CI Linux can run
1–4; step 3's MSVC-ASan variant needs a Windows runner if you want the shipped toolchain
exercised.

---

## 6. Priority order (do these in sequence)

1. ~~**L2 GoogleTest target for `Geometry/`**~~ — **DONE 2026-07-26** (§2.1).
2. **ASan/UBSan in a minimal CI job** — the target already builds and runs clean under
   MSVC ASan locally (`Invoke-CppTests.ps1 -Sanitize`); what is missing is CI to run it on
   push, plus the Linux UBSan variant. No CI exists yet, so this is now the top item.
3. **Capture one mesh-level snapshot dump and close the L3 gap** — promoted, because L2
   turned out to be synthetic-only for want of a fixture (§2.1 limit 1), and because the
   same work makes the recorded out-of-bounds finding reachable (§2.1 limit 2). Then a
   real-dump replay test for every ported command's math.
4. **Stand up the L5 live-driver harness (§8)** on the Windows/AC29 box — launcher +
   port-discovery + version-guard fixture, then port the existing `historical source legacy-scripts/03 Tapir + GeometryServer/
   test_*.py` smoke scripts to pytest asserts. `GateStressProbe`, `UndoReplayProbe`,
   `LifecycleProbe` are the add-on-side commands it calls.
5. **L4 ABI round-trip harness** for `EvPPyApi.h`.
6. **clang-tidy/`/W4` gate**, scoped to `Geometry/`/`EvPPy/` first, expanding outward.

Every fixed bug becomes a test at the lowest layer that can hold it (review item 8): a
geometry bug → L2/L3 fixture; a marshaling bug → L4; a gate/undo bug → an L5 probe **plus** a
`DumpTestCase` capture so the next check is offline.

---

## 7. What the generic review got right vs. what changes here

| Review item | Verdict for EvP |
|---|---|
| 1 Split logic out of host | **Already done** — `Geometry/` is host-free; act on it. |
| 2 Unit-test pure C++ | **Closed 2026-07-26** — 69 tests in `tests/cpp/`, green plain + ASan. §2.1. |
| 3 In-host integration tests | **Automatable off-CI** — external driver over the two channels on a licensed Windows/AC29 box; §8. Hosted CI still can't. |
| 4 Sanitizers everywhere | **Only on the host-free L2 core.** DevKit won't link on Linux. §5. |
| 5 Static analysis | Yes, **scoped** — DevKit TUs won't parse offline. §5. |
| 6 Stress/lifetime | **Automated loop** via the §8 driver (`LifecycleProbe` × N + `/health` memory), not just a manual run. §4/§8. |
| 7 Release-like builds | Already `RelWithDebInfo`; keep an optimized CI config. §5. |
| 8 Automate regression | **Via dump-back** — every host observation → offline fixture. §4/§6. |
| 9 Logging/asserts/crash evidence | Already the house rule: log to file, "NOW LOOK" block. §4. |
| 10 Plugin-specific (lifetime/undo/re-entrancy) | **The real risk**, covered by L4 + L5 probes, not sanitizers. §4. |

The review's closing principle — *for a C++ add-on the main risk is lifetime/ownership, not
logic* — is correct, but here it splits cleanly by layer: **logic risk lives in `Geometry/`
and is now cheaply testable (L2)**; **lifetime/ownership risk lives at the ABI boundary and
in the host callbacks and is only reachable by the ABI harness (L4) and probes (L5)**. Fund
both; don't let the easy L2 win make you think the hard L4/L5 risks are covered.

---

## 8. Live-driver harness — automated in-host ACAPI tests (Windows + AC29 + license)

With a Windows machine that has the DevKit, AC29, and a license, L5 stops being manual: an
external `pytest` process drives the **running** add-on and asserts on real ACAPI results.
No human clicks; no hosted CI (Archicad has **no headless mode** — it needs an interactive
desktop + OpenGL, so this runs on a self-hosted/interactive Windows session, not a cloud
runner).

### 8.1 Why this is possible here — two channels already built

| Channel | Endpoint | Thread | What it drives |
|---|---|---|---|
| **Control plane** | Archicad JSON port (per instance) | **main thread** (`ScheduleForExecutionOnMainThread`) | real ACAPI — writes, snapshot build, status. `test_addoncommand.py` proves it dispatches on the main thread while a script runs. |
| **Data plane** | `POST http://127.0.0.1:19191/evp/call` + `GET /snapshot /mesh /query/* /ray/* /clash /render /screenshot/*` | worker threads | the whole `evp.api.call(...)` bus + geometry reads + pixels |

`/evp/call` is a **generic bus call** (`HttpServer.cpp:152`) — anything a command can invoke,
the test can invoke, including native writes that go through the gate + transaction. The
existing `historical source legacy-scripts/03 Tapir + GeometryServer/test_*.py` are already this style of test; they need only
pytest asserts and a launcher wrapper.

### 8.2 No hot-reload — the iteration loop this implies

Archicad **does not hot-reload the .apx**. Plan the loop around that:

- **C++ change:** `Build-AddOn29.ps1` → **restart Archicad** to load the new `EvP.apx` → re-run the
  §8 suite. (The harness's launch-or-attach step, 8.3, handles the restart.)
- **Python command change:** `Sync-Commands.ps1` → **Rescan** in the palette (no restart). Most
  test iteration is here — algorithms and command logic are Python.

So the native-restart cost is paid **once per C++ build**, and the harness amortizes it by
running the whole suite against that one loaded binary.

### 8.3 Coexisting with your AC27 sessions — no VM, no second machine

You run AC27 and AC29 **side by side on the same machine and the same license**. They cannot
collide on the automation channel, and the isolation is already coded:

- **Per-instance ports.** `geomclient.py`: `AC_PORT_RANGE = range(19723, 19743)` — *"Archicad
  assigns one port per running instance."* AC27 and AC29 never share a JSON port.
- **AC27 has no EvP.** The harness finds its AC29 target by probing that range for the one
  instance where `_has_addon(port)` returns a status (EvP loaded). AC27 is structurally
  invisible. EvP's `127.0.0.1:19191` HTTP port likewise exists only in the AC29 process.
- **Version guard (add this).** Before any test runs, query the target's product/version over
  the JSON API and **assert major version == 29** (verify the exact command name against the
  local `archicad` package — do not take it from Tapir docs, per `CLAUDE.md`). A mis-set port
  then can never drive AC27; the suite aborts instead.

The only residual cross-talk is **desktop focus**, not data: a test that raises a screenshot
window or an alert could steal focus while you work in AC27. Two isolation levels:

1. **Same desktop, target by port** — fine for read/write API tests that don't pop UI. You may
   see AC29 flicker; your AC27 model is never touched.
2. **Second Windows interactive user session** (Fast User Switching / RDP into a second
   logged-in user) — the AC29 automation gets its own desktop and **cannot** steal focus from
   your AC27 session. The license stays on the physical machine, so none of the VM
   license-binding pain. **Recommended** if focus-stealing during a run would interrupt you.

A dedicated VM gives the cleanest isolation but re-introduces the machine-bound-license
problem you flagged; the second-session route gives the same isolation without it.

### 8.4 What the run does — automatic vs. manual

**Automatic (the harness):**
1. **Launch-or-attach.** Start `Archicad.exe` (AC29) with a fixture `.pln`, or attach to a
   running one. Poll the JSON port until it answers = ready-gate. (On a fresh C++ build this
   is where the required restart happens.)
2. **Discover + guard.** Probe `19723–19742` for the EvP-bearing instance; assert version 29.
3. **Run tests.** pytest calls control plane (writes/undo) + data plane (reads/render);
   asserts on returned JSON and on element GUIDs/counts before/after.
4. **Pixels.** Pull `/render` / `/screenshot` PNGs; golden-image compare where the view is
   stable.
5. **Teardown/reset.** Undo the batch (writes are one undo step each) or close-without-save,
   so each test starts from the fixture state. Repeat lifecycle probes N× while reading
   `/health` memory.

**Manual (unavoidable):**
- Rebuild + **restart Archicad** for native C++ changes (no hot-reload).
- One-time: fixture `test.pln` authoring, second-session setup (and license activation only
  if a test needs persistence — see §8.5; the read/create/delete/undo suite doesn't).
- **Human eyeball** on visual output too subtle for a golden image ("NOW LOOK").
- Blessing a captured case as the known-good golden before it's frozen.

### 8.5 Licensing — a prepared `test.pln` + never-save likely needs **no license**

The precise DEMO restriction is on **persistence and output**, not on modeling: DEMO blocks
**save, export/publish, print, teamwork, and clipboard copy-out** — it does **not** block
in-memory editing or the API (you can draw walls and place objects in DEMO). Our L5 workflow
is exactly the unblocked half:

- **Open a pre-authored `test.pln`** (a small fixture project committed once) — opening is
  fine in DEMO; you just can't save *back* to it, which we never do.
- **read → create → delete → undo**, all in-memory operations that the transaction/undo
  system runs without touching disk.
- **Never save.** Teardown is undo or close-without-save, so nothing ever needs to persist.

So the whole read/create/delete/undo suite should run **DEMO, license-free** — and that
removes the license-contention worry with your AC27 work entirely (a DEMO AC29 instance
holds no seat). **One thing to confirm empirically on first setup** (per `CLAUDE.md` — never
assert unverified host behaviour): run a single `create → read-back → delete` probe in a DEMO
session and check the create actually executes. It is expected to, because interactive
modeling works in DEMO; if some ACAPI create path turns out to be gated, fall back to the
license for *that* test only.

You still need the real license **only** for tests that must genuinely persist — save/export/
publish/teamwork — and the port workflow here deliberately has none. If you do run licensed
(e.g. AC29 test instance shares your working license), just make sure it's the session the
harness drives, especially in the two-session setup.

**Does "send geometry to the Python worker" count as a blocked export?** No — the DEMO gate
lives in Archicad's own file-output pipelines (save `.pln`, DWG/IFC/DXF/PDF export, print,
publish, teamwork, clipboard copy-out), not in an add-on reading the model and moving bytes.
EvP's core path splits into three parts, none of them a gated export:

1. **Extraction** (`GeometryExtractor` / snapshot) is an in-memory **model read** — allowed in
   DEMO, same category as the query reads.
2. **Serialize → worker** is either an in-process copy to embedded CPython or msgpack over
   **EvP's own `127.0.0.1:19191` server** — the add-on's own socket, entirely outside
   Archicad's license gate. DEMO has no hook into it and cannot block it. There is no
   per-read "you exported model data" check; the gate is only in the save/publish/print code.
3. **The worker computes in Python**, outside Archicad — not a licensing concern.

So the extract→worker→compute core runs **DEMO, license-free.** The only real unknown is not
the data flow but the **control channel**: EvP's control-plane commands ride Archicad's
registered JSON API, and whether the *external* JSON port is exposed in DEMO is Graphisoft
behaviour this doc can't assert. Two escape hatches make it moot for the geometry path:
- **Embedded / palette-run** commands need no external API, so the full extract→worker path
  works in DEMO regardless.
- Only the **external §8 driver** depends on the JSON port. Verify once: run `GetStatus` on
  the JSON port in a DEMO session; if it answers, external driving works license-free too; if
  not, drive those tests from the embedded palette path (or license just the external-channel
  tests). Pair this with the create-probe above — one DEMO session settles both.

### 8.6 First moves

1. A `conftest.py` fixture: launch-or-attach → discover port → assert v29 → yield a client
   bound to that instance + `19191`.
2. Port `test_write_commands.py` / `test_lifecycle.py` / `test_mainthread.py` /
   `test_addoncommand.py` from print/PASS-FAIL to pytest asserts against that fixture.
3. Add `GateStressProbe` / `UndoReplayProbe` / `LifecycleProbe` as `Diagnostics` commands the
   suite invokes via `/evp/call`, each capturing a `DumpTestCase` on interesting failures so
   the regression check drops back down to offline L3.

### 8.7 Which coding-agent environment can run which layer

Where the agent's *shell* runs decides what it can reach. EvP's HTTP server binds
**`127.0.0.1`** (`HttpServer.hpp:21`) — loopback-only, not `0.0.0.0` — so only a process on
the Windows loopback can reach the live add-on.

| Layer | Agent in **native Windows** VS Code | Agent in **WSL** (repo-only, Linux) |
|---|---|---|
| L1 Python pytest | ✅ | ✅ |
| L2 `Geometry/` C++ tests | ✅ (`cl.exe`) | ✅ (gcc/clang — the engines are ACAPI-free) |
| L2 sanitizers | ✅ MSVC `/fsanitize=address` | ✅ ASan/UBSan/TSan (Linux) |
| L3 dump-replay | ✅ | ✅ |
| L4 ABI harness | ✅ | ⚠️ builds/links only with MSVC; skip |
| **`.apx` build** (`Build-AddOn29.ps1`) | ✅ MSVC + DevKit | ❌ needs Windows MSVC + DevKit |
| **L5 in-host** (launch/drive Archicad) | ✅ shares OS + loopback + desktop | ❌ separate net namespace; can't reach `127.0.0.1:19191` or the JSON port, can't launch Archicad |

- **Native-Windows agent = the whole stack.** It can `Build-AddOn29.ps1`, launch/restart
  `Archicad.exe`, hit `127.0.0.1:19191` + the JSON port, and run §8 end-to-end. Costs: it
  needs a real interactive desktop session (no headless), and restarting Archicad to load a
  rebuilt `.apx` disrupts whatever you have open.
- **WSL agent = the host-free tier, and the *better* place for it** — L1–L3 plus real Linux
  sanitizers on the geometry core, fast, no Windows. It simply cannot do the `.apx` build or
  L5. This is the same split as "hosted CI vs. self-hosted Windows session": let the WSL
  agent own L1–L3 continuously, the Windows agent (or you) own the build + L5.
- **Do not** make the WSL agent reach Archicad by binding EvP to `0.0.0.0` + opening the
  firewall to WSL — that exposes the command bus (including writes) beyond loopback. Keep it
  `127.0.0.1`; the Windows agent owns L5.
