"""The Python Watch Trace API, its exact wire shape, and fixed budgets."""

import json
import importlib
import os
import sys

import pytest

_PACKAGE = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
if _PACKAGE not in sys.path:
    sys.path.insert(0, _PACKAGE)

import evp  # noqa: E402
import tapioca  # noqa: E402
from evp import _watchstore  # noqa: E402
from evp import watch as watch_api  # noqa: E402
watch_module = importlib.import_module("evp.watch")


def _decode(payload):
    nodes = [json.loads(node_json) for node_json in payload["nodes"]]
    for node in nodes:
        node["frames"] = [json.loads(frame_json) for frame_json in node["frames"]]
    return nodes


def _capture(fn):
    token = watch_module._start()
    try:
        fn()
        return watch_module._complete(token)
    except BaseException:
        watch_module._abandon(token)
        raise


def test_evp_and_tapioca_export_the_same_callable_namespace():
    assert callable(evp.watch)
    assert tapioca.watch is evp.watch
    for constructor in (
            "point", "polyline", "arrow", "dimension", "angle", "label", "element"):
        assert callable(getattr(evp.watch, constructor))
        assert getattr(tapioca.watch, constructor) is getattr(evp.watch, constructor)


def test_unarmed_watch_is_a_cheap_identity_operation():
    invalid = object()
    assert watch_api("ignored", invalid, not_a_field=invalid) is invalid
    assert watch_api.point(invalid) is invalid
    assert watch_api.polyline(invalid) is invalid
    assert watch_api.element(invalid) is invalid


def test_generic_watch_infers_point_and_polyline_and_appends_frames():
    point = (1, 2, 3)
    line = [(0, 0, 0), (4, 5, 6)]

    payload = _capture(lambda: (
        watch_api("path", point, role="guide"),
        watch_api("path", line, closed=True),
    ))

    assert payload["version"] == 1
    assert all(isinstance(item, str) for item in payload["nodes"])
    nodes = _decode(payload)
    assert [node["name"] for node in nodes] == ["path"]
    assert [frame["index"] for frame in nodes[0]["frames"]] == [0, 1]
    assert nodes[0]["frames"][0]["primitives"] == [
        {"kind": "point", "points": [1.0, 2.0, 3.0], "role": "guide"}]
    assert nodes[0]["frames"][1]["primitives"] == [
        {"kind": "polyline", "points": [0.0, 0.0, 0.0, 4.0, 5.0, 6.0],
         "closed": True}]


def test_a_list_of_primitive_records_is_one_frame():
    records = [
        {"kind": "point", "points": (1, 2, 3), "role": "context"},
        {"kind": "element", "guid": "abc", "text": "wall"},
    ]
    payload = _capture(lambda: watch_api("mixed", records))

    frame = _decode(payload)[0]["frames"][0]
    assert frame["index"] == 0
    assert len(frame["primitives"]) == 2
    assert frame["primitives"][0]["points"] == [1.0, 2.0, 3.0]
    assert frame["primitives"][1] == {"kind": "element", "text": "wall", "guid": "abc"}


def test_explicit_constructors_need_no_name_and_emit_flat_points():
    def build():
        watch_api.dimension((0, 0, 0), (2, 0, 0), text="2m", offset=0.2)
        watch_api.angle((0, 0, 0), (1, 0, 0), (0, 1, 0))
        watch_api.label((3, 4, 5), "ridge", role="context")
        watch_api.arrow((0, 0, 0), (0, 0, 1), direction=True)
        watch_api.element("guid-1")

    nodes = _decode(_capture(build))
    assert [node["name"] for node in nodes] == [
        "dimension", "angle", "label", "arrow", "element"]
    primitives = [node["frames"][0]["primitives"][0] for node in nodes]
    assert primitives[0] == {
        "kind": "dimension", "points": [0.0, 0.0, 0.0, 2.0, 0.0, 0.0],
        "text": "2m", "offset": 0.2,
    }
    assert primitives[1]["points"] == [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
    assert primitives[2] == {
        "kind": "label", "points": [3.0, 4.0, 5.0], "text": "ridge",
        "role": "context",
    }
    assert primitives[3]["direction"] is True
    assert primitives[4] == {"kind": "element", "guid": "guid-1"}


def test_constructor_name_override_groups_repeated_frames():
    payload = _capture(lambda: (
        watch_api.arrow((0, 0, 0), (1, 0, 0), name="forces"),
        watch_api.arrow((0, 0, 0), (0, 1, 0), name="forces"),
    ))
    nodes = _decode(payload)
    assert len(nodes) == 1
    assert nodes[0]["name"] == "forces"
    assert [frame["index"] for frame in nodes[0]["frames"]] == [0, 1]


def test_rows_flat_coordinates_and_numpy_like_values_are_normalized():
    class FakeArray:
        def tolist(self):
            return [[0, 0, 0], [1, 2, 3]]

    payload = _capture(lambda: (
        watch_api.polyline(FakeArray(), name="array"),
        watch_api("flat", [0, 0, 0, 1, 2, 3]),
    ))
    nodes = _decode(payload)
    expected = [0.0, 0.0, 0.0, 1.0, 2.0, 3.0]
    assert nodes[0]["frames"][0]["primitives"][0]["points"] == expected
    assert nodes[1]["frames"][0]["primitives"][0]["points"] == expected


@pytest.mark.parametrize("value, match", [
    ([], "at least two"),
    ([(0, 0)], "three numeric"),
    ([0, 0, float("nan")], "finite"),
])
def test_invalid_generic_geometry_is_refused(value, match):
    with pytest.raises(ValueError, match=match):
        _capture(lambda: watch_api("bad", value))


def test_constructor_options_cannot_replace_kind_or_flat_points():
    with pytest.raises(ValueError, match="kind"):
        _capture(lambda: watch_api.point((0, 0, 0), kind="element"))
    with pytest.raises(ValueError, match="points"):
        _capture(lambda: watch_api.arrow((0, 0, 0), (1, 0, 0), points=[]))


@pytest.mark.parametrize("record, match", [
    ({"kind": "circle", "points": [0, 0, 0]}, "kind"),
    ({"kind": "point", "points": [0, 0, 0], "colour": "red"}, "colour"),
    ({"kind": "point", "points": [0, 0, 0, 1, 1, 1]}, "needs 1"),
    ({"kind": "point", "points": [0, 0, 0], "guid": "abc"}, "guid"),
    ({"kind": "element"}, "guid"),
    ({"kind": "arrow", "points": [0, 0, 0, 1, 0, 0], "direction": "forward"},
     "boolean"),
    ({"kind": "polyline", "points": [0, 0, 0, 1, 0, 0], "closed": 1},
     "boolean"),
    ({"kind": "point", "points": [0, 0, 0], "offset": "far"}, "finite number"),
    ({"kind": "label", "points": [0, 0, 0], "offset": object()}, "finite number"),
])
def test_invalid_primitive_records_are_refused(record, match):
    with pytest.raises((TypeError, ValueError), match=match):
        _capture(lambda: watch_api("bad", [record]))


def test_node_cap_refusal_does_not_add_a_partial_node(monkeypatch):
    monkeypatch.setattr(watch_module, "MAX_NODES", 1)
    token = watch_module._start()
    try:
        watch_api.point((0, 0, 0), name="kept")
        with pytest.raises(evp.WatchBudgetError, match="1 nodes"):
            watch_api.point((1, 1, 1), name="refused")
        payload = watch_module._complete(token)
    except BaseException:
        watch_module._abandon(token)
        raise
    assert [node["name"] for node in _decode(payload)] == ["kept"]


def test_frame_cap_refusal_keeps_contiguous_existing_frames(monkeypatch):
    monkeypatch.setattr(watch_module, "MAX_FRAMES_PER_NODE", 2)
    token = watch_module._start()
    try:
        watch_api.point((0, 0, 0), name="same")
        watch_api.point((1, 1, 1), name="same")
        with pytest.raises(evp.WatchBudgetError, match="2 frames"):
            watch_api.point((2, 2, 2), name="same")
        payload = watch_module._complete(token)
    except BaseException:
        watch_module._abandon(token)
        raise
    assert [frame["index"] for frame in _decode(payload)[0]["frames"]] == [0, 1]


def test_point_cap_is_cumulative_and_refuses_atomically(monkeypatch):
    monkeypatch.setattr(watch_module, "MAX_POINTS", 2)
    token = watch_module._start()
    try:
        watch_api.polyline([(0, 0, 0), (1, 1, 1)], name="kept")
        with pytest.raises(evp.WatchBudgetError, match="cap of 2"):
            watch_api.point((2, 2, 2), name="refused")
        payload = watch_module._complete(token)
    except BaseException:
        watch_module._abandon(token)
        raise
    assert [node["name"] for node in _decode(payload)] == ["kept"]


def test_persisted_byte_cap_refuses_without_mutating_the_trace(monkeypatch):
    token = watch_module._start()
    try:
        watch_api.label((0, 0, 0), "small", name="kept")
        current = watch_module._current.get().payload()
        monkeypatch.setattr(_watchstore, "MAX_PERSISTED_BYTES",
                            len(_watchstore.encode(current)) + 1)
        with pytest.raises(evp.WatchBudgetError, match="persisted-byte cap"):
            watch_api.label((1, 1, 1), "a much larger label", name="refused")
        payload = watch_module._complete(token)
    except BaseException:
        watch_module._abandon(token)
        raise
    assert [node["name"] for node in _decode(payload)] == ["kept"]


def test_store_replaces_atomically_and_an_oversize_save_keeps_prior_trace(
        tmp_path, monkeypatch):
    monkeypatch.setenv("EVP_HOME", str(tmp_path))
    folder = str(tmp_path / "Command")
    prior = {"version": 1, "nodes": ["prior"]}
    assert _watchstore.save(folder, prior)

    monkeypatch.setattr(_watchstore, "MAX_PERSISTED_BYTES", 8)
    assert not _watchstore.save(folder, {"version": 1, "nodes": ["replacement"]})
    assert _watchstore.load(folder) == prior


def test_store_keys_same_named_command_folders_by_full_path(tmp_path, monkeypatch):
    monkeypatch.setenv("EVP_HOME", str(tmp_path / "home"))
    first = str(tmp_path / "repo" / "Command")
    second = str(tmp_path / "deployed" / "Command")
    assert _watchstore.save(first, {"version": 1, "nodes": ["first"]})
    assert _watchstore.save(second, {"version": 1, "nodes": ["second"]})
    assert _watchstore.load(first)["nodes"] == ["first"]
    assert _watchstore.load(second)["nodes"] == ["second"]
