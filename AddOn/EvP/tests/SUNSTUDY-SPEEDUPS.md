# Sun-study speedups (2026-10-03)

Five independently committed changes. Live Archicad acceptance and large-model
timings are deliberately deferred; offline timings are not production speed claims.
Analysis remains the model minus Context/Ignored, with Late filtering glass faces.

## 1. GPU fallback and readback

- Three bounded, independent 16,384-ray slots overlap dispatch/readback and CPU
  resolution, instead of waiting after each 4,096-ray dispatch.
- Only the active result range is copied. CPU checks compact into parallel packets;
  fallback/validation overlap traces once, not twice.
- Full first-4,096 validation per direction and sparse later checks remain.
  Every ambiguous/work-budget ray still uses exact CPU resolution; any mismatch
  disables the backend and replays the whole timestep. No precision downgrade.
- Telemetry distinguishes arithmetic ambiguity, work-limit exhaustion, unique CPU
  checks, bytes read, and in-flight packets. Work budgets/tolerances are unchanged.
- Validation: shader ABI, every-ray/binary-bit parity, georeferencing, boundaries,
  ring wrap/tails, concurrency, cancellation, and immutable context-buffer reuse.
  The offline D3D adapter reports Microsoft Basic Render Driver, not the live RTX.

## 2. One calculation lane and explicit backend

- `StartSunStudy` accepts `backend=cpu|gpu` (CPU default); both paths use the same
  session, role-filtered occluders, exact GPU fallback, and store advance method.
  Follower adoption preserves the requested backend rather than changing it.
- A native manual start cancels/disarms the old automatic producer but retains
  its visible overlay. This no longer depends on a Python client calling Pause.
- A shared cancellable lane prevents concurrent manual/automatic calculations
  from competing; progress/cancellation never waits on the store lock. Admission
  wait is reported separately from calculation time.
- Completed records supply immutable grid/atlas reuse sources. Manual benchmarks
  deliberately do not seed results: CPU/GPU baseline runs still trace a fresh day.
- Offline tests cover queued cancellation, nonblocking progress, exception/revision
  safety, completed-record selection and backend/schema/adoption seams.

## 3. Shared and incremental geometry capture

- The viewer retains the original double meshes from its existing bounded host
  slices and assembles a completed immutable snapshot on the worker. No second
  host tessellation is needed for sun-study reruns.
- One monotonic model-watch stamp covers edits, visibility/sweep changes and watch
  epochs. A superseded/incomplete/cancelled pass never publishes a usable capture.
- Small updates splice changed/new meshes and remove absent/hidden ones. Missing
  revisions, selection/filtered bases or transparency-pool changes force a full
  sliced pass, not a guessed merge. Material classification stays capture-owned.
- The follower requests/waits for this shared capture without blocking the host;
  `BuildSnapshot(reuseShared=true)` adopts it cheaply. Legacy explicit BuildSnapshot
  remains a fresh synchronous baseline by default. It does not magically slice
  Archicad's indivisible model-generation call.
- Snapshot IDs share one allocator. Filtered copies receive distinct IDs and cannot
  alias the full snapshot's query cache. Viewer capture does not overwrite a manual
  selection/filtered snapshot or its metadata as a side effect.
- Offline gates cover assembly, immutability, creation/deletion, missing revisions,
  material renumbering, partial/cancelled passes, ownership and native slicing seams.

## 4. Persistent allocations and region-only GPU uploads

- Replacement studies copy compatible patch allocation state. Triangle studies
  use the same allocator with canonical GUID/local-face keys (ambiguous identities
  fall back to baseline packing). Surviving tiles keep addresses; removals leave
  holes, and growth explicitly rebuilds. No sunlight values are trusted from keys.
- Exact changed row runs, including retired texels, are merged into rectangles for
  hours and all step-bit layers. Fragmentation falls back to one plane update.
- Renderer updates require the exact retained immutable image base, not just a
  study name/dimension match. Missed commands, resize and viewer restart use the
  carried full image. Unchanged side maps keep their GPU buffers.
- Allocation happens before visible replacement; failed textures keep the previous
  overlay. Applied base images are released, rather than kept for the viewer lifetime.
- Offline tests cover reorder/removal/creation, ambiguous identities, growth,
  random exact reconstruction, shadow bits, stale bases and changed side mappings.

## 5. Worker display assembly and compact results

- Manual `ShowSunStudy` queues pure display work and returns `preparing=true` when
  accepted; it no longer scatters hours, packs step bits or builds side maps on the
  host thread. `SunStudyOverlayState` reports `preparing`, `pendingStudyId` and
  `preparationError`; renderer attachment remains the final acknowledgement.
- The follower uses the same producer on its task worker. Completion rechecks
  session/run/record/snapshot identity before enqueue and only then accepts the
  replacement. Hide, cancellation, edits and viewer teardown cannot resurrect it.
- Manual publication also rechecks the original place inputs, live role binding
  and model capture stamp. Enqueue is serialized with record cancellation, not a
  revision read followed by an unguarded push. No worker calls the host SDK.
- A worker-held session lease protects even partial-day display reads without
  holding the store lock. Progress and cancellation remain nonblocking; no worker
  waits on the host. Teardown joins workers only on quit/unload.
- Immutable hours/step images and per-element maps are shared across display-only
  changes. Cache keys include resolved steps as well as session generation, so a
  partial day cannot masquerade as its later result. No sample arrays are copied
  to construct display payloads.
- `GetSunStudyResults(summaryOnly=true)` returns count/min/mean/max/daylight and
  fully lit/shaded counts with an empty `hours` array, no sample/atlas transfers.
  Conflicting bulk options refuse. Existing full results remain available for
  accuracy comparisons; `includePositions=false` now also avoids native copies.
- Offline tests compare both cached display domains to legacy images/every step
  bit, exercise reuse/invalidation, stable ownership, cancelled leases and schemas.
- The smoke client exposes the CPU/GPU choice (CPU default), requests compatible
  shared capture, and reads a compact summary by default. `fresh_snapshot` retains
  the host-capture baseline; `detailed_results` retains raw sample/atlas checks.
  It waits on small overlay-state packets for renderer attachment, not for bulk
  arrays. The fake wire exercises delayed preparation and hide-before-completion.

## Final offline validation

- **2,001 C++ tests passed**, including GPU parity, both atlas domains, selective
  reuse, texture regions, worker/display leases and cancellation. Two existing
  tests remain disabled. D3D tests used Microsoft Basic Render Driver, not the
  live RTX 4070 Ti; these results are correctness evidence, not speed claims.
- **104 focused Python tests passed** across seams, fake-wire behavior, strict
  schemas/catalog generation, preset metadata, scanners and the private smoke
  client. Full core pytest: **646 passed, 6 skipped, 2 pre-existing failures**.
  The Dynamo template target assertion and preview mouse-routing source assertion
  both reproduce at the pre-speedup commit `e41ed81`; no unrelated fix is included.
- C++ architecture/formatting, Python quality/scanners, native schema checks,
  structure and secret checks passed. The stale native catalog count is corrected
  for the already-registered `PauseSunStudyFollowing` command.
  The shared `dryrun_command.py` harness retains its same five baseline Ruff
  findings (imports and two unrelated semicolon lines); it was not mass-formatted.
- CPU/compact, GPU/Late/patch/compact, and fresh-capture/detailed smoke dry runs
  each completed twice, with real input-schema validation and delayed display
  preparation. They are fake-wire checks, not Archicad acceptance.
- The final **RelWithDebInfo `EvPAddOn` build succeeded** after explicit CMake
  regeneration. Output: `AddOn/EvP/build_29/EvP.apx`. It has not been deployed.
- Evidence lives under `C:/Users/Dever/AppData/Local/Temp/opencode/`:
  `sun-speedups-final-cpp-tests.log`, `sun-speedups-final-python-tests.log`,
  `sun-speedups-smoke-*-dryrun.log`, and `sun-speedups-final-native-build.log`.

## Commit sequence

1. `9f0ba8c` — GPU packets/readback/exact CPU checks.
2. `4383501` — shared calculation admission and explicit CPU/GPU choice.
3. `5fc4fb8` — shared revision-compatible sliced capture.
4. `80a4346` — stable atlas allocations and exact texture-region uploads.
5. This commit — worker-side cached display and compact result summaries.

The private smoke command/handoff and parent repository's core pointer are tracked
in a separate integration commit because `core` is a Git submodule. No deployment,
push, or live run is part of this offline implementation.

## Deferred live acceptance

Compare `sun-gpu-step` CPU checks, GPU compute, wait, fallback reasons, and wall time
on the same large model/settings. Verify cancellation never publishes a partial day.
The existing 53 missing solid meshes must be investigated before claiming complete
model accuracy; these optimisations do not waive extraction diagnostics.
