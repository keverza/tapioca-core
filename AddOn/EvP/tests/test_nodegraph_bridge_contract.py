"""The node-graph bridge contract, checked against the C++ it projects.

Every one of these pins a way the RESPONSE SCHEMA and the RUNTIME can drift
apart. That drift is not cosmetic: a response the schema refuses is a failed
command, and the first thing the editor does on open is ask for the catalog - so
one unlisted enum value makes the palette unusable. It happened, with
`display: "selectionSet"`, and nothing offline noticed because no test compared
the two sides.
"""

import re
from pathlib import Path

import pytest

_SOURCES = Path(__file__).resolve().parents[1] / "Sources" / "AddOn"
_REGISTRY = _SOURCES / "NodeGraph" / "NodeRegistry.cpp"
_NODETYPE = _SOURCES / "NodeGraph" / "NodeType.hpp"
_COMMANDS = _SOURCES / "NativeCommands" / "NodeGraphCommands.cpp"


def _catalog_schema() -> str:
    """The one raw JSON literal assigned to kCatalogResponseSchema."""
    text = _COMMANDS.read_text(encoding="utf-8")
    match = re.search(
        r"kCatalogResponseSchema\[\]\s*=\s*R\"json\((.*?)\)json\"", text, re.DOTALL
    )
    assert match is not None, "kCatalogResponseSchema is no longer a raw JSON literal"
    return match.group(1)


def _names_returned_by(function: str, source: Path) -> set[str]:
    """The string literals a `const char* Name (Enum)` switch returns."""
    text = source.read_text(encoding="utf-8")
    match = re.search(
        rf"const char\* {function} \([^)]*\)\s*\{{(.*?)\n\}}", text, re.DOTALL
    )
    assert match is not None, f"{function} not found in {source.name}"
    return set(re.findall(r'return "([^"]+)";', match.group(1)))


def _enum_in_schema(schema: str, property_name: str) -> set[str]:
    match = re.search(
        rf'"{property_name}":\{{"type":"string","enum":\[([^\]]*)\]\}}', schema
    )
    assert match is not None, f"{property_name} has no string enum in the catalog schema"
    return set(re.findall(r'"([^"]+)"', match.group(1)))


@pytest.mark.parametrize(
    ("function", "source_name", "schema_property"),
    [
        ("NodeDisplayName", "NodeRegistry.cpp", "display"),
        ("EffectKindName", "NodeRegistry.cpp", "effect"),
    ],
)
def test_catalog_schema_admits_every_value_the_runtime_can_emit(
    function, source_name, schema_property
):
    emitted = _names_returned_by(function, _SOURCES / "NodeGraph" / source_name)
    allowed = _enum_in_schema(_catalog_schema(), schema_property)
    missing = emitted - allowed
    assert not missing, (
        f"{function} can return {sorted(missing)}, which the catalog response "
        f"schema's '{schema_property}' enum does not allow. A node type using it "
        f"makes Tapioca.GraphGetNodeTypes fail its own schema, and that is the "
        f"first call the editor makes."
    )


def test_every_node_display_value_reaches_the_catalog_schema():
    """The enum itself, not just the names function, so a new member is caught."""
    members = re.findall(
        r"^\s{4}(\w+),", _NODETYPE.read_text(encoding="utf-8").split("enum class NodeDisplay {")[1].split("};")[0],
        re.MULTILINE,
    )
    assert len(members) >= 3, "NodeDisplay members could not be read"
    names = _names_returned_by("NodeDisplayName", _REGISTRY)
    assert len(names) == len(members), (
        f"NodeDisplay has {len(members)} members but NodeDisplayName returns "
        f"{len(names)} names - a member added without a name falls through to "
        f"the default and is silently mislabelled."
    )


def test_a_catalog_default_value_uses_the_response_encoding():
    """defaultValue is OUTBOUND data and must not be typed as inbound.

    `parameterValue` is the INBOUND schema and deliberately admits scalars only,
    because a graph is authored from small typed values. The catalog is a
    response, so a node whose parameter is a list could not describe its default
    at all while defaultValue pointed at it.
    """
    schema = _catalog_schema()
    assert '"defaultValue":{"$ref":"#/$defs/value"}' in schema, (
        "the catalog's defaultValue must reference $defs/value, the response "
        "encoding, not $defs/parameterValue, the request one"
    )


def test_the_graph_command_boundary_uses_the_fault_barrier():
    """Constraint 3: a node-graph error must never take Archicad down.

    `catch (...)` does not catch a structured exception under /EHsc, so an
    access violation anywhere in the runtime used to reach Archicad's stack.
    Every Tapioca.Graph* verb goes through this one wrapper, so this pins it.
    """
    support = (_SOURCES / "NativeCommands" / "NodeGraphCommandSupport.hpp").read_text(
        encoding="utf-8"
    )
    assert "graph::RunGuarded" in support, (
        "GateFreeGraphCommand must run its handler behind RunGuarded; a plain "
        "try/catch lets a structured exception terminate Archicad"
    )
    body = support.split("class GateFreeGraphCommand")[1]
    assert "catch (...)" not in body, (
        "a bare catch(...) in the graph command boundary reads as total "
        "containment and is not - RunGuarded is what makes it total"
    )


def test_the_editor_page_is_served_not_pushed_through_navigatetostring():
    """A 2 MB cap that kills Archicad must not be reintroduced.

    ICoreWebView2::NavigateToString accepts at most 2 MB, and its argument is
    UTF-16 - so an ASCII bundle hits the cap at about a million characters. On
    2026-08-31 the editor bundle reached 1,049,720 characters, crossed it, and
    opening the palette TERMINATED ARCHICAD. Serving the page from a virtual
    origin through a resource handler has no such cap, so the ceiling is gone
    rather than moved; this pins that it stays gone.
    """
    host = (
        Path(__file__).resolve().parents[1]
        / "Sources"
        / "AddOn"
        / "Palette"
        / "WebView2GraphHost.cpp"
    ).read_text(encoding="utf-8")

    # The CALL, not the word: the comment explaining why the call is gone is
    # the most useful line in that file and must not trip its own guard.
    assert not re.search(r"->\s*NavigateToString\s*\(", host), (
        "the editor page must not go through NavigateToString: it caps at 2 MB "
        "of UTF-16 and took Archicad down when the bundle outgrew it"
    )
    assert "add_WebResourceRequested" in host, (
        "the editor page is served from a virtual origin through a resource "
        "handler; that is what removes the size ceiling"
    )


def test_palette_items_exist_before_dg_event_processing_starts():
    """Nothing an event handler touches may be built after event processing.

    BeginEventProcessing makes DG deliver panel events to the observer the
    constructor just attached, and DG delivers a PanelResized immediately when
    the restored palette geometry differs from the resource default - which
    depends on where the user last left the window, the monitor and the DPI. On
    2026-08-31 `surface` was created after that call and `PanelResized`
    dereferenced it: an access violation inside BeginEventProcessing that took
    Archicad down, with no Windows Error Reporting entry and no Archicad crash
    report to name it.
    """
    palette = (
        Path(__file__).resolve().parents[1]
        / "Sources"
        / "AddOn"
        / "Palette"
        / "GraphEditorPalette.cpp"
    ).read_text(encoding="utf-8")

    body = palette.split("GraphEditorPalette::GraphEditorPalette ()")[1].split(chr(10) + "}")[0]
    begin = body.index("BeginEventProcessing ()")
    for member in ("surface = std::make_unique", "webView = std::make_unique"):
        assert member in body, f"{member!r} is no longer built in the constructor"
        assert body.index(member) < begin, (
            f"{member!r} is constructed AFTER BeginEventProcessing, so a panel "
            f"event delivered during that call dereferences a null member and "
            f"terminates Archicad"
        )

    # The handler that actually crashed keeps its own guard, because the
    # ordering above is one refactor away from being undone.
    resized = palette.split("GraphEditorPalette::PanelResized")[1].split(chr(10) + "}")[0]
    assert "surface == nullptr" in resized, (
        "PanelResized must null-check surface before dereferencing it"
    )
