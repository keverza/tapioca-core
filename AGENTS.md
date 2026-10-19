# AGENTS.md

Tapioca is an Archicad 29 add-on with a native C++ host and a shipped Python
command package. Keep changes small, preserve the existing seams, and validate
the public command roots before syncing them.

## Naming and comments

- C++ types and functions use PascalCase.
- C++ variables, parameters, and members use camelCase.
- File-scope C++ statics use `s_` plus camelCase.
- Python follows PEP 8.
- Comments explain contracts, units, SDK gotchas, or rejected alternatives, not
  obvious assignments.

## Native boundaries

Rust-only work lives in `AddOn/EvP/rust/`. Read its `AGENTS.md` and
`docs/guides/rust-kernel.md`; its version-only foundation is production-unlinked,
not a generic migration mandate. New namespaces use `tapioca` or `geometry`,
never `EvP`/`evp`; existing compatibility paths/types need not be renamed.
Run `python tools/quality/check_rust.py` for Rust changes. Rust uses idiomatic
Rust naming, not the C++ naming convention above.

- ACAPI is main-thread-only; use `MainThreadGate`.
- Gate lambdas capture by value.
- Native commands do not open undo scopes.
- `EvPPyApi.h` is the only cross-binary ABI contract: extern-C POD values and
  UTF-16 buffers, never C++ types.
- Success payloads contain domain data, not transport `ok` or `error` fields.
- New native command registrations declare strict input and response schemas.

## Command workflow

1. Add a command under `Examples/` or a component command source root.
2. Use `import tapioca` for new public commands.
3. Run `python tools/quality/check_python.py`.
4. Run `python AddOn/EvP/tests/dryrun_command.py <folder>`.
5. Run `AddOn/EvP/Sync-Commands.ps1` in native Windows PowerShell and press
   Rescan in the palette.

The scanner is a safety gate: a command it cannot parse silently disappears
from the palette. Keep annotation arguments literal and do not place UI code in
Python.

## Automated live validation (opt-in)

Read `docs/guides/automated-live-workflow.md` before controlling Archicad.
With Archicad closed and saved fixture inputs prepared, run:

```powershell
python tools/live_session.py --project <source.pln> --fixture <expected-metrics.json>
```

Reuse disposable files in `C:\Tests` directly; do not create a new PLN per run.
For a protected master, pass `--project-copy <one reusable disposable.pln>`.
Per-run directories should contain reports/JSON/logs, not project copies.
The runner owns its launched PID, tests the disposable PLN through Tapir and
Tapioca HTTP, and quits only that verified process. Never bypass its existing-instance
guard, force-kill an application, or replace expected metrics with observed
results. Check the session report's shutdown and source-hash assertions as well
as the nested pytest report. Use `tools/live_test.py` for attach-only testing.
