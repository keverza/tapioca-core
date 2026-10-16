# Live testing bootstrap (Archicad 29)

For the complete **launch → Tapir → Tapioca server → numeric tests → quit** loop,
see [Automated live workflow](automated-live-workflow.md). This page describes the
attach-only `tools/live_test.py` runner, which never controls application lifecycle.

Run from the public repository root (`core/` in the private workspace):

```powershell
python -m pip install -r tools/live-requirements.txt
python tools/live_test.py --suite smoke --port 19723
```

Use a **dedicated** Archicad 29 instance with Tapir installed and a disposable
local PLN already open. This bootstrap never opens, saves, closes or modifies a
project, never changes selection and never replaces a loaded APX. Without
`--port`, discovery refuses multiple instances rather than picking the first.

For connectivity before the bridge is installed:

```powershell
python tools/live_test.py --suite smoke --port 19723 --connect-only
```

This checks Archicad/Tapir and reports missing Tapioca capabilities. It is **not**
a Tapioca bridge pass. The normal smoke suite fails when any bridge command is
missing. Add `--expected-project C:\Tests\disposable.pln` to enforce exact project
identity; otherwise smoke only reports it. Untitled/Teamwork projects are refused
when enforcing identity.

## Build/install

With Archicad closed, use the existing build entry point:

```powershell
powershell -File tools/build/Build-AddOn29.ps1
```

Install the matched package using the existing deployment procedure, then start
only the dedicated test instance. The bridge is registered during `Initialize()`
and does not require opening the palette or ImGui. `Ping.buildHash` records the
configured source revision (with a dirty suffix for local changes); it is not an
APX content digest. Reconfigure through the build entry point after edits.

## First numeric test

Prepare a disposable PLN with one rectangular **20 × 10 m slab, 9 m thick**,
vertical sides, no SEO and no custom Tapioca height metadata (default floor
height: 3 m). Its bottom may be at any known elevation. No current selection is
needed. Copy `AddOn/EvP/tests/live/fixtures/massing.example.json` outside the
repository and replace its **example GUID** with the slab's real GUID. Do not
change expected metrics to match a failing response: derive them from the known
fixture. The three floors total 600 m² and 1,800 m³.

```powershell
python tools/live_test.py --suite massing --port 19723 `
  --expected-project C:\Tests\disposable.pln --fixture C:\Tests\massing.json
```

The test runs twice, verifies source GUIDs, finite numeric results, expected
areas/volumes/floor count, row totals and identical repeat results. The runner
checks the exact open PLN before and after each call. No master `.pln` is shipped
or fabricated by the bootstrap. Automatic disposable project creation is a later
phase.

### Bridge contract

All six commands are registered on the Graphisoft JSON API as `TapiocaTest.*`.
Internally they use the existing strict registry as `Tapioca.TestPing`,
`Tapioca.TestRunWorkflow`, etc.; this is not a second production dispatcher.

| Command | MVP behavior |
| --- | --- |
| `Ping` | Version, configured revision, protocol version, readiness, workflows |
| `RunWorkflow` | Read-only `massing_feasibility`, explicit 1–8 slab GUIDs, `mode: preview`, integer `seed` |
| `GetStatus` | Poll job ID for running/completed/failed state, timing and numeric rows |
| `GetDiagnostics` | Worker drain state, retained job count, gate counters and scope |
| `Reset` | Discard test job and prevent late publication; never clears production state |
| `Capture` | Explicit `supported: false` with reason; never treated as screenshot evidence |

The production `massingslices::Build` calculator is shared with the UI. This MVP
copies polygon geometry, stories and existing per-element metadata on the main
thread through `MainThreadGate`, then calculates on an owned async worker.
Results use `basis: unclipped_slab_prisms`: **no setbacks, allowed envelope,
façade area, terrain, preview publication or model mutation**. Sloped edges,
SEO, non-slabs and oversized inputs are refused rather than approximated.
Seed is accepted for protocol consistency but this calculation is non-random.
This tests production floor accounting, not the full operated-body/UI pipeline.

Only one job is retained. A new run replaces a completed job; concurrent runs
are refused. Reset/project replacement discards results immediately, but a pure
calculation already running drains in the background; check `workerBusy` before
retrying. Shutdown joins that worker before unloading. Source element stamp
changes invalidate results. Nothing subscribes to mouse or ImGui events.

## Results and limits

Every invocation creates `test-results/<UTC timestamp>/report.json` and
`junit.xml`. JSON includes health/build metadata, individual test phases,
requests/responses, timings and failure text. Pass `--log <native log path>`
(repeatable) to retain its final 64 KiB. `--api-timeout` and `--workflow-timeout`
are finite positive seconds. A native error response is never accepted as a
domain result.

API timeout/disconnection, workflow failure, analysis timeout and assertion
failure remain distinct. Without an owned process, disconnection cannot establish
whether Archicad crashed. Screenshots and UI correctness are not claimed.
Offline `pytest -q` skips these live tests; runner unit tests remain offline.
No live success should be recorded until the real host returns numeric metrics.

Next: live-verify this slab, then add operated-body snapshots and deterministic
Tapir fixture creation. Build/restart automation must establish process/project
ownership before it is allowed to close anything.
