# Tapioca
![Tapioca logo](./docs/static/Logo.jpg)

Tapioca is an UI for Python automation scripts.
Tapioca exposes more AC29 C++ SDK calls and can call upon Tapir and ArchiCAD python API if installed. 

[![Build](https://github.com/keverza/tapioca-core/actions/workflows/addon_build_check.yml/badge.svg)](https://github.com/keverza/tapioca-core/actions/workflows/addon_build_check.yml)

## Download and install

Prebuilt AC29 Tapioca.apx is on Releases page. Download the asset and clone this repository for the python runtime installer and example commands. With Archicad closed, run these commands from the repository root in Windows PowerShell:

```powershell
 ./dist\Install-Runtime.ps1
 ./AddOn\EvP\Sync-Commands.ps1
```

`Install-Runtime.ps1` installs the bundled CPython runtime and baseline packages
under `%LOCALAPPDATA%\Tapioca\runtime`. `Sync-Commands.ps1` copies commands from
the repository's `Examples\` folder to `%LOCALAPPDATA%\Tapioca\Commands`, which is
the folder Tapioca scans to show scripts.

Then:

1. Open Archicad 29.
2. Open Options > Add-On Manager.
3. Choose Add, select Tapir.apx and Tapioca.apx.
4. Open the Tapioca palette from the Window menu.
5. Press Rescan after add changes from a command folder.

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
