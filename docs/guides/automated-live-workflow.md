# Automated live workflow: launch → Tapir → Tapioca → quit

## Run it

From the public repository root (`core/` in the private workspace), using Python
3.12:

```powershell
python -m pip install -r tools/live-requirements.txt
python tools/live_session.py --project C:\Tests\disposable.pln --fixture C:\Tests\massing.json
```

The second command performs the whole cycle. **Archicad must be closed first.**
It will not adopt or close an already-running instance.

The equivalent command verified in this workspace, without installing packages
into the user's interpreter:

```powershell
uv run --frozen --with archicad==29.3000 --with "psutil>=6,<8" python tools/live_session.py `
  --project C:\Tests\disposable.pln --fixture C:\Tests\massing.json
```

No menu clicks, palette opening or manual server startup are required.

## Prerequisites (once per machine)

- Windows, an interactive logged-in desktop, Archicad 29 and a working license.
- Tapir installed and enabled. The verified installation used Tapir 1.5.8.
- The current Tapioca package installed/enabled in Archicad. Its Graphisoft JSON
  interface must expose `Tapioca.StartServer` and the `TapiocaTest.*` bridge.
  A previous build without `StartServer` is insufficient.
- A **saved local PLN** containing the fixture GUIDs and a JSON file containing
  independently known expected metrics. Do not use a Teamwork project.
- No Archicad process and no listener on ports 19723 or 19191. The runner checks
  this before launching and fails safely if either port is occupied.
- No first-launch/license/migration/library modal requiring human action. Resolve
  those once manually. Startup timeout is a failure, not permission to click
  arbitrary dialogs or kill a process.

The default executable is
`C:\Program Files\GRAPHISOFT\Archicad 29\ARCHICAD.exe`. Override it if needed:

```powershell
python tools/live_session.py --project C:\Tests\disposable.pln --fixture C:\Tests\massing.json `
  --archicad-exe "D:\Applications\Archicad 29\ARCHICAD.exe"
```

The supplied source PLN is treated as a master: the runner copies it into a new
results directory and opens **that copy**, never the source. GUIDs survive this
file copy. Save fixture changes deliberately before running; unsaved geometry in
a previous Archicad session is not part of the source PLN.

## What the command actually tests

| Stage | Actual operation / assertion |
| --- | --- |
| Isolation | Hash source PLN, copy it and the fixture JSON into a fresh run directory |
| Launch | Start `ARCHICAD.exe <copied PLN>` without shell/file-association launching; retain its PID and creation time |
| Readiness | Wait for Archicad 29 JSON API; verify its listener belongs to that PID and its project path is the exact copied PLN |
| Tapir | Execute `TapirCommand.GetDetailsOfElements` for the fixture GUIDs and verify the saved elements are slabs |
| Server startup | Execute `Tapioca.StartServer` through Archicad JSON; this delegates to Tapioca's existing server command |
| Server verification | Verify port 19191 belongs to the same PID; read `/health` and execute `Tapioca.GetStatus` through `/evp/call` |
| Tapioca workflow | Execute `Tapioca.TestRunWorkflow` and poll `Tapioca.TestGetStatus` **through Tapioca HTTP**, not the Graphisoft command transport |
| Numeric checks | Run smoke + massing pytest; verify GUIDs, finite values, expected floor area/volume/count, row totals and identical repeat results |
| Cleanup | Reset test state, request `TapirCommand.QuitArchicad`, wait for process exit code 0 and verify both listeners have closed |
| Source protection | Re-hash the source PLN; any change fails the session |

The transport mapping is intentional:

- Tapir commands and server bootstrap use the Graphisoft JSON port (19723).
- Tapioca test operations use its native bus on port 19191, with canonical
  `Tapioca.Test*` command names. HTTP responses must have a successful bus
  envelope and real domain data; a `success`-looking error is not a pass.
- Server startup does not create a second HTTP service or a new implementation
  of the production command. The JSON adapter reuses the existing dispatcher
  and its strict schemas.

## Fixture expectations

The massing bridge currently tests **unclipped slab-prism floor accounting**:
explicit slab GUIDs, existing per-element floor-height metadata or the default
3 m floor height, no SEO or sloped edges. It does not test setbacks, façade
area, preview rendering or UI interaction.

A basic rectangular slab **20 × 10 m, 9 m thick**, without custom height
metadata, has these expectations:

```json
{
  "elementGuids": ["<actual GUID in the saved PLN>"],
  "expected": {
    "rawAreaM2": 600.0,
    "rawVolumeM3": 1800.0,
    "floorCount": 3
  },
  "absoluteTolerance": 0.000001
}
```

`rawAreaM2` is the **total counted floor area**, not one footprint. Volume is
reported in m³. Do not replace expectations with returned Tapioca values to
make a test pass; derive them from analytic geometry or independent model reads.

The existing `C:\Tests\massing.json` instead describes the user's original
16 × 48.810257057 m slab, 20 m thick: six floors, approximately
4,685.784677472 m² total floor area and 15,619.282258240 m³ volume.

If creating a fixture with Tapir, **read it back before testing**. A composite
slab can retain its composite thickness despite a requested creation thickness.
Use a Basic slab for the analytic fixture and verify its polygon and height.

## Pass/fail and artifacts

Success is process exit code **0** from `live_session.py`, with all six stages
passed, `processExitCode: 0`, `sourceUnchanged: true`, and no session errors.
A successful HTTP request alone is not enough.

Each invocation creates:

```text
test-results/session-<UTC timestamp>/
  session.json                 stage outcomes, PID, requests/responses, source hashes
  project.pln                  disposable project copy
  fixture.json                 exact assertion inputs
  tapir-elements.json          independently read element details
  native-log-*.txt             final 64 KiB of available startup/ArchViz logs
  tests/<UTC timestamp>/
    report.json                numeric results, HTTP bus envelopes, pytest phases
    junit.xml
    fixture.json
```

Add `--log <path>` to preserve other explicit native logs. Results are ignored
by git; retain a decisive report separately when using it as long-term evidence.
Old run copies can be removed manually when no process has them open. The runner
does not delete them automatically.

Timeouts can be adjusted in seconds:

```powershell
python tools/live_session.py --project C:\Tests\disposable.pln --fixture C:\Tests\massing.json `
  --startup-timeout 240 --api-timeout 5 --workflow-timeout 60 --shutdown-timeout 90
```

Short API timeouts while Archicad loads can appear in the trace even on a passing
run. The readiness stage retries read-only requests until the startup deadline;
it proceeds only after verifying the copied project.

### When it fails

- **Existing instance / occupied port:** close your own test instance deliberately
  or free the port. The runner never steals a service or touches that instance.
- **Missing capability:** rebuild/install the matched Tapioca package with
  Archicad closed. Check that Archicad actually loads the new APX, not an older
  copy elsewhere.
- **Missing GUID:** the source PLN probably lacks unsaved fixture changes, or the
  JSON references another project's element. Inspect `tapir-elements.json` and
  the failed request in `session.json`.
- **Assertion failure:** compare independent geometry and expected metrics. The
  runner still attempts a verified, graceful shutdown of its owned process.
- **Startup or shutdown timeout:** inspect the application/modal and logs. If
  ownership or project identity cannot be proven, the runner leaves the process
  alone and reports `manualCleanupMayBeRequired`. It never force-kills an app,
  saves a project to dismiss a prompt, or calls a timeout a pass.
- **Nonzero Archicad exit:** session fails even if pytest passed. No crash claim
  is inferred solely from a lost HTTP connection.

## Coding-agent edit/build/test loop

1. Verify Archicad is closed; preserve unrelated source changes.
2. Make the scoped source change and run offline checks.
3. Build with `powershell -File tools/build/Build-AddOn29.ps1` and ensure Archicad's
   registered add-on location receives that matched package. The session runner
   does **not** deploy or replace binaries.
4. Run `tools/live_session.py` with the saved fixture.
5. Read **both** `session.json` (including shutdown) and the nested numeric report.
6. Report the failing stage and evidence; never weaken expected metrics to pass.

For tests attached to an already-open instance rather than owning its lifecycle,
use [`tools/live_test.py`](live-testing.md). `--tapioca-port 19191` selects its HTTP
transport when the server is already running; omitting it uses `TapiocaTest.*`
on the Graphisoft JSON interface.

## Verification recorded on 2026-10-16

The complete command above passed against Archicad 29 build 3000 and Tapir 1.5.8:
startup/project readiness took about 32.8 s, the nested smoke/massing suite
reported **2 passed**, and graceful shutdown took about 5.4 s. Archicad exited
with code 0, both ports closed and the source PLN hash was unchanged. No menus,
mouse input, force termination, source save or manual intervention were used.

Local evidence:
`test-results/session-20261016T171639.959992Z/session.json` and
`tests/20261016T171713.182506Z/report.json` beneath that directory.

A second complete cycle also passed, with all six stages successful, clean exit
code 0 and unchanged source:
`test-results/session-20261016T172021.245563Z/session.json`.
