# Tapioca

Tapioca is a Python command host for Archicad 29. It keeps Archicad integration
and safe main-thread SDK access in a native add-on while letting command authors
write ordinary Python workflows.

[![Build](https://github.com/keverza/tapioca-core/actions/workflows/addon_build_check.yml/badge.svg)](https://github.com/keverza/tapioca-core/actions/workflows/addon_build_check.yml)

## Download and install

Prebuilt AC29 releases are published as `Tapioca_AC29_Win.apx` on the Releases
page. Download the asset and keep a checkout of this repository for the runtime
installer and example commands. With Archicad closed, run these commands from
the repository root in Windows PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\AddOn\EvP\Install-Runtime.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\AddOn\EvP\Sync-Commands.ps1
```

`Install-Runtime.ps1` installs the bundled CPython runtime and baseline packages
under `%LOCALAPPDATA%\Tapioca\runtime`. `Sync-Commands.ps1` copies commands from
the repository's `Examples\` folder to `Documents\Tapioca Commands`, normally
`%USERPROFILE%\Documents\Tapioca Commands`, which is the folder Archicad scans.
Do not copy the example folders into `AddOn\EvP\Commands` or load them directly
from the repository.

Then:

1. Open Archicad 29.
2. Open Options > Add-On Manager.
3. Choose Add, select `Tapioca_AC29_Win.apx`, and confirm the load.
4. Open the Tapioca palette from the Window menu.
5. Press Rescan after adding or changing a command folder.

## First command

```python
import tapioca

@tapioca.command(title="Hello", category="Examples")
def run(name: tapioca.Text = "Archicad"):
    tapioca.ui.text("Hello, " + name)
```

Save it as `Examples\HelloCommand\command.py` (or another command folder), run
`AddOn\EvP\Sync-Commands.ps1` from the repository root, and press Rescan. The
`Examples\` directory contains small runnable patterns for inputs, selection
reads, result tables, and built-in properties.

## Build from source

The build is Windows-only and requires Visual Studio with C++ tools. A clean
checkout can provision the declared dependencies and build AC29 with:

```powershell
powershell -File tools/build/provision-reference.ps1 -FromUpstream
powershell -File tools/build/Build-AddOn29.ps1
```

See `docs/architecture/commands/SPEC.md` and `docs/architecture/api/SPEC.md`
for the command and API contracts. Offline tests are documented in
`docs/guides/testing.md`.

## License

Tapioca is GPL-3.0-or-later. Third-party notices are in `NOTICE`.
