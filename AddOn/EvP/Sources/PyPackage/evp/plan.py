"""A Plan — the BIM elements a command INTENDS, before anything is written.

This is the missing step between computing and committing. Until now a command
either wrote immediately or ran in a dry-run mode that logged prose and threw
the work away, so seeing what a command would do meant running it twice and
recomputing from scratch (RearrangeGrid writes a record file to disk between
the two runs to work around exactly this).

A Plan is data: a frozen list of `ElementSpec` records naming a bus verb and
its parameters. It holds no handles and touches nothing, which is what makes
the same object usable three ways —

    plan = build(inputs)          # reads only
    evp.ui.table(*plan.rows())    # show it
    plan.diff(previous).rows()    # show what CHANGED since last time
    plan.commit()                 # one transaction, one undo step

`Plan` does NOT replace `evp.transaction`; it sits on top of it and commits
through it unchanged. Everything the transaction contract says still holds: one
undo step, all-or-nothing rollback, and no read-after-write inside a batch.

⚠️ A Plan is not a promise. Between building one and committing it the user can
edit the model, so `commit()` can still fail — the plan is what the command
MEANT, and the transaction is what Archicad accepted. Never report a plan's
contents as the result of a run; report what commit() returned.
"""

from __future__ import annotations

# ⚠️ Import the FUNCTION, not the module. evp/__init__.py does
# `from .transaction import transaction`, which rebinds `evp.transaction` on
# the package from the submodule to the context manager — so
# `from . import transaction` here yields whichever of the two won the import
# race, and `_transaction.transaction(...)` then fails with a bare
# AttributeError on a function object.
from .transaction import transaction as _open_transaction

__all__ = ["ElementSpec", "Plan", "PlanDiff", "PlanError", "FromStep"]


class PlanError(RuntimeError):
    """A plan that could not be built or replayed."""


class FromStep:
    """A value that comes from an earlier spec's result, resolved at replay.

    Within one transaction a later step can consume an earlier step's output —
    that is what `Handle.<key>` is for. But a Plan is built before any
    transaction exists, so it names the earlier step by its stable `key`
    instead, and `commit()` turns that into the real Ref:

        ElementSpec(key="dim-1", command="Tapioca.SetElementDetails",
                    params={"element": FromStep("sym-1", "guid")})

    ⚠️ The transaction protocol addresses OBJECT FIELDS, not array indices, so a
    FromStep inside a list is refused there — same rule, same reason.
    """

    __slots__ = ("step_key", "path")

    def __init__(self, step_key, path="guid"):
        self.step_key = step_key
        self.path = path

    def __repr__(self):
        return "<FromStep %s.%s>" % (self.step_key, self.path)

    # Value semantics, because ElementSpec equality compares `params` and that
    # comparison IS the diff. With identity equality a spec carrying a FromStep
    # reads as changed on every single run — including against its own
    # round-tripped self, so the noise would never settle.
    def __eq__(self, other):
        return (isinstance(other, FromStep)
                and (self.step_key, self.path) == (other.step_key, other.path))

    def __hash__(self):
        return hash((self.step_key, self.path))


class ElementSpec:
    """One intended write: a bus verb, its parameters, and a stable identity.

    `key` is what makes a diff possible. It must identify the same intended
    element across two runs of the command — derive it from the thing itself
    (the source element's GUID, a grid position, a room number), never from a
    loop counter, or every row reads as changed the moment anything is inserted.

    `label` is the row the user reads in the preview. Write it for a human:
    "Slope symbol on Roof-04 (23.5 deg)", not "PlaceObject #4".
    """

    __slots__ = ("key", "op", "command", "params", "label", "detail")

    def __init__(self, key, command, params=None, op="create", label="", detail=None):
        if op not in ("create", "modify", "delete"):
            raise PlanError(
                "ElementSpec op must be create, modify or delete, not %r" % op)
        if not key:
            raise PlanError(
                "ElementSpec needs a key: it is the identity a diff compares "
                "across runs, so a spec without one can only ever read as new.")
        self.key = str(key)
        self.op = op
        self.command = command
        self.params = dict(params or {})
        self.label = label or self.key
        # Values shown as extra columns in the preview and compared by diff().
        # Keep them scalar and human-readable — this is the change the user is
        # being asked to approve, not a parameter dump.
        self.detail = dict(detail or {})

    def __repr__(self):
        return "<ElementSpec %s %s %s>" % (self.op, self.key, self.command)

    def __eq__(self, other):
        return (isinstance(other, ElementSpec)
                and (self.key, self.op, self.command, self.params, self.detail)
                == (other.key, other.op, other.command, other.params, other.detail))


class PlanDiff:
    """What changed between two plans, keyed by ElementSpec.key."""

    __slots__ = ("added", "removed", "changed", "unchanged")

    def __init__(self, added, removed, changed, unchanged):
        self.added = added            # [ElementSpec]
        self.removed = removed        # [ElementSpec] — in the old plan only
        self.changed = changed        # [(old, new)]
        self.unchanged = unchanged    # [ElementSpec]

    @property
    def is_empty(self):
        return not (self.added or self.removed or self.changed)

    def summary(self):
        return "%d added, %d changed, %d removed, %d unchanged" % (
            len(self.added), len(self.changed), len(self.removed),
            len(self.unchanged))

    def as_text(self, unchanged=False):
        """The diff as plain lines for `evp.ui.text` — THE path to use.

        ⚠️ NOT `rows()`/`ui.table`. The results table drops or misplaces cell
        text after the first fill (CMD-ResultsTableColumns), confirmed again in
        Archicad on 2026-08-19, and MassingFeasibility already moved to text for
        the same reason.

        NO COLUMN ALIGNMENT. The panel is a PROPORTIONAL font, so padded columns
        and dot leaders read ragged — both were tried live and rejected. One
        self-describing line per change instead.

        `unchanged` is off by default: on a re-run the untouched elements are
        usually most of the list, and burying three real changes under forty
        "no change" lines is how a diff stops being read.
        """
        lines = []
        for spec in self.added:
            lines.append(_line("Add", spec.label, _detail_text(spec.detail)))
        for old, new in self.changed:
            lines.append(_line("Change", new.label, _change_text(old, new)))
        for spec in self.removed:
            lines.append(_line("Remove", spec.label, _detail_text(spec.detail)))
        if unchanged:
            for spec in self.unchanged:
                lines.append(_line("Same", spec.label, _detail_text(spec.detail)))
        if not lines:
            return "No change: this run would produce exactly the last result."
        return "\n".join(lines)

    def rows(self):
        """(headers, rows, row_colors) for evp.ui.table.

        ⚠️ DO NOT USE YET. `ui.table` does not update cell text correctly after
        the first fill — CMD-ResultsTableColumns, seen again in Archicad on
        2026-08-19. Use `as_text()` with `evp.ui.text` until that is fixed. This
        is kept because it is the right shape once the control works, and
        deleting it would only mean writing it again.

        Colours are evp.ui.TABLE_COLORS NAMES, which is what ui.table takes —
        passing names keeps this module off Layer 2 entirely. Green for what
        appears, amber for what is altered, red for what goes away. Unchanged
        rows are listed last and uncoloured: they are context, not the answer.
        """
        headers = ["Change", "Element", "Detail"]
        rows = []
        colors = []
        for spec in self.added:
            rows.append(["Add", spec.label, _detail_text(spec.detail)])
            colors.append("green")
        for old, new in self.changed:
            rows.append(["Change", new.label, _change_text(old, new)])
            colors.append("amber")
        for spec in self.removed:
            rows.append(["Remove", spec.label, _detail_text(spec.detail)])
            colors.append("red")
        for spec in self.unchanged:
            rows.append(["-", spec.label, _detail_text(spec.detail)])
            colors.append(None)
        return headers, rows, colors


_FROMSTEP_TAG = "__fromstep__"


def _encode(value):
    """Make params JSON-safe. A FromStep is the only non-JSON value they hold."""
    if isinstance(value, FromStep):
        return {_FROMSTEP_TAG: [value.step_key, value.path]}
    if isinstance(value, dict):
        return {key: _encode(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_encode(item) for item in value]
    return value


def _decode(value):
    if isinstance(value, dict):
        marker = value.get(_FROMSTEP_TAG)
        if isinstance(marker, list) and len(marker) == 2:
            return FromStep(marker[0], marker[1])
        return {key: _decode(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_decode(item) for item in value]
    return value


def _line(action, label, detail):
    """One self-describing line, no padding.

    The panel's font is proportional, so any attempt at columns reads ragged;
    the action word carries the meaning instead of a position.
    """
    return "%s: %s%s" % (action, label, (" (%s)" % detail) if detail else "")


def _detail_text(detail):
    return ", ".join("%s=%s" % (key, value) for key, value in sorted(detail.items()))


def _change_text(old, new):
    """Only the fields that actually moved, old -> new.

    Printing the whole record would bury the one number that changed, which is
    the only thing the user is being asked to look at.
    """
    parts = []
    for key in sorted(set(old.detail) | set(new.detail)):
        before = old.detail.get(key)
        after = new.detail.get(key)
        if before != after:
            parts.append("%s: %s -> %s" % (key, before, after))
    return ", ".join(parts) or "parameters differ"


class Plan:
    """An ordered, immutable set of intended writes plus the undo-step name."""

    __slots__ = ("name", "specs", "notes")

    def __init__(self, name, specs=(), notes=()):
        if not name:
            raise PlanError(
                "a plan needs a name: it becomes the Archicad undo step's name, "
                "and 'Undo' with nothing after it tells the user nothing.")
        self.name = name
        self.specs = tuple(specs)
        # Anything the user should read alongside the rows — a refusal, a
        # skipped element and why. Notes are shown, not just logged: a plan that
        # silently drops an element is worse than one that says it did.
        self.notes = tuple(notes)

        seen = {}
        for spec in self.specs:
            if spec.key in seen:
                raise PlanError(
                    "two specs share the key %r (%s and %s). Keys are the "
                    "identity a diff compares, so duplicates make the diff "
                    "arbitrary." % (spec.key, seen[spec.key].command, spec.command))
            seen[spec.key] = spec

    def __len__(self):
        return len(self.specs)

    def __iter__(self):
        return iter(self.specs)

    def __repr__(self):
        return "<Plan %r %d spec(s)>" % (self.name, len(self.specs))

    def by_key(self):
        return {spec.key: spec for spec in self.specs}

    def as_text(self):
        """The plan as plain lines for `evp.ui.text` — no comparison.

        See PlanDiff.as_text for why this is text and not a table.
        """
        lines = [_line(spec.op.title(), spec.label, _detail_text(spec.detail))
                 for spec in self.specs]
        lines.extend("Note: %s" % note for note in self.notes)
        return "\n".join(lines)

    def rows(self):
        """(headers, rows) for evp.ui.table — see PlanDiff.rows: DO NOT USE YET."""
        headers = ["Action", "Element", "Detail"]
        rows = [[spec.op.title(), spec.label, _detail_text(spec.detail)]
                for spec in self.specs]
        return headers, rows

    # ---------------- persistence, so a diff has something to compare to ----

    def to_json(self):
        """A JSON-safe dict. See evp._planstore for why a plan is persisted."""
        return {
            "name": self.name,
            "notes": list(self.notes),
            "specs": [
                {
                    "key": spec.key,
                    "op": spec.op,
                    "command": spec.command,
                    "params": _encode(spec.params),
                    "label": spec.label,
                    "detail": spec.detail,
                }
                for spec in self.specs
            ],
        }

    @classmethod
    def from_json(cls, data):
        return cls(
            data["name"],
            [ElementSpec(key=s["key"], command=s["command"],
                         params=_decode(s.get("params") or {}),
                         op=s.get("op", "create"), label=s.get("label", ""),
                         detail=s.get("detail") or {})
             for s in data.get("specs") or []],
            notes=data.get("notes") or [],
        )

    def diff(self, previous):
        """What this plan changes relative to `previous` (a Plan, or None).

        With no previous plan every spec is an addition — which is the right
        answer for a first run, not a special case to guard against.
        """
        if previous is None:
            return PlanDiff(list(self.specs), [], [], [])

        old = previous.by_key()
        new = self.by_key()
        added = [new[k] for k in new if k not in old]
        removed = [old[k] for k in old if k not in new]
        changed = []
        unchanged = []
        for key in new:
            if key not in old:
                continue
            if old[key] == new[key]:
                unchanged.append(new[key])
            else:
                changed.append((old[key], new[key]))
        return PlanDiff(added, removed, changed, unchanged)

    def commit(self, name=None):
        """Replay every spec into ONE transaction. Returns the step results.

        The undo-step name defaults to the plan's. A caller may override it
        when the same plan is committed for a different reason, but a plan that
        commits under a name the user did not see in the preview is a surprise
        in the undo list.
        """
        if not self.specs:
            raise PlanError(
                "commit() on an empty plan. A command with nothing to write "
                "should say so and return, not open an empty undo step.")

        with _open_transaction(name or self.name) as tx:
            handles = {}
            for spec in self.specs:
                params = _resolve_from_steps(spec, handles)
                handles[spec.key] = tx.call(spec.command, params)
            ordered = [handles[spec.key] for spec in self.specs]
        return [handle.result() for handle in ordered]


def _resolve_from_steps(spec, handles):
    """Turn every FromStep in a spec's params into a real transaction Ref.

    Only object fields are walked, not lists — the binding protocol addresses
    dot-separated paths, so a Ref inside an array has nowhere to be addressed
    and the transaction refuses it. Refusing here instead names the spec.
    """
    def walk(value, path):
        if isinstance(value, FromStep):
            handle = handles.get(value.step_key)
            if handle is None:
                raise PlanError(
                    "spec %r reads from step %r, which does not run before it. "
                    "A FromStep may only name an EARLIER spec — results do not "
                    "exist until replay reaches them."
                    % (spec.key, value.step_key))
            ref = handle
            for part in value.path.split("."):
                ref = getattr(ref, part)
            return ref
        if isinstance(value, dict):
            return {k: walk(v, "%s.%s" % (path, k)) for k, v in value.items()}
        if isinstance(value, (list, tuple)):
            for item in value:
                if isinstance(item, FromStep):
                    raise PlanError(
                        "spec %r puts a FromStep inside a list at %s. The "
                        "binding protocol addresses object fields, not array "
                        "indices, so it has no path to resolve."
                        % (spec.key, path))
            return value
        return value

    return walk(spec.params, spec.key)
