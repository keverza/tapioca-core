"""Offline checks for the deferred-binding wire protocol."""
import importlib.util
import json
import os
import sys
import types


_PACKAGE_DIR = os.path.join(os.path.dirname(__file__), "..", "Sources", "PyPackage", "evp")


def _load():
    package = types.ModuleType("_evp_tx_test")
    package.__path__ = [_PACKAGE_DIR]
    sys.modules[package.__name__] = package

    api = types.ModuleType("_evp_tx_test.api")
    api.LEGACY_NATIVE_NAMESPACE = "EvP"
    api.NATIVE_NAMESPACE = "Tapioca"
    api.call = lambda *args, **kwargs: None
    sys.modules[api.__name__] = api

    path = os.path.join(_PACKAGE_DIR, "transaction.py")
    spec = importlib.util.spec_from_file_location("_evp_tx_test.transaction", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _binding(step, index=0):
    return json.loads(step["bindings"][index])


def test_top_level_guid_wire_shape_is_preserved():
    mod = _load()
    tx = mod.Transaction("test")
    handle = tx.call("EvP.Create", {})
    tx.call("EvP.Update", {"element": handle.guid})

    assert tx._steps[1]["params"] == {}
    assert _binding(tx._steps[1]) == {"path": "element", "step": 0, "key": "guid"}


def test_nested_source_and_target_paths_are_encoded():
    mod = _load()
    tx = mod.Transaction("test")
    handle = tx.call("EvP.Create", {})
    params = {
        "inheritFrom": {"elementId": {"guid": handle.elementId.guid}},
        "enabled": handle.enabled,
    }
    tx.call("EvP.Create", params)

    assert tx._steps[1]["params"] == {"inheritFrom": {"elementId": {}}}
    assert isinstance(params["inheritFrom"]["elementId"]["guid"], mod.Ref)
    assert [_binding(tx._steps[1], i) for i in range(2)] == [
        {"path": "inheritFrom.elementId.guid", "step": 0, "key": "elementId.guid"},
        {"path": "enabled", "step": 0, "key": "enabled"},
    ]


def test_complete_object_binding_uses_the_same_protocol():
    mod = _load()
    tx = mod.Transaction("test")
    handle = tx.call("EvP.Create", {})
    tx.call("EvP.Update", {"element": handle.elementId})

    assert _binding(tx._steps[1]) == {"path": "element", "step": 0, "key": "elementId"}


def test_refs_inside_lists_are_rejected_explicitly():
    mod = _load()
    tx = mod.Transaction("test")
    handle = tx.call("EvP.Create", {})

    try:
        tx.call("EvP.Update", {"elements": [handle.elementId]})
    except mod.TransactionError as exc:
        assert "inside a list" in str(exc)
    else:
        raise AssertionError("list binding should have failed")
