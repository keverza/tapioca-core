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

## Learn by example

The public repository contains small commands that can be run without the
private development workspace:

- [`HelloCommand`](https://github.com/keverza/tapioca-core/tree/main/Examples/HelloCommand)
  demonstrates a text input and a result.
- [`SelectionTable`](https://github.com/keverza/tapioca-core/tree/main/Examples/SelectionTable)
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
