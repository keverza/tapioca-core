"""Regression tests for the API v2 native namespace transition."""
import importlib.util
import json
import os


_API_PATH = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage", "evp", "api.py")


def _load():
    spec = importlib.util.spec_from_file_location("_evp_api_undertest", _API_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _transport_for(seen):
    def transport(command, params_json):
        seen.append((command, json.loads(params_json)))
        return json.dumps({"ok": True, "data": {}, "meta": {}})

    return transport


def test_legacy_native_call_uses_canonical_tapioca_wire_namespace():
    api = _load()
    seen = []
    api._transport = lambda: _transport_for(seen)

    api.call("EvP.GetSelection")

    assert seen == [("Tapioca.GetSelection", {})]


def test_canonical_native_call_is_unchanged():
    api = _load()
    seen = []
    api._transport = lambda: _transport_for(seen)

    api.call("Tapioca.GetSelection", {"scope": "current"})

    assert seen == [("Tapioca.GetSelection", {"scope": "current"})]
