"""The overlays in Archicad's own views: layers of geometry, text, legends and HUD panels.

    from evp import overlay

    panel = overlay.Panel(title="Sun study", anchor="top-right")
    panel.row("Date", "21 June").row("Hours of sun", "8.0 h").separator()
    panel.ramp("sunhours", 0, 8, unit="h", title="Sun hours")
    panel.swatch("#FEC44F", "over 6 h").swatch("#253494", "under 2 h")

    overlay.set_layer("sun", views="both",
                      meshes=[overlay.heatmap(points, indices, values, "sunhours")],
                      legends=[overlay.legend("sunhours", 0, 8, title="Sun hours", unit="h",
                                              horizontal=True, screen=(0.5, 1.0), corner="bottom-left")],
                      panels=[panel])
    overlay.clear("sun")

A layer is drawn by both overlays -- the floor plan and the 3D window -- through each
one's own transform (`views` picks). Coordinates are MODEL METRES: a single point is a
{x, y, z} record on the wire (a tuple here), bulk geometry flat x, y, z. `occlusion` says
what an item does behind the building in 3D: "hide", "fade", "dash" (lines) or
"always"; the layer's own is the default for its items. A HUD panel is laid
out by Dear ImGui and never takes a click; a legend is a colour bar with its values, and
a panel's `ramp` is the same bar inside a panel, for a layout of one's own.

Colours are "RRGGBB[AA]" hex, "#" optional, or an (r, g, b[, a]) tuple in 0..1. Numbers
are sent as REALS: a JSON whole number reaches the add-on as an integer, which the
overlay reads either way but most verbs do not (see NativeCommands/CommandUtils.hpp).
"""

from .api import call

__all__ = [
    "Panel",
    "colour",
    "point2",
    "point3",
    "polyline",
    "points",
    "mesh",
    "heatmap",
    "text",
    "dimension",
    "legend",
    "set_layer",
    "clear",
    "clear_all",
    "layers",
    "hud",
    "story_slices",
    "annotations",
]

# The keys whose numbers ARE integers on the wire; every other number is sent as a real.
_INTEGER_KEYS = {"indices", "ticks", "decimals", "bands", "mesh", "storeys"}

_ANCHORS = ("top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right")

_OCCLUSIONS = ("hide", "fade", "dash", "always")

_TERMINATORS = ("none", "arrow", "tick", "dot")


def colour(value):
    """ "RRGGBBAA" from "#RRGGBB", "#RRGGBBAA", "RRGGBBAA" or an (r, g, b[, a]) tuple.

    A tuple is 0..1 per component, as evp.selection takes it; a tuple whose components
    are above 1 is read as 0..255.
    """
    if isinstance(value, str):
        hexdigits = value.lstrip("#")
        if len(hexdigits) == 6:
            hexdigits += "FF"
        if len(hexdigits) != 8:
            raise ValueError("a colour is RRGGBB or RRGGBBAA, got %r" % value)
        int(hexdigits, 16)  # raises on anything that is not hex
        return hexdigits.upper()
    components = list(value)
    if len(components) == 3:
        components.append(1.0 if max(components) <= 1.0 else 255)
    if len(components) != 4:
        raise ValueError("a colour tuple is (r, g, b) or (r, g, b, a), got %r" % (value,))
    scale = 255.0 if max(components) <= 1.0 else 1.0
    return "".join("%02X" % max(0, min(255, int(round(float(c) * scale)))) for c in components)


def _point(value, axes):
    """A {x, y[, z]} record from a tuple, a list or a record."""
    if isinstance(value, dict):
        return {axis: float(value[axis]) for axis in axes}
    values = list(value)
    if len(values) != len(axes):
        raise ValueError("a point is %s, got %r" % (", ".join(axes), value))
    return {axis: float(v) for axis, v in zip(axes, values, strict=True)}


def point3(value):
    """{x, y, z} from (x, y, z) -- the wire's #Point3D."""
    return _point(value, ("x", "y", "z"))


def point2(value):
    """{x, y} from (x, y) -- the wire's #Point2D."""
    return _point(value, ("x", "y"))


def _occlusion(value):
    if value is not None and value not in _OCCLUSIONS:
        raise ValueError("occlusion is one of %s" % ", ".join(_OCCLUSIONS))
    return value


def _flat(points):
    """Flat x, y, z from a flat list or a list of (x, y, z)."""
    values = list(points)
    if values and isinstance(values[0], (list, tuple, dict)):
        values = [c for p in values for c in point3(p).values()]
    return [float(v) for v in values]


def _terminator(value):
    if value is not None and value not in _TERMINATORS:
        raise ValueError("a line ends in one of %s" % ", ".join(_TERMINATORS))
    return value


def _put(out, pairs, convert=None):
    for key, value in pairs:
        if value is not None:
            out[key] = convert(value) if convert else value
    return out


def _coloured(fields, keys=("color", "halo", "textColor")):
    """A copy of a style dict with its colours as the wire's, nested dicts included."""
    out = dict(fields or {})
    for key, value in out.items():
        if key in keys and value is not None:
            out[key] = colour(value)
        elif isinstance(value, dict):
            out[key] = _coloured(value, keys)
    return out


def _reals(value, key=None):
    """Every number a real unless its key says integer; bools and strings untouched."""
    if isinstance(value, dict):
        return {k: _reals(v, k) for k, v in value.items() if v is not None}
    if isinstance(value, (list, tuple)):
        return [_reals(v, key) for v in value]
    if isinstance(value, bool) or isinstance(value, str):
        return value
    if isinstance(value, int) and key not in _INTEGER_KEYS:
        return float(value)
    return value


def _colormap(colormap, minimum=None, maximum=None, bands=None, isolines=None):
    """A preset name, a list of (at, colour) stops, or a ready dict."""
    if isinstance(colormap, dict):
        out = dict(colormap)
    elif isinstance(colormap, str):
        out = {"preset": colormap}
    else:
        out = {"stops": [{"at": float(at), "color": colour(c)} for at, c in colormap]}
    if minimum is not None or maximum is not None:
        if minimum is None or maximum is None:
            raise ValueError("a ramp's range is min and max together")
        out["min"], out["max"] = float(minimum), float(maximum)
    if bands:
        out["bands"] = int(bands)
    if isolines:
        out["isolines"] = dict(isolines)
        if "color" in out["isolines"]:
            out["isolines"]["color"] = colour(out["isolines"]["color"])
    return out


class Panel:
    """A HUD panel: its items in order, laid out by Dear ImGui in a corner (or any of
    nine anchor points) of the view. Every method returns the panel, so they chain."""

    def __init__(
        self,
        title=None,
        anchor="top-left",
        offset=(16, 16),
        width=None,
        size=None,
        color=None,
        background=None,
        border=None,
        rounding=None,
        padding=None,
        font=None,
    ):
        if anchor not in _ANCHORS:
            raise ValueError("anchor is one of %s" % ", ".join(_ANCHORS))
        self._panel = {"anchor": anchor, "offsetPixels": point2(offset)}
        if title:
            self._panel["title"] = str(title)
        if font:
            self._panel["font"] = str(font)
        for key, value in (
            ("widthPixels", width),
            ("sizePixels", size),
            ("roundingPixels", rounding),
            ("paddingPixels", padding),
        ):
            if value is not None:
                self._panel[key] = float(value)
        for key, value in (("color", color), ("background", background), ("border", border)):
            if value is not None:
                self._panel[key] = colour(value)
        self._items = []

    def _add(self, kind, **fields):
        item = {"kind": kind}
        for key, value in fields.items():
            if value is None:
                continue
            item[key] = colour(value) if key == "color" else value
        self._items.append(item)
        return self

    def text(self, text, color=None, size=None, wrap=False):
        return self._add("text", text=str(text), color=color, sizePixels=size, wrap=bool(wrap) or None)

    def row(self, label, value, color=None, size=None):
        """A label and its value; consecutive rows line up in two columns."""
        return self._add("row", text=str(label), value=str(value), color=color, sizePixels=size)

    def separator(self):
        return self._add("separator")

    def spacing(self, height=6):
        return self._add("spacing", heightPixels=float(height))

    def progress(self, fraction, text=None, color=None, width=None, height=None):
        return self._add(
            "progress", fraction=float(fraction), text=text, color=color, widthPixels=width, heightPixels=height
        )

    def swatch(self, color, text):
        """A key entry: a square of `color` and what it means."""
        return self._add("swatch", color=color, text=str(text))

    def ramp(
        self,
        colormap,
        minimum,
        maximum,
        title=None,
        unit=None,
        ticks=None,
        decimals=None,
        tick_values=None,
        tick_labels=None,
        bands=None,
        width=None,
        height=None,
        color=None,
    ):
        """A colour bar over [minimum, maximum] with its ticks: a legend inside the panel."""
        return self._add(
            "ramp",
            colormap=_colormap(colormap, minimum, maximum, bands),
            text=title,
            unit=unit,
            ticks=ticks,
            decimals=decimals,
            tickValues=[float(v) for v in tick_values] if tick_values is not None else None,
            tickLabels=[str(v) for v in tick_labels] if tick_labels is not None else None,
            widthPixels=width,
            heightPixels=height,
            color=color,
        )

    def plot(self, values, caption=None, minimum=None, maximum=None, color=None, width=None, height=None):
        if (minimum is None) != (maximum is None):
            raise ValueError("a plot's range is minimum and maximum together")
        return self._add(
            "plot",
            values=[float(v) for v in values],
            text=caption,
            min=minimum,
            max=maximum,
            color=color,
            widthPixels=width,
            heightPixels=height,
        )

    def table(self, columns, rows):
        return self._add(
            "table",
            columns=[str(c) for c in columns] if columns else None,
            rows=[[str(cell) for cell in row] for row in rows],
        )

    def to_dict(self):
        out = dict(self._panel)
        out["items"] = [dict(item) for item in self._items]
        return out


def polyline(
    points,
    color=None,
    width=None,
    closed=None,
    dash=None,
    occlusion=None,
    start_arrow=None,
    end_arrow=None,
    arrow_size=None,
    hidden_color=None,
    hidden_width=None,
    hidden_dash=None,
):
    """A line through `points` (flat x, y, z or a list of points), `width` pixels wide.

    `dash` is a pattern in model METRES along the line -- on, off, on, off... ([0.5, 0.3],
    or [1.0, 0.25, 0.1, 0.25] for a dash-dot) -- anchored at its start, so it stays on the
    model as the camera moves. `hidden_color`, `hidden_width` and `hidden_dash` draw the
    part behind the building, where `occlusion` is "dash" (default pattern 0.5 m on,
    0.3 m off) or "fade". `start_arrow` and `end_arrow` end an open one in "arrow", "tick"
    or "dot", `arrow_size` pixels long.
    """
    out = {"points": _flat(points)}
    _put(out, (("closed", closed),))
    _put(out, (("color", color),), colour)
    _put(
        out,
        (("widthPixels", width), ("arrowSizePixels", arrow_size)),
        float,
    )
    if dash is not None:
        out["dashMetres"] = [float(v) for v in dash]
    hidden = _put({}, (("color", hidden_color),), colour)
    _put(hidden, (("widthPixels", hidden_width),), float)
    if hidden_dash is not None:
        hidden["dashMetres"] = [float(v) for v in hidden_dash]
    if hidden:
        out["hidden"] = hidden
    _put(
        out,
        (
            ("occlusion", _occlusion(occlusion)),
            ("startArrow", _terminator(start_arrow)),
            ("endArrow", _terminator(end_arrow)),
        ),
    )
    return out


def points(points, color=None, size=None, size_metres=None):
    """Markers at `points`: squares `size` pixels wide in plan, crosses `size_metres` long in 3D."""
    out = {"points": _flat(points)}
    _put(out, (("color", color),), colour)
    return _put(out, (("sizePixels", size), ("sizeMetres", size_metres)), float)


def mesh(
    points,
    indices,
    color=None,
    vertex_colors=None,
    normals=None,
    shading=None,
    opacity=None,
    occlusion=None,
    cull=None,
    edges=None,
    edge_width=None,
    edge_angle=None,
):
    """Triangles: flat x, y, z points and three indices each.

    `shading` is "flat", "lit", "ghost" or "xray"; `edges` a colour draws its feature
    edges -- boundaries and creases sharper than `edge_angle` degrees -- `edge_width` wide.
    """
    out = {"points": _flat(points), "indices": [int(i) for i in indices]}
    _put(out, (("color", color),), colour)
    if vertex_colors is not None:
        out["vertexColors"] = [colour(c) for c in vertex_colors]
    if normals is not None:
        out["normals"] = _flat(normals)
    style = _put({}, (("shading", shading), ("occlusion", _occlusion(occlusion)), ("cull", cull)))
    _put(style, (("opacity", opacity),), float)
    if edges is not None:
        style["edges"] = _put(
            {"color": colour(edges)}, (("widthPixels", edge_width), ("angleDegrees", edge_angle)), float
        )
    if style:
        out["style"] = style
    return out


def dimension(
    start,
    end,
    offset=None,
    direction=None,
    normal=None,
    text=None,
    decimals=None,
    unit=None,
    show_unit=None,
    color=None,
    width=None,
    text_size=None,
    text_color=None,
    halo=None,
    halo_size=None,
    terminator=None,
    terminator_size=None,
    occlusion=None,
    font=None,
):
    """An aligned dimension from `start` to `end`, its line `offset` metres off towards
    `direction`, in the plane `normal` names. `text` replaces the measured length;
    `terminator` is "tick", "arrow", "dot" or "none", `terminator_size` pixels long.
    """
    out = {"from": point3(start), "to": point3(end)}
    if direction is not None:
        out["direction"] = point3(direction)
    if normal is not None:
        out["normal"] = point3(normal)
    _put(
        out,
        (
            ("text", text),
            ("decimals", decimals),
            ("unit", unit),
            ("showUnit", show_unit),
            ("terminator", _terminator(terminator)),
            ("occlusion", _occlusion(occlusion)),
            ("font", font),
        ),
    )
    _put(out, (("color", color), ("textColor", text_color), ("halo", halo)), colour)
    return _put(
        out,
        (
            ("offsetMetres", offset),
            ("widthPixels", width),
            ("textSizePixels", text_size),
            ("haloPixels", halo_size),
            ("terminatorSizePixels", terminator_size),
        ),
        float,
    )


def text(
    text,
    at=None,
    screen=None,
    plane=None,
    size=None,
    color=None,
    halo=None,
    halo_size=None,
    halo_scale=None,
    background=None,
    align=None,
    baseline=None,
    offset=None,
    rotation=None,
    occlusion=None,
    font=None,
):
    """A label: at a model point (`at`), fixed to the view (`screen`, fractions from its
    top left), or lying on a plane in the model (`at` and `plane`).

    `plane` is (direction, normal, height_metres) or a dict with those keys: the baseline
    runs along `direction`, the text rises towards normal x direction.

    The halo grows with the text as drawn -- 0.5 px round a 24 px em, 2 px round a 72 px
    one, none under 8 px -- times `halo_scale`; `halo_size` fixes it in pixels instead.
    """
    if (at is None) == (screen is None):
        raise ValueError("a text is anchored at a model point or on the screen, one of the two")
    out = {"text": str(text)}
    if at is not None:
        out["at"] = point3(at)
    if screen is not None:
        out["screen"] = point2(screen)
    if plane is not None:
        if isinstance(plane, dict):
            direction, normal, height = plane["direction"], plane["normal"], plane["sizeMetres"]
        else:
            direction, normal, height = plane
        out["plane"] = {"direction": point3(direction), "normal": point3(normal), "sizeMetres": float(height)}
    for key, value in (
        ("sizePixels", size),
        ("haloPixels", halo_size),
        ("haloScale", halo_scale),
        ("rotationDegrees", rotation),
    ):
        if value is not None:
            out[key] = float(value)
    for key, value in (("color", color), ("halo", halo), ("background", background)):
        if value is not None:
            out[key] = colour(value)
    for key, value in (("align", align), ("baseline", baseline), ("occlusion", _occlusion(occlusion)), ("font", font)):
        if value is not None:
            out[key] = value
    if offset is not None:
        out["offsetPixels"] = point2(offset)
    return out


def legend(
    colormap,
    minimum=None,
    maximum=None,
    title=None,
    unit=None,
    mesh=None,
    corner=None,
    screen=None,
    horizontal=None,
    length=None,
    width=None,
    ticks=None,
    decimals=None,
    tick_values=None,
    tick_labels=None,
    size=None,
    title_size=None,
    color=None,
    halo=None,
    background=None,
    padding=None,
    bar_border=None,
    offset=None,
    halo_size=None,
    font=None,
):
    """A colour bar fixed to the view. `mesh` (an index into the same call's meshes)
    takes that heatmap's ramp and range instead of `colormap`, `minimum`, `maximum`.

    `corner` names the legend's own corner; it sits in that corner of the view, or at
    `screen` (fractions of the view) when given. `background=None` keeps the default
    panel; pass "00000000" for none.
    """
    out = {}
    if mesh is not None:
        out["mesh"] = int(mesh)
    else:
        out["colormap"] = _colormap(colormap, minimum, maximum)
    for key, value in (
        ("title", title),
        ("unit", unit),
        ("corner", corner),
        ("horizontal", horizontal),
        ("ticks", ticks),
        ("decimals", decimals),
        ("font", font),
    ):
        if value is not None:
            out[key] = value
    for key, value in (
        ("lengthPixels", length),
        ("widthPixels", width),
        ("sizePixels", size),
        ("titleSizePixels", title_size),
        ("paddingPixels", padding),
        ("haloPixels", halo_size),
    ):
        if value is not None:
            out[key] = float(value)
    for key, value in (("color", color), ("halo", halo), ("background", background), ("barBorder", bar_border)):
        if value is not None:
            out[key] = colour(value)
    if screen is not None:
        out["screen"] = point2(screen)
    if offset is not None:
        out["offsetPixels"] = point2(offset)
    if tick_values is not None:
        out["tickValues"] = [float(v) for v in tick_values]
    if tick_labels is not None:
        out["tickLabels"] = [str(v) for v in tick_labels]
    return out


def heatmap(
    points,
    indices,
    values,
    colormap="viridis",
    minimum=None,
    maximum=None,
    bands=None,
    isolines=None,
    opacity=None,
    occlusion=None,
):
    """A mesh coloured by a value per vertex: flat x, y, z points, three indices per
    triangle. Smooth by default; `bands` steps it, `isolines` ({"step", "color"}) draws
    contours."""
    style = {}
    if opacity is not None:
        style["opacity"] = float(opacity)
    if occlusion is not None:
        style["occlusion"] = _occlusion(occlusion)
    out = {
        "points": [float(v) for v in points],
        "indices": [int(i) for i in indices],
        "values": [float(v) for v in values],
        "colormap": _colormap(colormap, minimum, maximum, bands, isolines),
    }
    if style:
        out["style"] = style
    return out


def set_layer(
    name,
    views="both",
    occlusion=None,
    polylines=None,
    points=None,
    meshes=None,
    texts=None,
    dimensions=None,
    legends=None,
    panels=None,
    font=None,
):
    """Add or replace the layer `name` on the overlays. Returns what was taken.

    `font` is every text's, dimension's, legend's and panel's that names none: an
    installed family as Windows lists it ("Arial", "Segoe UI Semibold") or a font file.
    """
    params = {
        "layer": str(name),
        "views": views,
        "occlusion": _occlusion(occlusion),
        "polylines": polylines,
        "points": points,
        "meshes": meshes,
        "texts": texts,
        "dimensions": dimensions,
        "legends": legends,
        "font": font,
    }
    if panels is not None:
        params["panels"] = [p.to_dict() if isinstance(p, Panel) else p for p in panels]
    return call("Tapioca.SetOverlayLayer", _reals(params)).data or {}


def hud(name, *panels, views="both"):
    """A layer of HUD panels only."""
    return set_layer(name, views=views, panels=list(panels))


def story_slices(
    action="on",
    source=None,
    elements=None,
    cut=None,
    step=None,
    levels=None,
    offset=None,
    min_top=None,
    storeys=None,
    views=None,
    outline=None,
    fill=None,
    label=None,
    lift=None,
):
    """The add-on's slices on the overlays: the selected massing slabs' floors, or the
    model per storey (Tapioca.OverlayStorySlices).

    `outline`, `fill` and `label` are style dicts in the wire's names -- outline
    {"color", "widthPixels", "dashMetres", "occlusion", "hidden": {"color", "widthPixels",
    "dashMetres"}}, fill {"color", "occlusion"},
    label {"show", "sizePixels", "color", "halo", "haloPixels", "decimals", "name",
    "onSlice", "sizeMetres"} -- with colours in any form `colour` reads.
    """
    params = {
        "action": action,
        "source": source,
        "cut": cut,
        "stepMetres": step,
        "levels": levels,
        "offsetMetres": offset,
        "minTopMetres": min_top,
        "storeys": storeys,
        "views": views,
        "liftMetres": lift,
    }
    if elements is not None:
        params["elements"] = [{"elementId": {"guid": str(guid)}} for guid in elements]
    for key, value in (("outline", outline), ("fill", fill), ("label", label)):
        if value is not None:
            params[key] = _coloured(value)
    return call("Tapioca.OverlayStorySlices", _reals(params)).data or {}


def annotations(action="on", style=None):
    """The Watch trace's annotations on the overlays (Tapioca.OverlayAnnotations).

    `style` replaces the previous one: {"lineWidthPixels", "contextWidthPixels",
    "pointSizePixels", "textSizePixels", "halo", "haloPixels", "occlusion", "colors":
    {"add", "remove", "modify", "context", "guide", "none"}}.
    """
    params = {"action": action}
    if style is not None:
        styled = _coloured(style, ("halo",))
        if "colors" in styled:
            styled["colors"] = {role: colour(c) for role, c in styled["colors"].items()}
        params["style"] = styled
    return call("Tapioca.OverlayAnnotations", _reals(params)).data or {}


def clear(name):
    """Remove one layer. Returns how many went (0 or 1)."""
    return (call("Tapioca.ClearOverlayLayer", {"layer": str(name)}).data or {}).get("cleared", 0)


def clear_all():
    """Remove every layer a caller set; the add-on's own (slices, annotations) stay."""
    return (call("Tapioca.ClearOverlayLayer", {"all": True}).data or {}).get("cleared", 0)


def layers():
    """What is set, in draw order, and what the overlays' renderers have drawn of it."""
    return call("Tapioca.OverlayLayers", {}).data or {}
