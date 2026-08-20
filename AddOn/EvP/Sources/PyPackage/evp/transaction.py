"""Transactions — how writes get ONE undo step and true atomicity.

    with evp.transaction("Place slope symbols") as tx:
        for roof in roofs:
            tx.call("EvP.PlaceLevelDimension", {...})
    # commit happens here: one lambda, one undo step
    new_guids = tx.results

The problem this solves: an undo scope only exists INSIDE the lambda passed to
ACAPI_CallUndoableCommand, on the main thread. A worker cannot hold one open
across round trips, so five sequential writes would be five undo steps — and a
failure at step 4 would leave steps 1-3 committed, with the user hitting Undo
three times.

So writes are RECORDED, not executed. Commit replays the whole batch inside one
undoable command. If any step fails, the lambda returns an error and Archicad
rolls back the entire batch: no compensating-delete machinery, no residue.

Immediate mode (calling evp.call directly) still works — each write is then its
own undo step. Fine for one-shot commands, but a loop of them is exactly the
multi-undo-step surprise this exists to prevent.
"""

import json as _json

from .api import LEGACY_NATIVE_NAMESPACE, NATIVE_NAMESPACE, call


class TransactionError(RuntimeError):
    """A transaction was used in a way its contract does not allow."""


class Ref:
    """A placeholder for a value that will not exist until replay.

    `handle.guid` yields one of these. It is resolved SERVER-SIDE, in the middle
    of the batch, at the moment the earlier step's result actually exists.
    """

    __slots__ = ("step", "key")

    def __init__(self, step, key):
        self.step = step
        self.key = key

    def __repr__(self):
        return "<evp.Ref step=%d key=%s>" % (self.step, self.key)

    def __getattr__(self, key):
        if key.startswith("_"):
            raise AttributeError(key)
        return Ref(self.step, "%s.%s" % (self.key, key))


class Handle:
    """A deferred reference to a step's result — not a value.

    The step has not run yet at record time, so there is nothing to read. But
    `handle.<key>` gives a Ref usable as an input to a LATER step in the same
    batch, resolved during replay:

        with evp.transaction("place + tag") as tx:
            h = tx.call("EvP.PlaceLevelDimension", {...})
            tx.call("EvP.SetPropertyValue", {"element": h.guid, ...})

    Reading real data back mid-batch is a TransactionError: a script that needs
    read-after-write must split into multiple transactions and accept multiple
    undo steps — explicitly, by its own structure, never silently.
    """

    __slots__ = ("_tx", "_index")

    def __init__(self, tx, index):
        self._tx = tx
        self._index = index

    @property
    def index(self):
        return self._index

    def __getattr__(self, key):
        # Only reached when normal lookup fails, so _tx/_index/index/result are safe.
        if key.startswith("_"):
            raise AttributeError(key)
        return Ref(self._index, key)

    def result(self):
        """The step's real result — only after commit."""
        if self._tx.results is None:
            raise TransactionError(
                "step %d has not run yet: a transaction records writes and replays them "
                "at commit, so results do not exist until the block exits. If you need "
                "read-after-write to decide the next step, split into two transactions "
                "and accept two undo steps." % self._index
            )
        return self._tx.results[self._index]

    def __repr__(self):
        return "<evp.Handle step=%d%s>" % (
            self._index,
            "" if self._tx.results is None else " resolved",
        )


class Transaction:
    def __init__(self, name):
        self.name = name
        self.results = None
        self._steps = []
        self._committed = False

    def call(self, command, params=None):
        """Record a write. Returns a Handle, NOT a result — nothing has run yet."""
        if self._committed:
            raise TransactionError("this transaction has already been committed")

        if command.startswith(LEGACY_NATIVE_NAMESPACE + "."):
            command = NATIVE_NAMESPACE + command[len(LEGACY_NATIVE_NAMESPACE):]

        params = dict(params or {})
        index = len(self._steps)

        # Refs travel beside params as dot-separated source/target object paths.
        # Lists are intentionally excluded: the protocol addresses object fields,
        # not array indices. Existing top-level {'element': h.guid} stays identical.
        bindings = []
        omitted = object()

        def extract_refs(value, path):
            if isinstance(value, Ref):
                if value.step >= index:
                    raise TransactionError(
                        "step %d cannot use the result of step %d: a handle is only usable "
                        "by LATER steps in the same batch" % (index, value.step)
                    )
                bindings.append({"path": path, "step": value.step, "key": value.key})
                return omitted
            if isinstance(value, dict):
                extracted = {}
                for child_key, child_value in value.items():
                    if not isinstance(child_key, str) or not child_key or "." in child_key:
                        if _contains_ref(child_value):
                            raise TransactionError(
                                "deferred binding object keys must be non-empty strings without '.'"
                            )
                        extracted[child_key] = child_value
                        continue
                    child_path = "%s.%s" % (path, child_key) if path else child_key
                    child = extract_refs(child_value, child_path)
                    if child is not omitted:
                        extracted[child_key] = child
                return extracted
            if isinstance(value, (list, tuple)) and _contains_ref(value):
                raise TransactionError(
                    "a handle cannot be used inside a list: deferred paths address object fields only"
                )
            return value

        def _contains_ref(value):
            if isinstance(value, Ref):
                return True
            if isinstance(value, dict):
                return any(_contains_ref(item) for item in value.values())
            if isinstance(value, (list, tuple)):
                return any(_contains_ref(item) for item in value)
            return False

        params = extract_refs(params, "")

        # A Ref anywhere else cannot be resolved, so say so precisely rather than
        # let json.dumps fail with "Object of type Ref is not JSON serializable".
        try:
            _json.dumps(params)
        except TypeError:
            raise TransactionError(
                "transaction parameters must be JSON-serializable after deferred bindings are extracted"
            ) from None

        # Bindings cross as JSON STRINGS, exactly like steps do — the C++ side
        # reads GS::Array<GS::UniString>. Sending them as nested objects makes
        # ObjectState::Get fail on the type and drop them ALL, silently.
        # `bindingCount` exists so that drop is detected instead of surfacing
        # later as a baffling "missing parameter" from the command itself.
        self._steps.append({
            "command": command,
            "params": params,
            "bindings": [_json.dumps(b) for b in bindings],
            "bindingCount": len(bindings),
        })
        return Handle(self, index)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if exc_type is not None:
            # The block raised: never commit a batch the script abandoned.
            self._steps = []
            return False
        self.commit()
        return False

    def commit(self):
        if self._committed:
            raise TransactionError("this transaction has already been committed")
        self._committed = True

        if not self._steps:
            self.results = []
            return self.results

        # Steps cross as an array of JSON STRINGS, not nested objects: parsing
        # GS::Array<GS::ObjectState> has never been proven in this codebase,
        # while arrays of scalars have. One less unknown on the write path.
        res = call(
            "Tapioca.CommitTransaction",
            {"name": self.name, "steps": [_json.dumps(s) for s in self._steps]},
        )
        self.results = [_json.loads(r) for r in (res.data or {}).get("results", [])]
        return self.results


def transaction(name):
    return Transaction(name)
