# `_lib/` — shared modules every command can import

Put a module here and **every command under this scripts root can `import` it**, in both
zones and in the offline dry-run harness:

```python
import probelog                     # _lib/probelog.py
log = probelog.start("my_command")
```

The folder is on `sys.path`, so any number of modules is fine — **organise by domain**
(`roof_utils.py` + `wall_math.py`), never as one growing monolith. It is not scanned for
commands: the `_` prefix is reserved and the scanner skips it, so a `command.py` in here
would simply never appear in the palette.

## Precedence — what wins a name clash

1. `evp` and the stdlib (always first: a module here can never shadow the bus)
2. the running command's **own folder** (`import helpers`)
3. **this folder**
4. an **exporting sibling command folder** (see below)

So a command that has its own `roof_utils.py` keeps using its own. Name modules here
distinctly enough that nobody has to think about it.

## Importing from another command folder

A sibling command folder is private by default. Drop an **`_exports.py`** in it and it
becomes importable by its own folder name:

```python
from ApartmentGraph import aptgraph            # ApartmentGraph/_exports.py exists
```

`_exports.py` is loaded as that package's `__init__`, so it can also re-export a curated
surface (`from .aptgraph import CONFIG`). Keep it cheap — it runs on first import, inside
someone else's run. Opt-in is the point: a folder without the marker can be renamed or
reworked without breaking a stranger's import.

This replaces `sys.path.insert(0, "../OtherCommand")`, which put the sibling folder
*ahead* of everything (it could shadow `evp`) and left the sibling's modules un-evicted,
so an edit to one kept serving its stale version for the rest of the Archicad session.

## Editing a module here

Every module under the scripts root is evicted from `sys.modules` after each run, so an
edit takes effect on the next run — no Archicad restart. Run `Sync-Commands.ps1` first:
this folder is git-tracked in the repo and copied out like any command folder.

The mechanics live in `Sources/PyPackage/evp/_commandpath.py`.
