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

- Archicad 29.
- A Windows PowerShell session.
- A checkout of the public `tapioca-core` repository.
- The prebuilt `Tapioca_AC29_Win.apx` release asset.

For source-build prerequisites, see [Development](../development/).

## Install the runtime

Download the AC29 release from the [GitHub Releases
page](https://github.com/keverza/tapioca-core/releases). Keep the repository
checkout nearby because it contains the runtime installer and public examples.

With Archicad closed, run these commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\dist\Install-Runtime.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\AddOn\EvP\Sync-Commands.ps1
```

`Install-Runtime.ps1` provisions the bundled CPython runtime and baseline
packages under `%LOCALAPPDATA%\Tapioca\runtime`. The sync script copies
commands to `%LOCALAPPDATA%\Tapioca\Commands`.

## Load Tapioca in Archicad

1. Open Archicad 29.
2. Open **Options > Add-On Manager**.
3. Add the Tapioca and Tapir add-ons.
4. Open the Tapioca palette from the Window menu.

## Run the first example

Press **Rescan** in the palette, choose **Hello Tapioca**, accept the default
value, and run it. A successful result proves that the add-on, managed runtime,
command sync, and palette scanner are connected.

## Create your first file

Commands are folders with a scanner-readable `command.py` entry file:

```python title="Examples/HelloCommand/command.py"
import tapioca


@tapioca.command(
    title="Hello Tapioca",
    category="Examples",
)
def run(name: tapioca.Text = "Archicad"):
    tapioca.ui.text("Hello, " + name)
```

Save the file under an active public command root, run the sync script again,
and press **Rescan**.

## Verify the installation

If the command does not appear, run the scanner-only quality check from the
repository root:

```powershell
python tools/quality/check_python.py --scan-only
```

Then run the sync script and press **Rescan** again. See
[Troubleshooting](../troubleshooting/) for the common first-run symptoms.

## Next steps

- Learn the [command authoring contract](../commands/).
- Make a first [API request](../api/#make-your-first-api-request).
- Read the [development and contributing guide](../development/).
