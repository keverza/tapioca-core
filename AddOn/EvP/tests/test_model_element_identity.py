"""A model element is geometry; it is not always a database element.

WHY THIS TEST EXISTS.  On 2026-09-18 a live 356-element project answered
`Tapioca.GetModelElements {types:["wall"], limit:4}` with four walls whose
`elementId.guid` was 00000000-0000-0000-0000-000000000000.  The caller edited
the first, Archicad returned APIERR_BADID, and the diagnostic reported THREE
OVERLAY FAILURES for a model the overlay had drawn perfectly -- the guid was
never the overlay's fault and never looked like a guid problem.

The mechanism is in the modeler, not in us.  `ModelerAPI::Element::GetElemId ()`
returns a `Modeler::VOCA`, "data binding one Model3D Elem to an external elem",
whose `etype` (what `typeName` reports) and `elemGuid` (what `elementId` reports)
are INDEPENDENT fields -- and VOCA has a `SetEmpty ()`.  So "wall" and "has a
database guid" are two different questions, and the command used to answer only
the first while implying the second.

A zero guid is shaped like an id and survives every emptiness check a caller can
write.  So the contract is ABSENCE, not a sentinel value: no `elementId` at all,
`addressable:false`, and a reason.
"""
import json
import os
import re

import pytest


_NATIVE = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands",
                       "ModelGeometryCommands.cpp")
_UTILS_HPP = os.path.join(os.path.dirname(__file__), "..", "Sources", "AddOn", "NativeCommands",
                          "ModelAccessUtils.hpp")


def _schemas():
    with open(_NATIVE, encoding="utf-8") as source:
        return [json.loads(value) for value in re.findall(
            r'R"json\((.*?)\)json"', source.read(), re.DOTALL)]


def _record_schema():
    # kEmptyInput, GetModelInfo out, GetModelElements in, GetModelElements out, …
    for schema in _schemas():
        defs = schema.get("$defs", {})
        if "modelElement" in defs:
            return defs["modelElement"]
    pytest.fail("no modelElement definition in ModelGeometryCommands.cpp schemas")


def test_element_id_is_optional_and_addressable_is_not():
    """The whole fix, stated as a schema property.

    If `elementId` ever goes back into `required`, the command has to emit
    SOMETHING for an unbound element, and the only thing it can emit is the zero
    guid this test exists to prevent.
    """
    record = _record_schema()
    assert "elementId" not in record["required"]
    assert "addressable" in record["required"]
    assert record["properties"]["addressable"]["type"] == "boolean"
    assert "unaddressableReason" in record["properties"]


def test_body_geometry_does_not_echo_an_identity_it_may_not_have():
    """GetBodyGeometry is reachable by `elementIndex`.

    That is precisely the path that reaches an unbound element, so it must not
    hand back an `elementId` the caller never supplied.
    """
    for schema in _schemas():
        props = schema.get("properties", {})
        if "bodyIndex" in props and "bodyCount" in props and "body" in props:
            assert "elementId" not in schema["required"]
            assert "addressable" in schema["required"]
            return
    pytest.fail("no GetBodyGeometry output schema found")


def test_the_mechanism_is_written_down_where_it_is_used():
    """The next reader must not have to re-derive this from the DevKit.

    It took reading Model3D/VOCA.hpp and Model3DImp.hpp to learn that type and
    identity are independent. That belongs next to the predicate, not in a
    commit message nobody greps.
    """
    text = open(_UTILS_HPP, encoding="utf-8").read()
    assert "bool HasElementGuid (const ModelerAPI::Element& elem);" in text
    assert "VOCA" in text


def test_dryrun_fixture_contains_an_unaddressable_element():
    """Offline must be able to fail the way the live project did.

    Every fixture row was addressable, which is why nothing offline could have
    caught this. A command that assumes `elementId` is present now breaks here.
    """
    import importlib.util
    import sys

    path = os.path.join(os.path.dirname(__file__), "dryrun_command.py")
    spec = importlib.util.spec_from_file_location("dryrun_command", os.path.abspath(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    rows = module._one("EvP.GetModelElements", {})["data"]["elements"]
    unbound = [r for r in rows if not r.get("addressable", True)]
    assert unbound, "the fixture no longer exercises an element with no database guid"
    for row in unbound:
        assert "elementId" not in row, "an unaddressable row must carry NO id, not a zero one"
        assert row["unaddressableReason"]
        assert row["index"] >= 1, "index is what makes such an element still readable"
