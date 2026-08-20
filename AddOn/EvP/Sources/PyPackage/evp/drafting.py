"""Layer 2 — 2D drafting writes: text and raster figures.

    evp.drafting.create_text([{"text": "A-01", "x": 0, "y": 0}], layer="Notes", tx=tx)
    evp.drafting.place_picture(png_path, x=0, y=0, width=10, height=7.5, tx=tx)

Both are native (`EvP.CreateText` / `EvP.PlacePicture`), and both are writes — pass an
open `evp.transaction` as `tx` to fuse a whole run into ONE undo step.

WHY NOT TAPIR, for text: `Tapir.CreateTexts` sets coordinate / text / height / pen /
angle / justification and nothing else. It cannot set the layer, the anchor (which
corner of the box the coordinate means), bold/italic/underline, the font, the
fixed-width box, or the fixed angle/size flags. An annotation run needs those.

There is NO alternative at all for a picture: the image bytes cross to Archicad as a
memo handle, which no JSON schema can carry, so neither the `archicad` package nor
Tapir can place one.
"""

from .api import call

#: Every anchor the two commands accept — which point of the box (x, y) names.
#: Getting this wrong is the usual reason placed text or images look offset.
ANCHORS = (
    "topLeft", "topCenter", "topRight",
    "middleLeft", "middleCenter", "middleRight",
    "bottomLeft", "bottomCenter", "bottomRight",
)

#: Horizontal justification within the text box (distinct from `anchor`, which
#: positions the box itself).
JUSTIFICATIONS = ("left", "center", "right", "full")

_TEXT_KEYS = {
    "text", "x", "y", "floorInd", "layer", "size", "angle", "pen", "font",
    "just", "anchor", "bold", "italic", "underline", "strikeOut", "width",
    "fixedAngle", "fixedSize", "spacing", "widthFactor", "charSpaceFactor",
    "inheritDefaults",
}

# snake_case in, camelCase on the wire — the same split every other evp module
# keeps, so a command author never types a camelCase key.
_TEXT_ALIASES = {
    "floor_ind": "floorInd",
    "strike_out": "strikeOut",
    "fixed_angle": "fixedAngle",
    "fixed_size": "fixedSize",
    "width_factor": "widthFactor",
    "char_space_factor": "charSpaceFactor",
    "justification": "just",
    "inherit_defaults": "inheritDefaults",
}


def _text_item(item, defaults):
    """One {text, x, y, ...} dict -> the wire form, with `defaults` filled in under it."""
    merged = dict(defaults)
    merged.update(item)

    out = {}
    for key, value in merged.items():
        if value is None:
            continue
        key = _TEXT_ALIASES.get(key, key)
        if key not in _TEXT_KEYS:
            raise ValueError(
                "unknown text field %r (want one of: %s)"
                % (key, ", ".join(sorted(_TEXT_KEYS)))
            )
        out[key] = value

    for required in ("text", "x", "y"):
        if required not in out:
            raise ValueError("each text needs 'text', 'x' and 'y' (missing %r)" % required)
    if "just" in out and out["just"] not in JUSTIFICATIONS:
        raise ValueError("just must be one of %s" % (JUSTIFICATIONS,))
    if "anchor" in out and out["anchor"] not in ANCHORS:
        raise ValueError("anchor must be one of %s" % (ANCHORS,))
    return out


def create_text(texts, tx=None, **defaults):
    """Place one or more text elements. `texts` is a list of dicts (or a single dict).

    Per-text fields — only `text`, `x` and `y` are required; everything omitted keeps
    the Text tool's current default, so a bare {text, x, y} places what the user would
    get by typing it:

      text          the content; "\\n" makes extra lines
      x, y          model coordinates of the current database
      size          character height in MILLIMETRES (paper), not metres
      angle         rotation in RADIANS, counter-clockwise
      layer         layer NAME (what an evp.Layer picker hands you)
      just          "left" | "center" | "right" | "full" — within the box
      anchor        which point of the box (x, y) is; see ANCHORS
      bold, italic, underline, strike_out    style flags; each one you omit leaves
                    the tool default's flag alone rather than clearing it
      pen           pen index;  font  font attribute index (there is no font-NAME
                    lookup in the AC29 API, so this stays an index)
      width         > 0 makes it a fixed-width box that wraps. MILLIMETRES, like
                    `size` — the header says "horizontal size of text box in mm".
                    A metre-scale value here does not make a small box, it makes
                    a sub-millimetre one and every character wraps to its own line
      fixed_angle, fixed_size    keep angle/size when the drawing is rotated/rescaled
      floor_ind, spacing, width_factor, char_space_factor
      inherit_defaults   see the reset note below

    ⚠️ FORMATTING IS RESET, NOT INHERITED. ACAPI hands back the Text tool's CURRENT
    state, so a bare {text, x, y} would otherwise pick up whatever rotation, bold and
    justification the user (or the previous run) left set — and that compounds. This
    call therefore starts every text from a known baseline (angle 0, plain face, no
    effects, left justified, bottom-left anchor, no fixed-width box) and raises only
    what you ask for. Size, pen and font are NOT reset: a script that does not mention
    them wants the user's current text style. Pass `inherit_defaults=True` to take the
    tool state wholesale instead.

    Anything passed as a keyword becomes a DEFAULT under every item, so the common
    "same layer and size, many positions" case stays flat:

        evp.drafting.create_text(
            [{"text": t, "x": x, "y": y} for t, x, y in rows],
            layer="Notes", size=2.5, anchor="middleCenter", tx=tx)

    Returns a list of {ok, guid, error} aligned to `texts`. Returns [] for empty
    input. Raises ValueError on an unknown field or a bad just/anchor, before
    anything is written.

    ⚠️ Inside a transaction this returns a **Handle**, not data: nothing has run
    yet. After the `with` block commits, read it with `handle.result()` -- and note
    that `result()` gives the RAW wire response (camelCase keys), not the dict this
    function returns on the direct path. `handle.<anything>` is NOT an accessor: it
    silently yields a Ref for use as a later step's input.
    """
    if isinstance(texts, dict):
        texts = [texts]
    items = [_text_item(item, defaults) for item in texts]
    if not items:
        return []

    params = {"texts": items}
    if tx is not None:
        return tx.call("EvP.CreateText", params)

    data = call("EvP.CreateText", params).data or {}
    return [{"ok": bool(r.get("ok", False)), "guid": r.get("guid", ""), "error": r.get("error", "")}
            for r in (data.get("results") or [])]


def place_picture(path, x, y, width=None, height=None, dpi=None, layer=None, anchor=None,
                  rot_angle=None, mirrored=None, transparent=None, name=None,
                  floor_ind=None, use_pixel_size=None, tx=None):
    """Place a raster image as a Figure element. `path` is a file on disk.

    The image is read from disk BY ARCHICAD, not sent over the bus — so write it
    somewhere first (`evp.paths.output_path("plan.png")` is the usual spot) and pass
    that path. That also keeps a multi-megabyte PNG out of the envelope.
    Accepted types: .png .jpg/.jpeg .tif/.tiff .gif .bmp — an unknown extension is a
    refusal, not a guess, because Archicad stores the bytes verbatim and a wrong
    format tag yields a figure that will not display.

    SIZING — always an explicit box, in METRES, growing right/up from (x, y):
      width and height given  -> exactly that rectangle
      either omitted          -> derived from the image's pixels at `dpi`
                                 (default 96): metres = pixels / dpi * 0.0254

    ⚠️ The API's own "natural size" mode is deliberately NOT the default: it is
    not reproducible. Measured live, one 64x48 PNG placed that way came out
    0.355 x 0.266 m on the floor plan and 0.335 x 0.252 m on a layout — the size
    depends on the view it lands in, so a script cannot lay anything out around it.
    `use_pixel_size=True` opts back into it.

    Returns {ok, guid, pixel_width, pixel_height, placed_width, placed_height, error}
    — `placed_*` are the metres actually used, so a caller can position the next
    thing without guessing.

    ⚠️ Inside a transaction this returns a **Handle**, not data: nothing has run
    yet. After the `with` block commits, read it with `handle.result()` -- and note
    that `result()` gives the RAW wire response (camelCase keys), not the dict this
    function returns on the direct path. `handle.<anything>` is NOT an accessor: it
    silently yields a Ref for use as a later step's input.
    """
    params = {"path": str(path), "x": x, "y": y}
    if width is not None and height is not None:
        params["width"] = width
        params["height"] = height
    for key, value in (("layer", layer), ("anchor", anchor), ("rotAngle", rot_angle),
                       ("mirrored", mirrored), ("transparent", transparent),
                       ("name", name), ("floorInd", floor_ind), ("dpi", dpi),
                       ("usePixelSize", use_pixel_size)):
        if value is not None:
            params[key] = value
    if anchor is not None and anchor not in ANCHORS:
        raise ValueError("anchor must be one of %s" % (ANCHORS,))

    if tx is not None:
        return tx.call("EvP.PlacePicture", params)

    data = call("EvP.PlacePicture", params).data or {}
    return {
        "ok": bool(data.get("ok", False)),
        "guid": data.get("guid", ""),
        "pixel_width": data.get("pixelWidth", 0),
        "pixel_height": data.get("pixelHeight", 0),
        "placed_width": data.get("placedWidth", 0.0),
        "placed_height": data.get("placedHeight", 0.0),
        "error": data.get("error", ""),
    }
