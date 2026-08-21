---
title: Command authoring
description: Create scanner-readable Tapioca Python commands.
sidebar:
  label: Command authoring
  order: 2
---

A Tapioca command is a top-level `run` function plus literal metadata that the
palette can read without executing the command. Production commands live under
the command roots and are copied to the managed command directory by the sync
script.

Read the complete [Commands specification](https://github.com/keverza/tapioca-core/blob/main/docs/architecture/commands/SPEC.md)
for the full contract.

## The smallest useful shape

```python title="AddOn/EvP/Commands/<Name>/command.py"
import tapioca


@tapioca.command(
    title="Your Command",
    category="Annotation",
    needs_selection=True,
    description="One line for the palette.",
)
def run(count: tapioca.Int(minimum=1) = 4):
    # Read, compute, and present a result.
    ...
```

## Authoring rules

### Use the public import

New commands should use `import tapioca`. Existing `import evp` commands remain
valid during the compatibility window.

### Keep metadata literal

Decorator and annotation values read by the scanner must be AST-readable
literals. Computed metadata can disappear from the palette without changing the
Python syntax error state.

### Keep UI in the host

Use `tapioca.ui` for text, tables, results, and progress. Do not create
tkinter, Qt, or another Python UI. Native UI is rendered by Archicad's host.

### Declare selection requirements

Set `needs_selection=True` when a command needs the active selection so the
palette can enforce that precondition before Run.

### Batch writes

Destructive workflows should default to a dry run and use one transaction for
real writes. Native commands do not open their own undo scopes.

### Check cancellation

Long pure-Python loops should call `tapioca.runtime.check_cancel()` between
chunks.

## Common surfaces

| Surface | Use it for | Example |
| --- | --- | --- |
| `tapioca.selection` | Read or modify selected elements | `selection.get()` |
| `tapioca.properties` | Resolve and read project properties | `properties.values(...)` |
| `tapioca.ui` | Show text, tables, and results | `ui.table(...)` |
| `tapioca.api` | Call a registered native or routed command | `api.call(...)` |

## Deployment path

The repository is the source of truth. The deployed copy under
`%LOCALAPPDATA%\Tapioca\Commands` is generated and must not be edited directly:

```powershell
python tools/quality/check_python.py --scan-only
powershell -File AddOn\EvP\Sync-Commands.ps1
```

Press **Rescan** in the Tapioca palette after syncing.
