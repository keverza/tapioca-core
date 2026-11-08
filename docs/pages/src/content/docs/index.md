---
title: Overview
description: Python automation for Archicad 29.
---

Tapioca is a Python command host for Archicad 29 with a small, readable Python
surface and a native add-on boundary that keeps Archicad in control.

## Python at the edge. Archicad in control.

Tapioca is a Python command host for Archicad 29. Commands can read the active
model, present useful results, and call a controlled native API when Archicad
needs to do the work.

:::tip[The boundary in one sentence]
Python owns workflow composition and pure computation. The native add-on owns
Archicad SDK access, validation, dispatch, and main-thread safety.
:::

## Find your next step

| If you want to... | Start with... |
| --- | --- |
| Install Tapioca and run a real command | [Quickstart](./getting-started/) |
| Create a scanner-readable command | [Command authoring](./commands/) |
| Read selections, properties, or geometry | [API boundary](./api/) |
| Build the add-on or contribute | [Development](./development/) |
| Diagnose a missing command or runtime | [Troubleshooting](./troubleshooting/) |

## What ships today

The add-on registers one **Tapioca** menu in Archicad 29 with these surfaces:

- **Tapioca Panel** — the command palette: scans, lists, and runs Python
  commands from the managed command directory.
- **Node Graph** — a node-graph workflow editor palette backed by the same
  command catalog.
- **Vision Window** — a standalone GPU viewport of the live model; see
  [3D viewer](#3d-viewer). **2D Vision Overlay** and **3D Vision Overlay**
  draw analysis layers inside Archicad's own views.
- **Grasshopper Editor**, **Run Grasshopper Definition**, and the
  **rhino.compute** start/stop items — integration entry points that need the
  matching external applications installed.

The three Vision items toggle their surface on or off and show a check while
enabled. An overlay stays checked while waiting for its matching 2D or 3D view.
Opening Vision Window disables both overlays; enabling an overlay closes the
window. Temporary host hides (such as modal dialogs) do not clear the window's
check.

Behind the palette, the shipped Python facade and the generated native
`Tapioca.*` command catalog provide the model access; see the
[API boundary](./api/).

## 3D viewer

The **Vision Window** (Tapioca menu) is a standalone GPU viewport of the
live Archicad model, rendered by the Diligent engine inside the add-on. It is
not a baked export: the scene is populated progressively from the model while
it stays interactive, and the caches it builds are derived working data, never
a second BIM authority.

What it supports today:

- **Navigation** — orbit, pan, and zoom in the viewer window, with the camera
  synchronised to Archicad's 3D window (`Tapioca.SetDiligentCameraSync`,
  `Tapioca.ViewerAutoOrbit`).
- **Picking and selection** — picking in the viewport maps back to Archicad's
  selection (`Tapioca.SetDiligentSelectionBridge`).
- **Rendering** — shaded, wireframe, and debug views (`Tapioca.SetDiligentRenderMode`,
  `Tapioca.SetDiligentDebugView`), sun and environment lighting
  (`Tapioca.SetDiligentSun`, `Tapioca.SetDiligentEnvironmentMap`), and
  model-anchored text labels (`Tapioca.SetDiligentTextLabels`).
- **Captures** — viewport screenshots (`Tapioca.CaptureScreenshot`) and
  fixed-size off-screen renders at `fast` or `realistic` quality with optional
  story-slice overlays (`Tapioca.StartDiligentCapture`).
- **Sun studies and point clouds** — Archicad sun-study atlases on the model
  (`Tapioca.StartSunStudy`, `Tapioca.ShowSunStudy`) and tiled point-cloud
  display (`Tapioca.LoadDiligentPointCloud`).

### Drive it from Python

The `tapioca.diligent` facade wraps the camera, preset, label, and capture
commands; every wrapper reduces to the native calls listed above:

```python
import tapioca

# Freeze the current viewer camera as a named, reusable preset.
tapioca.diligent.save_camera("south-elevation")

# Restore it later and render a fixed-size frame from it.
tapioca.diligent.apply_camera("south-elevation")
artifact, png = tapioca.diligent.capture("south-elevation", "south-elevation", 1920, 1080)
tapioca.ui.text("PNG: %s" % artifact.path)
```

### Drive it fully natively

Any registered `Tapioca.*` viewer command can be called directly through the
envelope, without the Python facade:

```python
tapioca.api.call("Tapioca.SetDiligentSun", {"enabled": True})
tapioca.api.call("Tapioca.ViewerAutoOrbit", {"enabled": True})
```

The exact parameters and outputs of every viewer command are listed in the
generated [API catalog](https://github.com/keverza/tapioca-core/blob/main/dist/TAPIOCA-API-V2.md);
the rendering contract is specified in the
[Diligent viewer specification](https://github.com/keverza/tapioca-core/blob/main/docs/architecture/diligent/SPEC.md).

## Learn by example

The public repository contains small commands that can be run without the
private development workspace:

- [`HelloCommand`](https://github.com/keverza/tapioca-core/tree/main/Examples/HelloCommand)
  demonstrates a text input and a result.
- [`SelectionReport`](https://github.com/keverza/tapioca-core/tree/main/Examples/SelectionReport)
  reads the active selection.
- [`PropertySummary`](https://github.com/keverza/tapioca-core/tree/main/Examples/PropertySummary)
  resolves and reads a built-in property.

## Project scope

Tapioca is designed for Archicad 29 and ships under the GPL-3.0-or-later
license. Archicad remains authoritative for element identity, geometry,
properties, and project meaning; Tapioca caches and results are task-scoped
working data, not a second BIM authority.

The structure of this documentation follows the core and miscellaneous content
types from [The Good Docs Project templates](https://gitlab.com/tgdp/templates).
