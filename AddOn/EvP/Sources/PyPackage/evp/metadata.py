"""Tapioca's own metadata on elements: a typed property graph, and the project's schema of it.

    from evp import metadata

    entity = metadata.get([guid])[guid]          # the element's metadata, a dict
    metadata.merge(guid, metadata.entity(
        properties=[metadata.prop("program.usage", "residential", "enum")],
        ranges=[metadata.floors(1, 7, [metadata.prop("program.usage", "residential", "enum")])]))
    schema = metadata.schema()                   # the project's, or the default
    metadata.extend_schema({"properties": [...], "enumerations": [...]})

Six primitives carry everything -- property, classification, range, relationship, set and
tag -- and a workflow defines keys over them in the project's schema rather than a
structure of its own. Values are typed ({"type": "length", "value": 3.2}); lengths are
metres, areas square metres, angles degrees, percentages a fraction (0.25). Every value
says where it came from (`source`: user, graph, archicad, analysis, import) and whether it
was decided or computed (`state`: authored, computed, imported, derived, cached).

The metadata lives on the element itself; the schema in one object of the project. A write
is checked against the schema first: a defined key holding another type, or an option its
enumeration lacks, is refused with the schema's reasons and the element is not touched.
Writes go on Archicad's undo stack -- one step per call, or one for a whole
`evp.transaction`.
"""

import json as _json

from .api import call


class MetadataError(RuntimeError):
    """An element's metadata that could not be read, or a write refused -- by the project's
    schema (`problems`, a sentence each) or by the element. The call itself succeeded: a
    transport failure is :class:`evp.EvpError`."""

    def __init__(self, guid, error, problems=()):
        self.guid = guid
        self.error = error
        self.problems = list(problems)
        text = f"metadata of {guid}: {error}"
        if self.problems:
            text += ": " + "; ".join(self.problems)
        super().__init__(text)


def _elements(guids):
    return [{"elementId": {"guid": g}} for g in guids]


def get(guids):
    """{guid: metadata dict} for each element. An element without Tapioca metadata reads
    as its empty entity (``found`` False in :func:`read`); one that cannot be read raises."""
    out = {}
    for item in read(guids):
        guid = item["elementId"]["guid"]
        if item.get("error"):
            raise MetadataError(guid, item["error"])
        out[guid] = item["metadata"]
    return out


def read(guids):
    """The verb's own records: [{elementId, found, metadata (a dict), error?}], one per guid,
    in order -- for a caller that would rather see a failure than have it raised."""
    items = (call("Tapioca.GetElementMetadata", {"elements": _elements(guids)}).data or {}).get("items", [])
    for item in items:
        text = item.get("metadata") or ""
        item["metadata"] = _json.loads(text) if text else {}
    return items


def write(entries, mode="merge"):
    """Write [(guid, metadata dict), ...]. `mode` "merge" lays each over what the element
    carries -- what it does not mention is kept; "replace" makes it the whole. Returns the
    verb's {count, changed, results: [{elementId, succeeded, error?, problems?}]}."""
    items = [{"elementId": {"guid": guid}, "metadata": _json.dumps(meta)} for guid, meta in entries]
    return call("Tapioca.SetElementMetadata", {"items": items, "mode": mode}).data or {}


def merge(guid, meta):
    """Lay `meta` over the element's metadata; :class:`MetadataError` with the schema's reasons
    when refused."""
    return _one(write([(guid, meta)], "merge"))


def replace(guid, meta):
    """Make `meta` the element's whole metadata; raises with the schema's reasons when refused."""
    return _one(write([(guid, meta)], "replace"))


def _one(result):
    for record in result.get("results", []):
        if not record.get("succeeded"):
            raise MetadataError(
                record["elementId"]["guid"], record.get("error", "refused"), record.get("problems") or ()
            )
    return result


def schema():
    """The project's schema as a dict, and -- under "stored" -- whether the project has one of
    its own (False: the default)."""
    data = call("Tapioca.GetMetadataSchema", {}).data or {}
    parsed = _json.loads(data.get("schema") or "{}")
    parsed["stored"] = bool(data.get("stored"))
    return parsed


def extend_schema(extra):
    """Add what `extra` defines that the project's schema lacks, by id -- a workflow's keys,
    enumerations and classification systems -- leaving what is there alone. Returns
    {revision, added}."""
    return call("Tapioca.SetMetadataSchema", {"schema": _json.dumps(extra), "mode": "extend"}).data or {}


def replace_schema(whole):
    """Store `whole` as the project's schema: the way to change an existing definition."""
    whole = {k: v for k, v in whole.items() if k != "stored"}
    return call("Tapioca.SetMetadataSchema", {"schema": _json.dumps(whole), "mode": "replace"}).data or {}


# --------------------------------------------------------------------------- #
#  The model's shapes                                                          #
# --------------------------------------------------------------------------- #


def value(v, type):
    """A typed value: {"type": "length", "value": 3.2}."""
    return {"type": type, "value": v}


def prop(key, v, type="string", source="graph", state="authored", unit=None, generator=None):
    """A property under a namespaced key ("program.usage"). `source` says who wrote it:
    a script is "graph" unless it says otherwise; a value computed from geometry is
    `state="computed"` with its `generator`, so it can be invalidated."""
    out = {"key": key, "value": value(v, type), "state": state, "provenance": {"source": source}}
    if unit:
        out["unit"] = unit
    if generator:
        out["generator"] = generator
    return out


def floors(first, last, properties, domain="floor"):
    """Properties over floors `first` to `last`, both included: floor 0 commerce, floors
    1-7 residential. Assigning one takes its keys from whatever other floors' ranges it
    covers."""
    return {"domain": domain, "from": int(first), "to": int(last), "properties": list(properties)}


def entity(properties=(), ranges=(), classifications=(), relationships=(), tags=(), sets=()):
    """An entity's metadata to merge or replace: only what is given is said."""
    out = {}
    if properties:
        out["properties"] = list(properties)
    if ranges:
        out["ranges"] = list(ranges)
    if classifications:
        out["classifications"] = [{"system": s, "value": v} for s, v in classifications]
    if relationships:
        out["relationships"] = [{"type": t, "target": target} for t, target in relationships]
    if tags:
        out["tags"] = list(tags)
    if sets:
        out["sets"] = list(sets)
    return out
