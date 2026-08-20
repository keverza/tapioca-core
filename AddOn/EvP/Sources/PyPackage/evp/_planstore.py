"""Remember what a command's last run produced — its Plan, and its Outputs.

The Plan is here so the next run can diff against it. The Outputs are here so an
ACTION BUTTON does not have to re-run the command to have something to export:
pressing "Export CSV" must not repeat every write the run performed, and a run
whose result had to be recomputed to be saved is a run that happened twice.

Without this `ctx.previous_plan` is always None and every run reports its whole
plan as additions — which is exactly what the first in-Archicad run of
TemplateV2Probe showed: change the count, run again, get a fresh list of "Add"
rows and no sense of what moved.

⚠️ IT HAS TO BE ON DISK. The obvious place is a module-level dict, and it does
not work: every module under the scripts root is evicted from `sys.modules`
after each run (`evp._commandpath`), and the external runner is a fresh process
each time. In-memory state does not survive one run to the next by design.

This is a cache, not a record: `evp.paths.cache_dir`, deleted freely, rebuilt by
running the command again. It says what the command LAST PLANNED, never what
Archicad actually contains — the model can change underneath it, so a diff here
is "what my inputs changed", not "what the project is missing".
"""

from __future__ import annotations

import hashlib
import json
import os

from . import paths
from .plan import Plan

__all__ = ["load", "save", "load_outputs", "load_outputs_schema", "save_outputs",
           "forget"]

_CACHE = "plans"


def _path(folder):
    """One file per command folder, keyed by full path like the schema cache.

    The repo copy and the deployed copy of a command share a basename; keyed by
    name alone they would overwrite each other's last plan.
    """
    name = os.path.basename(os.path.normpath(folder)) if folder else "command"
    tag = hashlib.sha256(
        os.path.normcase(os.path.abspath(folder or name)).encode("utf-8")
    ).hexdigest()[:12]
    return os.path.join(paths.cache_dir(_CACHE), "%s-%s.json" % (name, tag))


def _read(folder):
    """The stored envelope, or {}. Tolerates the pre-envelope format.

    The file used to hold a bare plan. An old one is still readable — dropping it
    instead would make one run after an upgrade report its whole plan as
    additions, a scary-looking diff caused by nothing the user did.
    """
    try:
        with open(_path(folder), encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return {}
    if isinstance(data, dict) and "specs" in data:
        return {"plan": data}
    return data if isinstance(data, dict) else {}


def _write(folder, envelope):
    try:
        with open(_path(folder), "w", encoding="utf-8") as handle:
            json.dump(envelope, handle, ensure_ascii=False)
    except (OSError, TypeError, ValueError):
        # A cache that cannot be written costs the NEXT run its diff, or one
        # action button its input. Never the run itself.
        pass


def load(folder):
    """The Plan this command last produced, or None.

    None on anything unreadable rather than raising: a missing or stale previous
    plan makes the next diff read as all-additions, which is correct for a first
    run. Failing the command over its own cache would not be.
    """
    try:
        stored = _read(folder).get("plan")
        return Plan.from_json(stored) if stored else None
    except (ValueError, KeyError, TypeError):
        return None


def save(folder, plan):
    """Record `plan` as this command's last. Never raises."""
    if plan is None:
        return
    envelope = _read(folder)
    try:
        envelope["plan"] = plan.to_json()
    except (TypeError, ValueError):
        return
    _write(folder, envelope)


def load_outputs(folder):
    """The plain dict this command's last run returned, or None.

    A DICT, not the model: the action runs in a fresh process that would have to
    import the command to have the class, and importing a command to export its
    last result is exactly the re-run this store exists to avoid. `outputs.table_of`
    reads the schema off the live model when there is one and falls back to the
    dict's own keys when there is not.
    """
    stored = _read(folder).get("outputs")
    return stored if isinstance(stored, dict) else None


def save_outputs(folder, outputs):
    """Record what run() returned. Accepts a pydantic model or a plain dict."""
    if outputs is None:
        return
    dump = getattr(outputs, "model_dump", None)
    try:
        record = dump(mode="json") if dump is not None else dict(outputs)
    except (TypeError, ValueError):
        return
    envelope = _read(folder)
    envelope["outputs"] = record
    envelope["outputs_schema"] = _schema_of(outputs)
    _write(folder, envelope)


def _schema_of(outputs):
    """The output model's JSON Schema, so a later action can find role="table"
    without importing the command that declared it."""
    schema_of = getattr(type(outputs), "model_json_schema", None)
    if schema_of is None:
        return None
    try:
        return schema_of()
    except Exception:  # a model that cannot describe itself still ran fine
        return None


def load_outputs_schema(folder):
    """The stored Outputs JSON Schema, or None."""
    stored = _read(folder).get("outputs_schema")
    return stored if isinstance(stored, dict) else None


def forget(folder=None):
    """Drop one command's last plan, or every one. For the tests and a reset."""
    if folder is not None:
        try:
            os.remove(_path(folder))
        except OSError:
            pass
        return
    directory = paths.cache_dir(_CACHE)
    for name in os.listdir(directory):
        if name.endswith(".json"):
            try:
                os.remove(os.path.join(directory, name))
            except OSError:
                pass
