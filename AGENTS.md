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
