"""Translate a command's JSON Schema into the flat per-parameter metadata the
palette has always consumed.

This is the adapter that lets a Pydantic command drive the EXISTING DG
controls. `Palette/ParamPanel.cpp` decodes one flat ObjectState per parameter
and branches on a `type` string — `"Float"`, `"Layer"`, `"Enum"`, and so on.
Nothing about that changes here; only where the values come from does.

    Inputs.model_json_schema()  ->  ports_from_schema()  ->  [{"name": ..., "type": ...}, ...]

The output must be INDISTINGUISHABLE from what `evp._scanner` emits for an
equivalent signature-style command, because both feed the same C++ decode.
`tests/test_ports.py` asserts exactly that for every control kind; if you add a
key here, add it there.

⚠️ The C++ side reads a param as a FLAT ObjectState of scalars and string
arrays. A nested dict or a nested array arrives as NOTHING — no error, just a
control that is always visible or a popup with no items in it. So `show_when`
is flattened to two keys and enum choices go out as a flat `args` array, the
same shapes `_scanner._fold_show_when` produces and for the same reason.
"""

from __future__ import annotations

__all__ = ["ports_from_schema", "PortError"]


class PortError(ValueError):
    """A schema the palette could not turn into controls.

    Raised, never swallowed: a port that cannot be rendered is a row the user
    never sees, which looks exactly like a command that ignores its input.
    """


# JSON Schema `type` -> the parameter `type` string ParamPanel branches on,
# for fields that declare no explicit x-port control.
_INFERRED = {
    "number": "Float",
    "integer": "Int",
    "boolean": "Bool",
    "string": "Text",
}


def _auto_title(name):
    """The title pydantic invents for a field the author did not title.

    Mirrors pydantic v2's own derivation (`name.title()` then underscores to
    spaces). Used ONLY as a fallback, when the caller could not tell us which
    titles were real — see `explicit_titles` on ports_from_schema.
    """
    return name.title().replace("_", " ")


def _value_text(value):
    """One show_when value, spelled the way the palette spells a live control.

    A checkbox reads back as "true"/"false" on the C++ side, so a Python
    `False` has to normalise to the same string here — otherwise the comparison
    never matches and the row simply never appears. Identical to
    `_scanner._value_text`; the two are kept in step by test_ports.py.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _resolve(node, schema):
    """Follow a `$ref` into `$defs`.

    Pydantic hoists a nested model or a shared Enum into `$defs` and leaves a
    `$ref` behind, sometimes wrapped in a single-branch `allOf`/`anyOf` when
    the field also carries a default or a description.
    """
    seen = 0
    while True:
        if "$ref" in node:
            name = node["$ref"].rsplit("/", 1)[-1]
            target = (schema.get("$defs") or {}).get(name)
            if target is None:
                raise PortError("schema $ref %r has no definition" % node["$ref"])
            merged = dict(target)
            # Keys written on the field itself (default, title, description,
            # x-port) win over the referenced definition's.
            for key, value in node.items():
                if key != "$ref":
                    merged[key] = value
            node = merged
        elif len(node.get("allOf", ())) == 1:
            branch = dict(node["allOf"][0])
            branch.update({k: v for k, v in node.items() if k != "allOf"})
            node = branch
        else:
            return node
        seen += 1
        if seen > 16:
            raise PortError("schema $ref chain does not terminate")


def _choices(node):
    """The closed set of values a field can hold, as flat strings, or None.

    A `Literal[...]` lands as `enum` directly; a Python Enum class lands as a
    `$ref` into `$defs` that `_resolve` has already inlined by the time we get
    here.
    """
    values = node.get("enum")
    if values is None:
        return None
    out = []
    for value in values:
        if isinstance(value, (list, tuple, dict)):
            raise PortError(
                "a choice may not be a %s: the palette reads choices as a flat "
                "array of strings, and a nested one arrives as an EMPTY popup."
                % type(value).__name__
            )
        out.append(_value_text(value))
    return out


def _bounds(node, entry):
    """Copy numeric bounds across, exclusive ones included.

    ⚠️ `DG::RealEdit::SetMin` is INCLUSIVE and DG has no exclusive form, so a
    pydantic `gt=0` becomes `minimum: 0` on the control. The spinner will let
    the user reach exactly 0; pydantic then rejects it at the boundary with the
    real message. Clamping is a convenience, validation is the contract — and
    silently widening the bound to keep them consistent would be worse.
    """
    for source, target in (("minimum", "minimum"), ("exclusiveMinimum", "minimum"),
                           ("maximum", "maximum"), ("exclusiveMaximum", "maximum")):
        if source in node and target not in entry:
            entry[target] = node[source]


def ports_from_schema(schema, labels=None, explicit_titles=None):
    """`Inputs.model_json_schema()` -> the palette's parameter list.

    `labels` is the command decorator's optional {field: "shown text"}. A field
    that sets `title=` in its own Field() already carries its label; the
    decorator's entry wins, so a command can relabel a model it did not write.

    `explicit_titles` is the set of field names whose `title=` the author
    actually wrote. Pydantic titles EVERY field, so without this the only way to
    tell an authored title from a generated one is to compare it against the
    generated form — and that guess is wrong exactly when the author's choice
    AGREES with it. `title="Spacing"` on a field named `spacing` was dropped as
    "auto" and the row rendered as the raw identifier "spacing", live in
    Archicad. `evp._schemagen` reads the real answer off `model_fields` and
    passes it; the comparison stays as the fallback for a caller holding only a
    schema.
    """
    properties = schema.get("properties")
    if not properties:
        raise PortError(
            "the inputs model produced no 'properties'. An Inputs model with no "
            "fields is a command with no controls — declare the ports or drop "
            "inputs= from the decorator."
        )

    required_names = set(schema.get("required") or ())
    labels = labels or {}
    params = []

    for name, raw in properties.items():
        node = _resolve(raw, schema)
        spec = dict(node.get("x-port") or {})
        entry = {"name": name}

        control = spec.pop("control", None)
        choices = _choices(node)

        if control is not None:
            from .schema import PORT_CONTROLS

            entry["type"] = PORT_CONTROLS[control]
        elif choices is not None:
            entry["type"] = "Enum"
        else:
            json_type = node.get("type")
            if json_type not in _INFERRED:
                raise PortError(
                    "field %r has JSON type %r, which has no control. Give it an "
                    "explicit json_schema_extra=port(control=...) or use a type "
                    "the palette can render: %s"
                    % (name, json_type, ", ".join(sorted(_INFERRED)))
                )
            entry["type"] = _INFERRED[json_type]

        if entry["type"] in ("Enum", "Action"):
            if not choices:
                raise PortError(
                    "field %r renders as a popup but declares no choices. Use a "
                    "Literal[...] or a str Enum so the values are in the schema."
                    % name
                )
            entry["args"] = choices

        if name in required_names:
            entry["required"] = True
        else:
            entry["required"] = False
            if "default" in node:
                entry["default"] = node["default"]

        _bounds(node, entry)

        # UI metadata that JSON Schema has no vocabulary for. `readonly` and
        # `numeric` stay booleans; the C++ side reads them with os.Get(bool).
        for key in ("unit", "default_from", "readonly", "subtype", "numeric",
                    "element_type", "mode", "extensions"):
            if key in spec:
                entry[key] = spec[key]

        show_when = spec.get("show_when")
        if show_when is not None:
            if not isinstance(show_when, dict) or len(show_when) != 1:
                raise PortError(
                    "show_when on %r must name exactly ONE controlling field, "
                    'e.g. show_when={"action": "Place"}. Two conditions would '
                    "need a nested shape the palette cannot read back." % name
                )
            controller, wanted = next(iter(show_when.items()))
            if controller == name:
                raise PortError(
                    "%r has a show_when on itself: a control cannot decide "
                    "whether it is visible." % name
                )
            values = wanted if isinstance(wanted, (list, tuple)) else [wanted]
            entry["show_when_param"] = controller
            entry["show_when_values"] = [_value_text(v) for v in values]

        label = labels.get(name)
        if not label:
            title = node.get("title")
            # A label appears only when someone CHOSE it: an auto-title would
            # give the schema path a display string the signature path does not
            # have, so the same command ported over would render different text.
            if title:
                if explicit_titles is not None:
                    authored = name in explicit_titles
                else:
                    authored = title != _auto_title(name)
                if authored:
                    label = title
        if label:
            entry["label"] = label
        # Field(description=...) becomes the per-parameter help line. The
        # command-level description is a different string and a different band.
        if node.get("description"):
            entry["help"] = node["description"]

        params.append(entry)

    _check_show_when(params)
    return params


def _check_show_when(params):
    """Every show_when must name a real field and a value it can actually hold.

    The same three failures `_scanner._fold_show_when` refuses, for the same
    reason: a show_when nobody can satisfy is a control that never appears, and
    as a diagnostic that is a five-second fix while as silence it is an
    afternoon.
    """
    by_name = {p["name"]: p for p in params}

    actions = [p for p in params if p["type"] == "Action"]
    if len(actions) > 1:
        raise PortError(
            "the inputs model declares %d action ports (%s), but a command has "
            "ONE mode. Use control='enum' for a second choice — only the action "
            "is pinned to the top of the block and reflows the rows below it."
            % (len(actions), ", ".join(p["name"] for p in actions))
        )

    for param in params:
        controller_name = param.get("show_when_param")
        if controller_name is None:
            continue
        controller = by_name.get(controller_name)
        if controller is None:
            raise PortError(
                "show_when on %r names %r, which is not a field of the inputs "
                "model. It would be a control that never appears."
                % (param["name"], controller_name)
            )
        if controller["type"] in ("Enum", "Action"):
            allowed = controller.get("args", [])
        elif controller["type"] in ("Bool", "bool"):
            allowed = ["true", "false"]
        else:
            continue                       # an open type cannot be checked ahead
        for value in param["show_when_values"]:
            if value not in allowed:
                raise PortError(
                    "show_when on %r wants %r=%r, but %r can only be %s."
                    % (param["name"], controller_name, value, controller_name,
                       ", ".join(repr(a) for a in allowed))
                )
