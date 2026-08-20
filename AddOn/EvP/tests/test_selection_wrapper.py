from types import SimpleNamespace
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))
from evp import selection


def test_selection_write_uses_typed_wire_ids_and_preserves_guid_result(monkeypatch):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={
            "selected": 1,
            "missing": [{"elementId": {"guid": "missing-guid"}}],
            "count": 1,
        })

    monkeypatch.setattr(selection, "call", fake_call)

    result = selection.set(["selected-guid"])

    assert seen == [("Tapioca.SetSelection", {
        "elements": [{"elementId": {"guid": "selected-guid"}}],
        "add": False,
    })]
    assert result["missing"] == ["missing-guid"]


def test_selection_clear_omits_elements(monkeypatch):
    seen = []

    def fake_call(command, params):
        seen.append((command, params))
        return SimpleNamespace(data={"selected": 0, "missing": [], "changed": 0, "count": 0})

    monkeypatch.setattr(selection, "call", fake_call)

    assert selection.clear()["count"] == 0
    assert seen == [("Tapioca.ModifySelection", {"op": "clear"})]


def test_highlight_puts_per_element_colors_on_typed_records(monkeypatch):
    seen = []

    def fake_call(command, params, raise_on_error=False):
        seen.append((command, params, raise_on_error))
        return SimpleNamespace(data={"count": 2})

    monkeypatch.setattr(selection, "call", fake_call)

    selection.highlight(["a", "b"], colors=[(1, 0, 0), (0, 1, 0, 0.5)])

    assert seen[0][0] == "Tapioca.HighlightElements"
    assert seen[0][1]["elements"] == [
        {"elementId": {"guid": "a"}, "color": [1.0, 0.0, 0.0, 1.0]},
        {"elementId": {"guid": "b"}, "color": [0.0, 1.0, 0.0, 0.5]},
    ]
