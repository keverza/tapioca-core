# Tapioca for Archicad 29 — installation

## Required prerequisites

- **64-bit Windows** supported by Archicad 29, and a working, licensed **Archicad 29** installation. This release is not for macOS or other Archicad versions.
- **Windows PowerShell 5.1 or later** (included in Windows).
- **Microsoft Visual C++ v14 Redistributable, x64**: [Microsoft installer](https://aka.ms/vs/17/release/vc_redist.x64.exe). Install or repair it if a `VCRUNTIME140` / `MSVCP140` DLL is reported missing. The x86 package alone is not sufficient.
- Internet access to `python.org`, `bootstrap.pypa.io`, and PyPI for the initial runtime setup and any command-specific packages.
- A writable user profile. Run the installer as the **same Windows user who runs Archicad**; no administrator rights are needed for Tapioca's per-user runtime or commands.

You do **not** need a repository checkout, system Python, Visual Studio, the Archicad DevKit, CMake, Node.js, or a .NET SDK to use the basic prebuilt add-on.

## Install the release

1. Download **Tapioca-AC29.zip** from [Releases](https://github.com/keverza/tapioca-core/releases), unblock the ZIP in Windows Properties if necessary, and extract it completely.
2. Keep the extracted `Tapioca` directory in a permanent location. Do not load an add-on directly from the ZIP or copy only its `.apx` file.
3. **Close every Archicad session.** Open PowerShell in the extracted `Tapioca` directory and run:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Tapioca.ps1
   ```

   This downloads x64 **CPython 3.12.10** into `%LOCALAPPDATA%\Tapioca\runtime`, installs/verifies `numpy==2.0.2`, Pillow, Requests, and `pydantic>=2.7,<3`, and copies bundled starter commands to `%LOCALAPPDATA%\Tapioca\Commands`. Python is downloaded, not included in the ZIP. A healthy existing runtime is reused; existing command folders are preserved.

4. Start Archicad 29, open **Options > Add-On Manager**, and add the extracted **Tapioca.apx**. Keep **EvPPy.dll** and **PyPackage\** beside it, with all other release subdirectories intact. Disable/remove an older Tapioca registration rather than loading both versions.
5. Open **Tapioca Panel** from the **Tapioca** menu (menu placement may depend on your Work Environment). Open a project, press **Rescan**, select **Hello Tapioca**, and run it.

The release layout is:

```text
Tapioca\
  Tapioca.apx
  EvPPy.dll
  PyPackage\
  Commands\
  Install-Tapioca.ps1
  Install-Runtime.ps1
  README.md
  ...optional integration directories...
```

**Repository users:** the flat `dist\*.apx` / `dist\*.dll` files are build pickups, not a deployable installation. Use `dist\Tapioca\` or `dist\Tapioca-AC29.zip`. Build/stage it with `tools\build\Build-AddOn29.ps1`; repackage an existing build with `tools\build\Stage-Release.ps1`.

## Optional prerequisites

- **Tapir for AC29** is needed only by commands that declare it (for example, Archicad and Tapir Info); the core add-on and Hello Tapioca do not require Tapir.
- External point-cloud conversion needs **CloudCompare** and a valid executable path in that command.
- Optional managed integration workers need the matching **x64 .NET runtime**, plus their external application and license. Inspect the included `*.runtimeconfig.json`: `Microsoft.WindowsDesktop.App` requires the **Desktop Runtime**, while `Microsoft.NETCore.App` alone requires the regular **.NET Runtime**. Match its major version unless its `rollForward` setting explicitly permits a newer one. These integrations are not prerequisites for Python commands.
- Command-specific Python packages declared with `requires=[...]` are installed into the managed runtime on demand. Installing them into your system Python does not make them available to Tapioca.

## Update or repair

With Archicad closed, extract a new release into a separate permanent folder, run its installer, and update the Add-On Manager registration to that folder. Do not mix binaries and Python packages from different builds.

```powershell
# Explicitly rebuild the managed runtime (removes its installed packages).
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Tapioca.ps1 -ForceRuntime

# Explicitly overwrite bundled example files; back up edits first.
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Tapioca.ps1 -UpdateExamples
```

Neither option deletes unrelated command folders. To add your own command, create `%LOCALAPPDATA%\Tapioca\Commands\MyCommand\command.py` and press **Rescan**. Repository authors can continue to use `AddOn\EvP\Sync-Commands.ps1` instead.

For runtime-only setup, run `Install-Runtime.ps1`. `-NoBaseline` and custom `-Baseline` values are advanced options, not a supported first-run setup. A custom `-Target` requires setting `EVP_PYTHON_HOME` to that directory **before starting Archicad**; an existing `EVP_PYTHON_HOME` takes precedence over the default managed runtime.

If startup fails, check the complete release layout, the x64 Visual C++ runtime, and `%LOCALAPPDATA%\Tapioca\logs`. If a command is absent, check its `command.py` and press Rescan. Package binaries can be locked by a running Archicad session: close Archicad before repairs. A successful offline setup does not prove the add-on's live Archicad behavior.
