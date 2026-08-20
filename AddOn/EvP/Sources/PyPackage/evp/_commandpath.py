"""Where a command's imports come from, and what is thrown away after the run.

Three sources, in this precedence order, and the order is the whole design:

1. **The command's own folder** — ``import helpers``. "Folder = one command (+ its
   helpers)" is a documented promise of the command format.
2. **``_lib/`` at the scripts root** — ``import roof_utils``. The shared slot every
   command under that root sees. Organise it by domain (``roof_utils.py`` +
   ``wall_math.py``), not as one monolith; the folder is on ``sys.path``, so any
   number of modules is fine.
3. **An exporting sibling command folder** — ``from ApartmentGraph import aptgraph``.
   OPT-IN: a folder is importable by its own name only if it contains ``_exports.py``.
   Without the marker a sibling stays private, so renaming or reworking an ordinary
   command cannot break someone else's import.

Both runners (Zone B in ``EvPPy.cpp``, Zone C in ``_evp_external_main.py``) and the
offline dry-run harness call ``activate()``/``deactivate()`` here. ONE implementation
on purpose: when the two zones each carried their own copy, a helper module worked
externally and failed in-process, and the difference was invisible from the command.

Precedence falls out of the mechanism rather than being enforced separately: 1 and 2
are ``sys.path`` entries, APPENDED (never prepended — a command folder must not be
able to shadow ``evp`` itself, or a stray ``json.py`` in someone's folder breaks the
bus), and 3 is a ``sys.meta_path`` finder appended AFTER ``PathFinder``, so anything
reachable on ``sys.path`` — stdlib, ``evp``, the command's own folder, ``_lib`` — wins
a name clash against an exporting sibling.

⚠️ EVICTION IS LOAD-BEARING. ``deactivate()`` drops every module that came out of the
scripts root, because the embedded interpreter is never finalized: without it an
edited helper keeps serving its stale version for the life of the Archicad session,
and the symptom is an AttributeError on a function that plainly exists in the file —
which reads like a typo and is not. It cost a full user round trip on SunStudy.
Eviction is BY FILE LOCATION, never by "what this function added to sys.path": a
command that does the documented ``sys.path.insert(0, dirname(__file__))`` puts its
own folder there itself, so the added-list is empty on the next run and a
symmetric-cleanup scheme evicts nothing at all. That is the exact shape of the
SunStudy bug.
"""

import importlib.util
import os
import sys

#: A command folder is importable by its own name only if it contains this file.
#: The file IS the package body (it is loaded as the package's ``__init__``), so it
#: can re-export a curated surface -- ``from .aptgraph import CONFIG`` -- or stay
#: empty and merely say "my helper modules are public". Keep it cheap: it runs on
#: first import, inside someone else's command run.
EXPORTS_MARKER = "_exports.py"


class _ExportsFinder:
    """Resolves ``import <SiblingCommandFolder>`` for opted-in folders only.

    Top-level names only. A submodule (``ApartmentGraph.aptgraph``) is found by the
    ordinary ``PathFinder`` walking the package's ``__path__``, so nothing here has to
    understand the inside of a command folder.
    """

    def __init__(self, root):
        self.root = root
        self.served = set()

    def find_spec(self, fullname, path=None, target=None):
        if path is not None or "." in fullname:
            return None                      # submodules: not ours, PathFinder's
        folder = os.path.join(self.root, fullname)
        marker = os.path.join(folder, EXPORTS_MARKER)
        if not os.path.isfile(marker):
            return None
        spec = importlib.util.spec_from_file_location(
            fullname, marker, submodule_search_locations=[folder])
        self.served.add(fullname)
        return spec


def exporting_folders(root):
    """The command folders under `root` that opted into being importable."""
    try:
        names = sorted(os.listdir(root))
    except OSError:
        return []
    return [n for n in names
            if not n.startswith("_")
            and os.path.isfile(os.path.join(root, n, EXPORTS_MARKER))]


def activate(command_dir):
    """Make a command folder's imports resolvable. Returns a token for `deactivate`.

    Idempotent against a folder that is already on ``sys.path`` (a command may have
    put it there itself) — nothing is added twice, and the token records only what
    THIS call added.
    """
    folder = os.path.abspath(command_dir)
    root = os.path.dirname(folder)
    lib = os.path.join(root, "_lib")

    added = [d for d in (folder, lib)
             if os.path.isdir(d) and d not in sys.path]
    for d in added:
        sys.path.append(d)

    finder = _ExportsFinder(root)
    sys.meta_path.append(finder)           # APPEND: after PathFinder, never before

    return {"root": root, "folder": folder, "lib": lib,
            "added": added, "finder": finder}


def deactivate(token):
    """Undo `activate` and evict every module that came out of the scripts root.

    Safe to call on a partially-set-up token, and it never raises: it runs in a
    ``finally`` after a command that may have failed, and a cleanup error would
    replace the exception worth reading.
    """
    if not token:
        return
    for d in token.get("added") or ():
        try:
            sys.path.remove(d)
        except ValueError:
            pass

    finder = token.get("finder")
    if finder is not None:
        try:
            sys.meta_path.remove(finder)
        except ValueError:
            pass
        for name in finder.served:
            sys.modules.pop(name, None)    # namespace-ish parents have no __file__

    root = token.get("root")
    if not root:
        return
    prefix = os.path.abspath(root) + os.sep
    for mod_name, mod in list(sys.modules.items()):
        mod_file = getattr(mod, "__file__", None) or ""
        if mod_file and os.path.abspath(mod_file).startswith(prefix):
            sys.modules.pop(mod_name, None)
