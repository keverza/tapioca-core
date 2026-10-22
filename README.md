[Made by and Architect, not a Programmer. Decisions are mine, but code is AI.]

# Tapioca
![Tapioca logo](./docs/static/Logo.jpg)

The steamed potato sidecar for ArchiCAD needs.
Tapioca is an UI for Python automation scripts.
Tapioca exposes more AC29 C++ SDK calls and can call upon Tapir and ArchiCAD python API if installed. 
It has Diligent Render Engine for preview and viewport overlay (currently floor plan and 3D).

Check examples for possibilities.
Goal is to have design helpers and automation without leaving archicad:
Sunstudy, viewstudy, heatmap overlays, landscape design, massing feasibility ...

## Download and install

Download **Tapioca-AC29.zip** from the [Releases page](https://github.com/keverza/tapioca-core/releases)
and extract the complete `Tapioca` folder to a permanent location. Prerequisites are **64-bit Windows**, **Archicad 29**,
**PowerShell 5.1+**, the **x64 Visual C++ v14 Redistributable**, and internet access
for runtime/package downloads. With Archicad closed, run this from the extracted
`Tapioca` folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Tapioca.ps1
```

The installer downloads/verifies the managed CPython 3.12 runtime and baseline
packages under `%LOCALAPPDATA%\Tapioca\runtime`, and installs starter commands
under `%LOCALAPPDATA%\Tapioca\Commands`. See [installation and prerequisites](dist/README.md)
for repair options and optional integrations. The flat binaries in `dist\` are
build pickups, not a complete installation; use `dist\Tapioca\` or the ZIP.

Then:

1. Open Archicad 29.
2. Open Options > Add-On Manager.
3. Choose Add and select the extracted Tapioca.apx. Keep EvPPy.dll and PyPackage beside it.
4. Open Tapioca Panel from the Tapioca menu, open a project, and press Rescan.
5. Run Hello Tapioca. Tapir is optional and needed only by commands that declare it.

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
