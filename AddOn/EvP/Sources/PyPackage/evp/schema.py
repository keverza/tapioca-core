"""Pydantic models for the values that cross a command's boundary.

Use a model for INPUTS, OUTPUTS, and the shared Archicad value types. Keep
`evp.api`, the context, and the command's own execution as ordinary Python
objects — a model that wraps the bus buys nothing and costs a validation pass
on every call.

    from tapioca.schema import NodeModel, ElementRef, Point3D, port, Field

    class Inputs(NodeModel):
        height: float = Field(
            default=2.8, gt=0,
            title="Height",
            description="Wall height, shown under the input row.",
            json_schema_extra=port(unit="m", control="number"),
        )

The palette does NOT read this file. It reads the JSON Schema the model emits
(`Inputs.model_json_schema()`), translated to the flat per-parameter shape the
DG side has always consumed by `evp._ports`. That indirection is the point: a
constraint written once as `gt=0` becomes a validated bound AND a spin-box
minimum, instead of being spelled twice and drifting.

⚠️ `import evp` must never fail — the AST scanner imports the package in
processes that have no transport and, on a fresh machine, no pip packages at
all. So `evp/__init__.py` does NOT import this module eagerly; a command that
wants it imports `tapioca.schema` and gets a clear error if the runtime
baseline has not been provisioned yet. Do not add `from . import schema` to the
package __init__.
"""

from __future__ import annotations

try:
    from pydantic import BaseModel, ConfigDict, Field
except ImportError as exc:                                  # pragma: no cover
    raise ImportError(
        "tapioca.schema needs pydantic, which ships in the Tapioca runtime "
        "baseline (evp/_env.py). If you are running outside Archicad, install "
        "it into the same interpreter. Original error: %s" % exc
    ) from exc

__all__ = ["NodeModel", "Point3D", "ElementRef", "Guid", "port", "output",
           "PORT_CONTROLS", "PORT_UNITS", "OUTPUT_ROLES", "Field"]


class NodeModel(BaseModel):
    """The base every command model derives from.

    `extra="forbid"` turns a misspelled or outdated port name into an error at
    the boundary. The palette sends what the schema declared, so an unexpected
    key means the command and its metadata have drifted — exactly the failure
    that otherwise shows up as a control that does nothing.

    `frozen=True` stops a node from mutating what it was handed. Inputs arrive
    from the palette and may be re-sent for a preview and again for the run; a
    command that edited them in place would preview one thing and commit
    another.
    """

    model_config = ConfigDict(extra="forbid", frozen=True)


def _normalize_guid(value):
    """`{1A2B...}` and `1a2b...` are the same element; upper-case is canonical.

    The wire carries both spellings depending on which side produced them, and
    comparing them raw silently fails to match. The api SPEC states this rule
    for the Python edge; this is the first place that enforces it rather than
    restating it.
    """
    return str(value).strip().strip("{}").upper()


class Guid(str):
    """An Archicad element GUID, normalized on the way in."""

    @classmethod
    def __get_pydantic_core_schema__(cls, source, handler):
        from pydantic_core import core_schema

        return core_schema.no_info_after_validator_function(
            lambda value: cls(_normalize_guid(value)),
            core_schema.str_schema(),
        )


class Point3D(NodeModel):
    """A point in project coordinates, in METRES — API base units throughout.

    `z` defaults to 0 because most plan-level geometry never sets it, not
    because zero is a floor level; a story-relative height is a different
    quantity and belongs in its own field.
    """

    x: float
    y: float
    z: float = 0.0


class ElementRef(NodeModel):
    """One Archicad element: its GUID plus the type that GUID is.

    The type travels with the reference because almost every consumer branches
    on it, and looking it up afterwards costs a bus round trip the producer had
    already paid for.
    """

    guid: Guid
    element_type: str


# The control vocabulary `port(control=...)` accepts, mapped to the parameter
# `type` the AST scanner has always emitted. `evp._ports` uses this table, so
# adding a control here and a Kind in Palette/ParamPanel.hpp is the whole job.
PORT_CONTROLS = {
    "number": "Float",
    "integer": "Int",
    "checkbox": "Bool",
    "text": "Text",
    "enum": "Enum",
    "action": "Action",
    "layer": "Layer",
    "pen": "Pen",
    "fill": "Fill",
    "linetype": "LineType",
    "surface": "Surface",
    "story": "Story",
    "filepath": "FilePath",
    "view": "View",
    "database": "Database",
    "project-field": "ProjectField",
    "library-part": "LibraryPart",
    "favourite": "Favourite",
    "building-material": "BuildingMaterial",
    "wall-composite": "WallComposite",
    "slab-composite": "SlabComposite",
    "roof-composite": "RoofComposite",
    "shell-composite": "ShellComposite",
    "wall-profile": "WallProfile",
    "beam-profile": "BeamProfile",
    "column-profile": "ColumnProfile",
    "handrail-profile": "HandrailProfile",
    "all-profile": "AllProfile",
}

# Units the palette converts FROM. API base units only — the DG edit reads the
# open project's Working Units and shows cm/mm/feet/degrees itself, so
# declaring unit="mm" or unit="deg" would apply the conversion twice.
PORT_UNITS = ("m", "m2", "m3", "rad")


def port(control=None, unit=None, default_from=None, readonly=None,
         show_when=None, subtype=None, numeric=None, element_type=None):
    """Build the `json_schema_extra` payload the palette reads off a field.

    Everything here is UI metadata that JSON Schema itself has no vocabulary
    for. Constraints and help text do NOT belong here — write those as ordinary
    `Field(ge=..., title=..., description=...)` so pydantic validates them too
    and there is one spelling of each.

    control       one of PORT_CONTROLS. Omitted, the control is inferred from
                  the field's Python type (float -> number, bool -> checkbox,
                  a Literal -> enum).
    unit          "m" | "m2" | "m3" | "rad". See PORT_UNITS.
    default_from  "project:<field name>" prefills the row when the command is
                  selected, before anything runs.
    readonly      show the value but refuse edits — for a number the project
                  owns rather than the user.
    show_when     {"<other field>": value_or_list}. Exactly one controller; a
                  hidden field is still sent to the command.
    subtype       library-part only: "Object" | "Lamp" | "ZoneStamp" | "Label".
    numeric       project-field only: list only fields that parse as numbers.
    element_type  favourite only: restrict to one element type.
    """
    if control is not None and control not in PORT_CONTROLS:
        raise ValueError(
            "unknown port control %r. Known: %s"
            % (control, ", ".join(sorted(PORT_CONTROLS)))
        )
    if unit is not None and unit not in PORT_UNITS:
        raise ValueError(
            "unit=%r is not an API base unit. Use one of %s — the palette "
            "converts to the project's Working Units itself, so a display unit "
            "here would be applied twice." % (unit, ", ".join(PORT_UNITS))
        )

    spec = {}
    for key, value in (("control", control), ("unit", unit),
                       ("default_from", default_from), ("readonly", readonly),
                       ("show_when", show_when), ("subtype", subtype),
                       ("numeric", numeric), ("element_type", element_type)):
        if value is not None:
            spec[key] = value
    return {"x-port": spec}


#: What a field on an OUTPUTS model is, to the framework.
#:
#:   table    the rows a standard export writes. At most one per model — a
#:            second would leave "Export CSV" with no way to say which.
#:   summary  a single headline value, shown beside the results.
#:   files    a list of Artifact dicts the run already wrote.
OUTPUT_ROLES = ("table", "summary", "files")


def output(role=None, label=None, unit=None):
    """Build the `json_schema_extra` payload for a field on an OUTPUTS model.

    The mirror of `port()`, and separate from it on purpose: an input's metadata
    is about a CONTROL (which widget, what prefills it, when it is hidden) and an
    output's is about a RESULT (what it means, what can be done with it). One
    helper covering both would offer every input field an `role="table"` that
    does nothing, and every output field a `show_when` with no row to hide.

        class Outputs(NodeModel):
            rows: list[Row] = Field(
                default=[], json_schema_extra=output(role="table"))

    `role="table"` is what lets a command declare `actions=["csv", "pdf"]` and
    write no export code — `evp.outputs.table_of()` finds the field by it.
    """
    if role is not None and role not in OUTPUT_ROLES:
        raise ValueError(
            "unknown output role %r. Known: %s"
            % (role, ", ".join(OUTPUT_ROLES)))
    if unit is not None and unit not in PORT_UNITS:
        raise ValueError(
            "unit=%r is not an API base unit. Use one of %s."
            % (unit, ", ".join(PORT_UNITS)))

    spec = {}
    for key, value in (("role", role), ("label", label), ("unit", unit)):
        if value is not None:
            spec[key] = value
    return {"x-output": spec}
