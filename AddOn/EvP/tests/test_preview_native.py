"""Native preview/watch wire and registration source contracts."""

import json
import re
from pathlib import Path


_ADDON = Path(__file__).parents[1] / "Sources" / "AddOn"
_SOURCE = _ADDON / "NativeCommands" / "PreviewCommands.cpp"


def _schemas():
    source = _SOURCE.read_text(encoding="utf-8")
    return {
        name: json.loads(value)
        for name, value in re.findall(
            r'constexpr const char (k\w+Schema)\[\] =\s*R"json\((.*?)\)json";',
            source,
            re.DOTALL,
        )
    }


def test_watch_top_level_preserves_stringified_node_wire_and_limits():
    schema = _schemas()["kWatchInputSchema"]

    assert schema["required"] == ["version", "nodes"]
    assert schema["additionalProperties"] is False
    assert schema["properties"]["version"] == {"type": "integer", "const": 1}
    assert schema["properties"]["nodes"] == {
        "type": "array", "maxItems": 64, "items": {"type": "string"}
    }


def test_decoded_watch_wire_is_strict_flat_xyz():
    schemas = _schemas()
    counts = {
        "kPointSchema": (3, 3),
        "kArrowSchema": (6, 6),
        "kDimensionSchema": (6, 6),
        "kAngleSchema": (9, 9),
        "kLabelSchema": (3, 3),
    }
    allowed = {"kind", "points", "text", "role", "closed", "direction", "offset"}
    for name, (minimum, maximum) in counts.items():
        schema = schemas[name]
        assert schema["additionalProperties"] is False
        assert set(schema["properties"]) == allowed
        assert schema["required"] == ["kind", "points"]
        assert schema["properties"]["points"]["minItems"] == minimum
        assert schema["properties"]["points"]["maxItems"] == maximum
        assert schema["properties"]["points"]["items"] == {"type": "number"}

    polyline = schemas["kPolylineSchema"]
    assert polyline["properties"]["points"]["minItems"] == 6
    element = schemas["kElementSchema"]
    assert "points" not in element["properties"]
    assert element["required"] == ["kind", "guid"]


def test_decoder_fails_closed_before_publishing():
    source = _SOURCE.read_text(encoding="utf-8")

    assert 'error = context + " is malformed JSON"' in source
    assert "unknown primitive kind" in source
    assert "pointCount > kMaxWatchPoints" in source
    assert source.index("DecodeWatchTrace (params") < source.index("PublishWatchTrace")
    assert source.index("DecodePreview (params") < source.index("PublishPreviewScene")


def test_preview_domain_is_registered_as_acapi_free():
    registry = (_ADDON / "NativeCommands" / "CommandRegistry.cpp").read_text(
        encoding="utf-8"
    )
    source = _SOURCE.read_text(encoding="utf-8")

    assert '#include "NativeCommands/PreviewCommands.hpp"' in registry
    assert "&GetPreviewCommandRegistrations" in registry
    assert len(re.findall(
        r"bool NeedsMainThread \(\) const override\s*\{\s*return false;\s*\}",
        source,
    )) == 2
