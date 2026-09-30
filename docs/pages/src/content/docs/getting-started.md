---
title: Quickstart
description: Install Tapioca and run your first Archicad 29 Python command.
sidebar:
  label: Quickstart
  order: 1
---

This quickstart is for Archicad users and Python authors who want to install
Tapioca, run a real example, and create their own command.

## Before you start

Have the following ready:

- 64-bit Windows and a working, licensed Archicad 29 installation.
- Windows PowerShell 5.1 or later.
- The [x64 Visual C++ v14 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- The complete **Tapioca-AC29.zip** release asset, extracted to a permanent folder.
- Internet access for Python and package downloads, and a writable user profile.

A repository checkout, system Python, and build tools are not required for the
prebuilt add-on. Run setup as the Windows user who runs Archicad.

For source-build prerequisites, see [Development](../development/).

## Install the runtime

Download the AC29 release from the [GitHub Releases
page](https://github.com/keverza/tapioca-core/releases). Unblock the ZIP in Windows
Properties if necessary, then extract it completely. Do not copy only the `.apx`.

With every Archicad session closed, run this from the extracted `Tapioca` folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Tapioca.ps1
```

The installer downloads x64 CPython 3.12.10 and installs/verifies NumPy, Pillow,
Requests, and Pydantic 2 under `%LOCALAPPDATA%\Tapioca\runtime`. It also copies
bundled starter commands to `%LOCALAPPDATA%\Tapioca\Commands`, preserving existing
command folders. An existing healthy runtime is reused. Keep the release folder
intact: the runtime is per-user, but the add-on loads its bridge and Python package
from beside the `.apx`.

Tapir is optional; only commands that declare it require its AC29 add-on. External
point-cloud conversion needs CloudCompare. Optional managed integration workers
need their matching x64 .NET runtime and external application; these are not
requirements for the basic add-on. The release README lists setup and repair options.

## Load Tapioca in Archicad

1. Open Archicad 29.
2. Open **Options > Add-On Manager**.
3. Add **Tapioca.apx** from the extracted release folder. Keep **EvPPy.dll** and
   **PyPackage/** beside it. Disable any older Tapioca registration.
4. Open **Tapioca Panel** from the **Tapioca** menu and open a project. Menu
   placement can depend on your Work Environment.

## Run the first example

Press **Rescan** in the palette, choose **Hello Tapioca**, accept the default
value, and run it. A successful result proves that the add-on, managed runtime,
starter command installation, and palette scanner are connected.

## Create your first file

Commands are folders with a scanner-readable `command.py` entry file:

```python title="Commands/MyCommand/command.py"
import tapioca


@tapioca.command(
    title="Hello Tapioca",
    category="Examples",
)
def run(name: tapioca.Text = "Archicad"):
    tapioca.ui.text("Hello, " + name)
```

Save it as `%LOCALAPPDATA%\Tapioca\Commands\MyCommand\command.py` and press
**Rescan**. Repository authors can instead use `Examples/` and run
`AddOn/EvP/Sync-Commands.ps1` from the repository root before rescanning.

## Verify the installation

If the command does not appear, check its folder and `command.py`, then press
**Rescan**. Repository authors can additionally run the scanner-only quality
check from the repository root:

```powershell
python tools/quality/check_python.py --scan-only
```

Repository authors should then run the sync script and press **Rescan** again. See
[Troubleshooting](../troubleshooting/) for the common first-run symptoms.

## Next steps

- Learn the [command authoring contract](../commands/).
- Make a first [API request](../api/#make-your-first-api-request).
- Read the [development and contributing guide](../development/).
