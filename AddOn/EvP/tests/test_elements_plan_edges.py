"""Contract tests for the floor-plan drawing primitive wrapper."""

import os
import sys

PACKAGE = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage")
if PACKAGE not in sys.path:
    sys.path.insert(0, PACKAGE)

from evp import elements  # noqa: E402


def test_plan_element_edges_preserves_alignment_and_paths(monkeypatch):
    sent = []

    class Result:
        data = {
            "elements": [
                {
                    "elementId": {"guid": "beam"},
                    "succeeded": True,
                    "paths": [{"points": [{"x": 1.0, "y": 2.0}, {"x": 3.0, "y": 4.0}]}],
                    "truncated": False,
                }
            ]
        }

    monkeypatch.setattr(elements, "call", lambda name, params: sent.append((name, params)) or Result())

    assert elements.plan_element_edges(["beam"]) == [
        {
            "guid": "beam",
            "succeeded": True,
            "error": "",
            "paths": [[(1.0, 2.0), (3.0, 4.0)]],
            "truncated": False,
        }
    ]
    assert sent == [("Tapioca.GetPlanElementEdges", {"elements": [{"elementId": {"guid": "beam"}}]})]


def test_plan_element_edges_skips_transport_for_empty_input(monkeypatch):
    monkeypatch.setattr(
        elements, "call", lambda *_args: (_ for _ in ()).throw(AssertionError("transport called for empty input"))
    )

    assert elements.plan_element_edges([]) == []
