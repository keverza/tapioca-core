"""Opt-in visual trace records for inspecting values produced during a command."""

from __future__ import annotations

import contextvars
import json
import math
from numbers import Real

from . import _watchstore

__all__ = [
    "watch", "point", "polyline", "arrow", "dimension", "angle", "label",
    "element", "WatchBudgetError", "MAX_NODES", "MAX_FRAMES_PER_NODE",
    "MAX_POINTS",
]

MAX_NODES = 64
MAX_FRAMES_PER_NODE = 512
MAX_POINTS = 20000
_KINDS = {"point", "polyline", "arrow", "dimension", "angle", "label", "element"}
_OPTIONAL = {"text", "role", "closed", "direction", "guid", "offset"}
_REQUIRED_POINTS = {
    "point": 1, "arrow": 2, "dimension": 2, "angle": 3, "label": 1,
}
_current = contextvars.ContextVar("evp_watch_trace", default=None)


class WatchBudgetError(ValueError):
    """A watch trace exceeded a fixed capture or persistence budget."""


class _Trace:
    __slots__ = ("nodes", "point_count")

    def __init__(self):
        self.nodes = []
        self.point_count = 0

    def append(self, name, primitives):
        name = str(name)
        node_index = next(
            (index for index, node in enumerate(self.nodes) if node["name"] == name),
            None,
        )
        if node_index is None and len(self.nodes) >= MAX_NODES:
            raise WatchBudgetError(
                "the watch trace would hold more than %d nodes" % MAX_NODES)

        normalized = [_primitive(record) for record in primitives]
        added_points = sum(len(record.get("points", ())) // 3 for record in normalized)
        if self.point_count + added_points > MAX_POINTS:
            raise WatchBudgetError(
                "the watch trace would hold %d points, over the cap of %d"
                % (self.point_count + added_points, MAX_POINTS))

        frame_count = 0 if node_index is None else len(self.nodes[node_index]["frames"])
        if frame_count >= MAX_FRAMES_PER_NODE:
            raise WatchBudgetError(
                "watch node %r would hold more than %d frames"
                % (name, MAX_FRAMES_PER_NODE))

        # Build and size a candidate before mutating the live trace. A refused
        # frame can therefore never leave a node, point count, or partial frame.
        candidate = [{"name": node["name"], "frames": list(node["frames"])}
                     for node in self.nodes]
        frame = {"index": frame_count, "primitives": normalized}
        if node_index is None:
            candidate.append({"name": name, "frames": [frame]})
        else:
            candidate[node_index]["frames"].append(frame)
        payload = _payload(candidate)
        if len(_watchstore.encode(payload)) > _watchstore.MAX_PERSISTED_BYTES:
            raise WatchBudgetError(
                "the watch trace would exceed the persisted-byte cap of %d"
                % _watchstore.MAX_PERSISTED_BYTES)

        self.nodes = candidate
        self.point_count += added_points

    def payload(self):
        return _payload(self.nodes)


def _payload(nodes):
    encoded_nodes = []
    for node in nodes:
        encoded_frames = [
            json.dumps(frame, ensure_ascii=False, separators=(",", ":"))
            for frame in node["frames"]
        ]
        encoded_nodes.append(json.dumps(
            {"name": node["name"], "frames": encoded_frames},
            ensure_ascii=False, separators=(",", ":"),
        ))
    return {"version": 1, "nodes": encoded_nodes}


def _point(value, what="point"):
    tolist = getattr(value, "tolist", None)
    if tolist is not None:
        value = tolist()
    try:
        values = list(value)
    except TypeError as exc:
        raise ValueError("%s must contain three numeric coordinates" % what) from exc
    if len(values) != 3 or any(isinstance(item, bool) or not isinstance(item, Real)
                               for item in values):
        raise ValueError("%s must contain three numeric coordinates" % what)
    result = [float(item) for item in values]
    if not all(math.isfinite(item) for item in result):
        raise ValueError("%s coordinates must be finite" % what)
    return result


def _points(value, what="points"):
    tolist = getattr(value, "tolist", None)
    if tolist is not None:
        value = tolist()
    try:
        values = list(value)
    except TypeError as exc:
        raise ValueError("%s must be points or flat XYZ coordinates" % what) from exc
    if values and not isinstance(values[0], Real):
        flat = []
        for index, item in enumerate(values):
            flat.extend(_point(item, "%s[%d]" % (what, index)))
        return flat
    if len(values) % 3:
        raise ValueError("%s has %d values, not complete XYZ points" % (what, len(values)))
    flat = []
    for start in range(0, len(values), 3):
        flat.extend(_point(values[start:start + 3], what))
    return flat


def _primitive(record):
    if not isinstance(record, dict):
        raise TypeError("a watch primitive record must be a dict")
    kind = record.get("kind")
    if kind not in _KINDS:
        raise ValueError("unknown watch primitive kind %r" % kind)
    allowed = {"kind", "text", "role", "closed", "direction", "offset"}
    allowed.add("guid" if kind == "element" else "points")
    unknown = set(record) - allowed
    if unknown:
        raise ValueError("unknown watch primitive fields: %s" % ", ".join(sorted(unknown)))

    result = {"kind": kind}
    if "points" in record:
        result["points"] = _points(record["points"])
    required = _REQUIRED_POINTS.get(kind)
    count = len(result.get("points", ())) // 3
    if required is not None and count != required:
        raise ValueError("watch %s needs %d point(s), got %d" % (kind, required, count))
    if kind == "polyline" and count < 2:
        raise ValueError("watch polyline needs at least two points")
    if kind == "element" and not record.get("guid"):
        raise ValueError("watch element needs a guid")
    for field in _OPTIONAL:
        if field in record:
            value = record[field]
            if field in ("text", "role", "guid"):
                value = str(value)
            elif field in ("closed", "direction") and not isinstance(value, bool):
                raise ValueError("watch primitive %s must be a boolean" % field)
            elif field == "offset":
                if isinstance(value, bool) or not isinstance(value, Real):
                    raise ValueError("watch primitive offset must be a finite number")
                value = float(value)
                if not math.isfinite(value):
                    raise ValueError("watch primitive offset must be a finite number")
            result[field] = value
    # Catch unserialisable direction/offset values at the API boundary.
    try:
        json.dumps(result, allow_nan=False)
    except (TypeError, ValueError) as exc:
        raise ValueError("watch primitive fields must be JSON values") from exc
    return result


def _record(name, primitive):
    trace = _current.get()
    if trace is not None:
        trace.append(name, [primitive])


def _with_options(primitive, options):
    unknown = set(options) - _OPTIONAL
    if unknown:
        raise ValueError("unknown watch primitive fields: %s" % ", ".join(sorted(unknown)))
    primitive.update(options)
    return primitive


def watch(name, value, **kwargs):
    """Record one inferred point/polyline frame, or a frame of primitive dicts.

    Returns ``value`` so a watched intermediate can remain in an expression.
    Outside an armed invocation this performs no normalization or allocation.
    """
    trace = _current.get()
    if trace is None:
        return value
    if isinstance(value, list) and value and all(isinstance(item, dict) for item in value):
        trace.append(name, value)
        return value
    flat = _points(value, "value")
    kind = "point" if len(flat) == 3 else "polyline"
    primitive = _with_options({"kind": kind, "points": flat}, kwargs)
    trace.append(name, [primitive])
    return value


def point(value, name="point", **kwargs):
    if _current.get() is None:
        return value
    primitive = _with_options({"kind": "point", "points": _point(value)}, kwargs)
    _record(name, primitive)
    return value


def polyline(points, name="polyline", **kwargs):
    if _current.get() is None:
        return points
    primitive = _with_options({"kind": "polyline", "points": _points(points)}, kwargs)
    _record(name, primitive)
    return points


def arrow(a, b, name="arrow", **kwargs):
    if _current.get() is None:
        return (a, b)
    primitive = _with_options(
        {"kind": "arrow", "points": _point(a, "a") + _point(b, "b")}, kwargs)
    _record(name, primitive)
    return (a, b)


def dimension(a, b, text="", name="dimension", **kwargs):
    if _current.get() is None:
        return (a, b)
    primitive = _with_options(
        {"kind": "dimension", "points": _point(a, "a") + _point(b, "b"),
         "text": str(text)}, kwargs)
    _record(name, primitive)
    return (a, b)


def angle(vertex, a, b, name="angle", **kwargs):
    if _current.get() is None:
        return (vertex, a, b)
    primitive = _with_options(
        {"kind": "angle", "points": (_point(vertex, "vertex")
         + _point(a, "a") + _point(b, "b"))}, kwargs)
    _record(name, primitive)
    return (vertex, a, b)


def label(anchor, text, name="label", **kwargs):
    if _current.get() is None:
        return anchor
    primitive = _with_options(
        {"kind": "label", "points": _point(anchor, "anchor"), "text": str(text)},
        kwargs)
    _record(name, primitive)
    return anchor


def element(guid, name="element", **kwargs):
    if _current.get() is None:
        return guid
    primitive = _with_options({"kind": "element", "guid": str(guid)}, kwargs)
    _record(name, primitive)
    return guid


def _start():
    return _current.set(_Trace())


def _complete(token):
    trace = _current.get()
    try:
        return trace.payload()
    finally:
        _current.reset(token)


def _abandon(token):
    _current.reset(token)


# ``evp.watch`` is deliberately a callable namespace for compatibility with
# command examples that use both watch("name", value) and watch.dimension(...).
watch.point = point
watch.polyline = polyline
watch.arrow = arrow
watch.dimension = dimension
watch.angle = angle
watch.label = label
watch.element = element
