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
- `tapioca.model`, `tapioca.metadata`, `tapioca.topology` for structured
  model data, typed element metadata, and zone/room topology.
- `tapioca.cameras` for session-only perspective camera sets.
- `tapioca.ui` for text, tables, and progress.
- `tapioca.outputs` for CSV/JSON/text/PDF artifacts and captures.
- `tapioca.overlay` for view overlay layers, and `tapioca.preview` /
  `tapioca.diligent` for preview and render captures.
- `tapioca.layouts`, `tapioca.drawings`, `tapioca.drafting`,
  `tapioca.issues` for document production surfaces.
- `tapioca.transaction` for deferred, atomic writes.
- `tapioca.plan` for intended-element diffing before writes, and
  `tapioca.watch` for opt-in value traces.
- `tapioca.runtime` for cancellation and execution state.
- `tapioca.changes` for model-change detection, `tapioca.context` for the
  schema-command context, and `tapioca.paths` for managed logs and output paths.
- `tapioca.webui` for report helpers that reuse the command result envelope.

Every wrapper is built on `tapioca.api.call`; any wrapper behavior reduces to
one reproducible envelope call.

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

## Native workflows

Short, supported patterns composed from the shipped commands. They mirror the
examples in the repository's `Examples/` folder.

### Read the active selection

`tapioca.selection.get()` wraps `Tapioca.GetSelection`; a missing selection
returns an empty list, so check it before use.

```python
guids = tapioca.selection.get()
if not guids:
    tapioca.ui.text("Nothing is selected.")
    return
```

### Report element data with an artifact

Combine element identities with a managed artifact, and present the result.
This is the shipped `Selection Report` example in short form.

```python
identities = tapioca.elements.ids(guids)
artifact = tapioca.outputs.csv_file(
    "selection-report",
    ["Element type", "Element ID", "GUID"],
    [[row["type_name"], row["element_id"], row["guid"]] for row in identities],
)
tapioca.ui.text("CSV: %s" % artifact.path)
```

### Batch writes in one undo step

Record writes in a transaction and commit once; a failed step rolls the whole
batch back. Each direct `tapioca.api.call` write outside a transaction is its
own undo step.

```python
with tapioca.transaction("place level dimensions") as tx:
    for x, y, value in spots:
        tx.call(
            "Tapioca.PlaceLevelDimension",
            {"x": x, "y": y, "value": value},
        )
```

Native write commands never open their own undo scopes; the transaction owns
the single undo step.

## Machine-readable catalog

The generated API catalog is available in the repository:

- [`dist/TAPIOCA-API-V2.json`](https://github.com/keverza/tapioca-core/blob/main/dist/TAPIOCA-API-V2.json)
  is the machine-readable contract.
- [`dist/TAPIOCA-API-V2.md`](https://github.com/keverza/tapioca-core/blob/main/dist/TAPIOCA-API-V2.md)
  is the generated human-readable table.

Both files are produced by
[`AddOn/EvP/tools/generate_tapioca_api_v2.py`](https://github.com/keverza/tapioca-core/blob/main/AddOn/EvP/tools/generate_tapioca_api_v2.py)
from the registered native schemas, and the API catalog freshness workflow
fails CI when the catalog drifts from the registered commands. Treat the
generated catalog as authoritative for names, parameters, and outputs; the
catalog supersedes any older hand-maintained API tables.

For the complete contract, read the [API specification](https://github.com/keverza/tapioca-core/blob/main/docs/architecture/api/SPEC.md).
