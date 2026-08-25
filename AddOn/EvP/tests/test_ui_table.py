import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage"))

from evp import ui


def test_table_preserves_every_cell_in_each_row(monkeypatch):
    sent = {}

    def capture(command, payload, **kwargs):
        sent["command"] = command
        sent["payload"] = payload

    monkeypatch.setattr(ui, "call", capture)

    ui.table(
        ["Metric", "Value", "Unit"],
        [["Plot area", 12000, "m2"], ["FAR", 1.6]],
    )

    assert sent["command"] == "Tapioca.ShowResults"
    assert sent["payload"]["headers"] == ["Metric", "Value", "Unit"]
    assert [json.loads(row)["cells"] for row in sent["payload"]["rows"]] == [
        ["Plot area", "12000", "m2"],
        ["FAR", "1.6"],
    ]
