# Public Script Layout

The public core keeps scripts that build, validate, or demonstrate Tapioca. The
private development workspace may add command and probe roots through the optional
`AddOn/EvP/command-sync.local.json` overlay; a public clone does not need that file.

## Commands

- `Examples/` contains small command folders intended for learning and smoke tests.
- `AddOn/EvP/Diagnostics/Probes/PlanAnchorProbe/` is the canonical probe shape.
- `AddOn/EvP/Sync-Commands.ps1` composes the public roots and writes the palette files.

Run `python tools/quality/check_python.py --scan-only` before syncing. Then run
`AddOn/EvP/tests/dryrun_command.py <command-folder>` for a command with suitable
defaults and press Rescan in the Tapioca palette.

## Build And Tests

- `tools/build/` contains the AC29 build, doctor, and reference provisioner.
- `tools/quality/` contains the architecture, structure, secret, and command gates.
- `AddOn/EvP/tests/` contains the Python contract tests and the standalone C++ suite.
- `AddOn/EvP/tools/` contains add-on-local schema, API, log, and shader tools.

The C++ add-on requires Windows PowerShell, Visual Studio, the AC29 DevKit, and
the catalog-listed reference dependencies. The standalone C++ suite does not need
Archicad or the DevKit beyond its own catalog-listed test dependencies.
