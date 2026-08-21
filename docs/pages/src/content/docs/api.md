---
title: API boundary
description: Use the Tapioca Python facade to request model data safely.
sidebar:
  label: API boundary
  order: 3
---

Tapioca keeps Python workflows flexible without turning Python into a second
Archicad SDK. Requests cross a controlled boundary, and Archicad remains the
authority for model truth.

```text
Python command -> Tapioca native host -> Archicad
workflow          validation/dispatch     SDK authority
```

## Three callable families

Use the namespace that owns the capability:

| Namespace | Purpose | Example |
| --- | --- | --- |
| `Tapioca.*` | Native Tapioca commands | `tapioca.api.call("Tapioca.Name", params)` |
| `API.*` | Official Archicad JSON API wrappers | `tapioca.api.call("API.Name", params)` |
| `Tapir.*` | Commands routed to the Tapir add-on | `tapioca.api.call("Tapir.Name", params)` |

`EvP.*` remains accepted at the dispatcher boundary for unmigrated consumers.
New callers and documentation use canonical `Tapioca.*` names.

## Make your first API request

This request reads the current Archicad selection. It requires a running
Tapioca add-on and an active command context, but no network authentication or
external service.

```python
import tapioca


result = tapioca.api.call("Tapioca.GetSelection", {})
elements = (result.data or {}).get("elements", [])

tapioca.ui.text(f"Selected: {len(elements)}")
```

The same operation is available through the higher-level
`tapioca.selection.get()` facade.

## Python facade

The shipped package is organized by the question a command is asking:

- `tapioca.selection` for selection and highlight state.
- `tapioca.elements` for IDs, headers, details, and writes.
- `tapioca.properties` for property resolution and values.
- `tapioca.geometry` for snapshots, rays, and queries.
- `tapioca.ui` for text, tables, and progress.
- `tapioca.transaction` for deferred, atomic writes.
- `tapioca.runtime` for cancellation and execution state.
- `tapioca.paths` for managed logs and output paths.

## Request-shaped extraction

Ask for the smallest useful projection:

1. Scope, such as a selection, explicit element IDs, an analysis scope, or
   required context.
2. Representation, such as mesh or materials.
3. Archicad properties required by the consumer.
4. Execution intent: `interactive`, `background`, or `auto`.

No scope implies the whole project. A snapshot or cache may support a workflow,
but it does not replace Archicad as the source of project meaning.

## Safety rules

- Python never calls ACAPI directly.
- Archicad-dependent work stays on the controlled main-thread path.
- Native success payloads contain domain data, not transport fields.
- Batch writes use one transaction and one user undo step.
- A wrapper does not hide a provider gap; missing capabilities belong to the
  owning native/API work.

## Machine-readable catalog

The generated API catalog is available in the repository:

- [`dist/TAPIOCA-API-V2.json`](https://github.com/keverza/tapioca-core/blob/main/dist/TAPIOCA-API-V2.json)
  is the machine-readable contract.
- [`dist/TAPIOCA-API-V2.md`](https://github.com/keverza/tapioca-core/blob/main/dist/TAPIOCA-API-V2.md)
  is the generated human-readable table.

For the complete contract, read the [API specification](https://github.com/keverza/tapioca-core/blob/main/docs/architecture/api/SPEC.md).
