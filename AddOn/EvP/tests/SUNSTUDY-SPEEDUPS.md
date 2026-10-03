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

## Deferred live acceptance

Compare `sun-gpu-step` CPU checks, GPU compute, wait, fallback reasons, and wall time
on the same large model/settings. Verify cancellation never publishes a partial day.
The existing 53 missing solid meshes must be investigated before claiming complete
model accuracy; these optimisations do not waive extraction diagnostics.
