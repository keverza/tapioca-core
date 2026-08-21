# Dev Machine Setup

This guide contains only setup and host actions that `tools/build/doctor.ps1` cannot
perform. The doctor is the authoritative offline preflight for tools, references,
governance, architecture checks, tests, and the command dry-run.

The public product name is **Tapioca**. The binary and repository paths remain `EvP`.

## 1. Install manually

Install these system components before running the doctor:

- Visual Studio 2022 or 2026 with the **MSVC v143 x64/x86** toolset.
- **C++ ATL for v143 build tools (x86 & x64)**.
- Archicad 29, including any license or interactive desktop access needed for live tests.
- Python 3.12 recommended, with the repository development packages:

```powershell
python -m pip install -r dev-requirements.txt
```

Node.js/npm and CMake are also required by the add-on build. Install them if the
doctor reports either one missing. Do not install the ARM-only v143 component: it
does not provide the x64 CRT required by this build.

## 2. Obtain the ignored reference tree

`AddOn/reference/` is not in Git. The usual machine-move procedure is to copy it from
the old development machine, or obtain the same catalog-listed source trees from their
authorities:

```powershell
robocopy "\\OLDPC\...\Archicad\AddOn\reference" "D:\...\Archicad\AddOn\reference" /E /MT:16
```

If the source is kept elsewhere, provision it with the catalog-driven copier:

```powershell
.\tools\build\provision-reference.ps1 -SourceRoot "D:\reference" -Profile all
```

The doctor validates the destination, but it cannot download, locate, or copy a
multi-gigabyte reference bundle. The build profile needs the AC29 DevKit, CPython
3.12 headers/import library, cpp-httplib, msgpack-c, nanort, DiligentEngine, and the
connector's bundled AC29 SDK. The offline C++ tests additionally need GoogleTest.

## 3. Provision the embedded runtime

The runtime is separate from the system Python and is loaded by the add-on from
`%LOCALAPPDATA%\Tapioca\runtime`:

```powershell
cd AddOn\EvP
.\Install-Runtime.ps1
```

Run this with Archicad closed. The doctor only reports whether `python312.dll` is
available; it never downloads, deletes, or repairs the runtime.

## 4. First native load

Run the doctor from the repository root and fix every failure it reports:

```powershell
.\tools\build\doctor.ps1
```

Then perform the host actions manually:

```powershell
.\tools\build\Build-AddOn29.ps1
.\AddOn\EvP\Sync-Commands.ps1
```

Add `AddOn\EvP\build_29\EvP.apx` through Archicad 29's Add-On Manager, restart
Archicad after native changes, and press **Rescan** in the Tapioca palette after
Python command changes. The build must run while Archicad is closed because a loaded
add-on can lock the output files.

## 5. Manual evidence

The following cannot be established by an offline doctor:

- Archicad accepts and loads the rebuilt `.apx` in the intended installation.
- A live command performs the expected SDK read or write in a real project.
- Palette layout, viewport rendering, screenshots, and other visual results are correct.
- A live run is safe to repeat, can be undone, and leaves no unwanted project residue.
- A user has the required Archicad license, project fixture, and desktop session.

Record live or visual evidence in the owning task. Keep logs and raw dumps under
`%LOCALAPPDATA%\Tapioca\`; do not commit runtime state, build trees, or arbitrary
captures.

## 6. Carry over optional state

- `AddOn\reference\`: copy or provision it as described above.
- `%LOCALAPPDATA%\Tapioca\palette.json`: copy only if the saved palette layout and
  parameter values matter; otherwise the palette recreates defaults.
- `%LOCALAPPDATA%\Tapioca\runtime`: do not copy; provision it on the new machine.
- `%LOCALAPPDATA%\Tapioca\Commands\`: do not copy; sync regenerates it from the repository.
- `%LOCALAPPDATA%\Tapioca\logs`, `output`, and `dumps`: copy only a named fixture or
  evidence case that is needed for the current task.

## Troubleshooting

- **Reference validation fails**: compare the missing path with
  `AddOn\reference\CATALOG.yaml`, then copy or provision the catalog-listed source.
- **v143 or ATL validation fails**: add the x64/x86 components in Visual Studio
  Installer, not the ARM variant.
- **The runtime is missing**: run `AddOn\EvP\Install-Runtime.ps1` with Archicad
  closed; do not delete it as a first repair step.
- **A native change is not visible**: rebuild, restart Archicad, and reload the `.apx`.
- **A Python command is not visible**: run `AddOn\EvP\Sync-Commands.ps1`, then press
  Rescan. The repository folders are the source of truth.
- **Live visual or SDK behavior is wrong**: keep the doctor output, command log, and
  a curated dump or probe result with the task; do not treat a green offline report as
  host verification.
