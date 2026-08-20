"""Layer 2 — element details read natively (no Tapir).

    elems = evp.elements.details(guids)        # slab/roof/mesh/wall/beam/column/
                                               # polyline/object/lamp/fill, per kind
    plan  = evp.elements.wall_plan_outlines(g) # a wall as the FLOOR PLAN draws it

`details` absorbs what MassingFeasibility used to get from Tapir's
`GetDetailsOfElements` (the same absorption E6 did for zone/room topology) by reading
parametric geometry DIRECTLY off the element instead of the 3D mesh.

⚠️ ONE READ, NOT ONE PER TYPE. There was a second function here, `slab_details`, from
before `details` covered every kind; it survived as a slab-only spelling of the same
call. That is the drift this API does not have room for: it kept its own copy of the
unflattening, it went stale when v2 retired the command underneath it, and the failure
read as a missing add-on rather than a dead wrapper. `Tapioca.GetElementDetails` reports
slabs natively (ElementReadCommands.cpp -> `case API_SlabID`), so a slab-only wrapper
could only ever narrow what the one read already returns. Filter on `kind == "slab"`.

`details` reports a `kind` per element. Since E16.0 the wire carries one NESTED RECORD
per element ({guid, found, kind, floorInd, details{...}}) rather than flat parallel
arrays, so this function reads rather than unflattens. By kind:
  - POLYGON kinds (slab, roof, mesh): `footprint` as a list of (x, y) tuples (outer
    contour, distinct points, no closing repeat), `holes` as a list of such rings, and
    `has_holes`. Use `net_footprint_area(rec)` for the outer area minus the holes.
  - TERRAIN kind (mesh): the polygon keys, plus `footprint_z` (one elevation per contour
    vertex, parallel to `footprint`) and `sublines` (the interior level lines as
    [(x, y, z), ...]) — see `details` for the rest.
  - AXIS/POINT kinds (wall, beam, column, object, lamp): `axis` as ((bx,by,bz), (ex,ey,ez))
    — native primitives, not a synthesized footprint (a column and an object are points, so
    begin == end). Beam and column also carry `section_width` / `section_height` (segment-0
    cross-section) and `n_segments`; object and lamp carry `library_part_name`, `x_ratio`,
    `y_ratio` and `reflected`.
  - CHAIN kind (polyline): the footprint keys again (`footprint`, `footprint_arcs`) plus
    `closed` — the chain is not a footprint, and only a closed one has an area.
A non-supported or missing guid comes back with `found=False`, `kind=""`, empty geometry
and zeroed scalars, positionally aligned to the input.
"""

import math

from .api import call


def _ring(points):
    """[{x, y}, ...] -> [(x, y), ...]. The one place the wire's coordinate objects
    become the tuples every geometry helper in this module already speaks."""
    return [(p.get("x", 0.0), p.get("y", 0.0)) for p in (points or [])]


def _arc_radius(chord, angle):
    """Radius of a circular arc spanning `chord` with central angle `angle` (radians)."""
    half = math.sin(abs(angle) / 2.0)
    if half < 1e-12:
        return 0.0
    return chord / (2.0 * half)


def polygon_area(ring, arcs=None):
    """Absolute area (m^2) of a ring [(x, y), ...]. 0 for < 2 points.

    With `arcs` (one signed arc angle per vertex, for the edge leaving it), each curved
    edge adds its circular-segment area to the straight-edge shoelace — so a slab with a
    bulged or scalloped edge measures its true area, not the chord polygon's. A circle is
    stored as 2 nodes + two 180-degree arcs, so 2-point rings are valid when arced.
    """
    n = len(ring)
    if n < 2:
        return 0.0
    acc = 0.0
    for i in range(n):
        x0, y0 = ring[i]
        x1, y1 = ring[(i + 1) % n]
        acc += x0 * y1 - x1 * y0
    signed = acc / 2.0
    if arcs:
        for i in range(n):
            a = arcs[i] if i < len(arcs) else 0.0
            if a:
                x0, y0 = ring[i]
                x1, y1 = ring[(i + 1) % n]
                chord = math.hypot(x1 - x0, y1 - y0)
                r = _arc_radius(chord, a)
                # Signed circular-segment area; sign follows the arcAngle's right-hand
                # convention, so it matches the shoelace orientation on either winding.
                signed += 0.5 * r * r * (a - math.sin(a))
    return abs(signed)


def polygon_perimeter(ring, arcs=None):
    """Perimeter (m) of a ring [(x, y), ...]. With `arcs`, curved edges use arc length."""
    n = len(ring)
    if n < 2:
        return 0.0
    total = 0.0
    for i in range(n):
        x0, y0 = ring[i]
        x1, y1 = ring[(i + 1) % n]
        chord = math.hypot(x1 - x0, y1 - y0)
        a = arcs[i] if (arcs and i < len(arcs)) else 0.0
        total += (abs(a) * _arc_radius(chord, a)) if a else chord
    return total


def net_footprint_area(rec):
    """Net footprint area (m^2) of a slab/roof record: outer ring minus its holes.

    Takes a dict from `details`; honors arc edges on both the outer
    contour and the holes. Equals the gross outer area when the element has no holes.
    Returns 0.0 for records without a `footprint`.
    """
    footprint = rec.get("footprint")
    if not footprint:
        return 0.0
    outer = polygon_area(footprint, rec.get("footprint_arcs"))
    hole_arcs = rec.get("hole_arcs", [])
    holes = rec.get("holes", [])
    inner = sum(polygon_area(h, hole_arcs[j] if j < len(hole_arcs) else None)
                for j, h in enumerate(holes))
    return outer - inner


def set_surface(guids, surface=None, clear=False, restore=None):
    """Paint each element a named surface override, clear it, or restore a prior state.

    A destructive element-level override (the surface sticks until reverted or the
    element is edited), used to mark elements in 3D — e.g. MassingFeasibility
    reddening over-size slabs. Supported types: slab and wall. Roof/beam/column
    come back kind="unsupported" (surfaces live in nested/segment structs there).

    Three ops (priority: restore > clear > paint):
      - Paint:   set_surface(guids, surface="<exact surface name>")
      - Clear:   set_surface(guids, clear=True)          # -> element's default surface
      - Restore: set_surface(guids, restore=<prior result>)  # exact original, per element

    EVERY call also CAPTURES each element's state before it writes, so a round-trip is
    non-destructive:

        before = set_surface(guids, surface="red")   # go red, capture the original
        ...                                           # (look, or whatever)
        set_surface(guids, restore=before)           # back to EXACTLY what it was

    `restore` takes the list this function returned earlier (it reads each dict's
    "prev_mats"/"prev_chained"); pass the SAME guids in the SAME order.

    Returns a list of dicts aligned to `guids`:
      {guid, found, changed, kind, prev_mats, prev_chained}
    where `found` is "was a supported element that exists", `changed` is "the write
    succeeded", `kind` is "slab"/"wall"/"unsupported"/"missing", `prev_mats` is the
    (top, side, bot) surface indices BEFORE this call (None = that face had no
    override), and `prev_chained` the element's prior materials-chained flag. Returns
    [] for empty input. Raises ValueError if painting without a surface name.
    """
    guids = list(guids)
    if not guids:
        return []
    if restore is None and not clear and not surface:
        raise ValueError("set_surface needs surface=<name>, clear=True, or restore=...")

    payload = {"guids": guids, "clear": bool(clear)}
    if restore is not None:
        # Flatten the prior result back into the wire arrays the native command wants.
        # None -> -1 (no override), matching the command's NoOverride sentinel.
        flat_mats, chained = [], []
        for rec in restore:
            t, s, b = rec.get("prev_mats", (None, None, None))
            flat_mats += [-1 if v is None else int(v) for v in (t, s, b)]
            chained.append(bool(rec.get("prev_chained", False)))
        payload["restoreMats"] = flat_mats
        payload["restoreChained"] = chained
    elif not clear:
        payload["surface"] = surface

    data = call("Tapioca.SetElementSurface", payload).data or {}

    out_guids  = data.get("guids", [])
    found      = data.get("found", [])
    changed    = data.get("changed", [])
    kinds      = data.get("kind", [])
    prev_mats  = data.get("prevMats", [])       # flat 3 per element; -1 = no override
    prev_chain = data.get("prevChained", [])

    def at(seq, i, default):
        return seq[i] if i < len(seq) else default

    result = []
    for i, g in enumerate(out_guids):
        b = i * 3
        mats = tuple(
            None if at(prev_mats, b + m, -1) < 0 else at(prev_mats, b + m, -1)
            for m in range(3)
        )
        result.append({
            "guid": g,
            "found": bool(at(found, i, False)),
            "changed": bool(at(changed, i, False)),
            "kind": at(kinds, i, ""),
            "prev_mats": mats,
            "prev_chained": bool(at(prev_chain, i, False)),
        })
    return result


# Kinds whose geometry is a horizontal footprint polygon vs an axis/point member. A mesh
# is one of them — its contour reads exactly like a slab's, and the elevations ride
# alongside as a parallel array, so every polygon helper here applies unchanged.
_POLYGON_KINDS = ("slab", "roof", "mesh", "fill")
# ... of which only the mesh carries a Z per contour vertex and interior level lines.
_TERRAIN_KINDS = ("mesh",)
_SECTION_KINDS = ("beam", "column")
# A polyline is a vertex CHAIN, read with the same keys as a footprint (see `details`).
_CHAIN_KINDS = ("polyline",)
# Library-part placements. API_LampType is API_ObjectType natively, so they read alike.
_OBJECT_KINDS = ("object", "lamp")


def details(guids):
    """Parametric geometry for slab / roof / mesh / fill / wall / beam / column /
    polyline / object / lamp, aligned to `guids`.

    Each dict always carries: {guid, found, kind, floor_ind, thickness, height, level,
    plan_angle, slant_angle, is_slanted}. `slant_angle` (radians) is each type's tilt
    option: roof plane pitch, wall slantAlpha (pi/2 = plumb), beam slant (0 = level),
    column slant (pi/2 = plumb); multi-plane roofs report 0 (per-plane there).
    `is_slanted` is the real beam/column flag; it is False for wall/roof (no such flag —
    read the angle). Then, by kind:
      - slab / roof / mesh / fill: `footprint` = [(x, y), ...] (outer contour, meters),
        `footprint_arcs` (one arc angle per vertex, 0 = straight), `holes` = list of rings,
        `hole_arcs` (per-hole arc lists), `has_holes`. `net_footprint_area(rec)` gives outer
        minus holes, honoring curved edges.
      - roof, additionally: `roof_class` ("plane" | "poly") and the PIVOT its pitch is
        measured from. A plane roof hinges on a line: `base_line` = ((bx,by),(ex,ey))
        and `pos_sign` (which side slopes up), with the pitch in `slant_angle`. A poly
        roof hinges on a polygon: `pivot_outline` / `pivot_arcs`, and its pitch is PER
        LEVEL in `levels` = [(angle_rad, height_m), ...] with `level_num` — which is why
        `slant_angle` is 0 for one. Plus `eaves_overhang` and `overhang_type`. Both
        halves are always present (zeros/empties on the inapplicable side, so the record
        keeps one shape), so branch on `roof_class` rather than testing for zero.
      - mesh, additionally: `footprint_z` — one elevation per `footprint` vertex, a
        PARALLEL array so the 2D helpers above still apply (compare its length against
        `footprint`; a short one means the elevation memo read failed) — plus `hole_z`,
        `sublines` (interior level lines, each [(x, y, z), ...] — a surveyed terrain's are
        typically ONE POINT each, i.e. level *points*, so do not assume two ends),
        `subline_vertex_ids`,
        `skirt_level`, `skirt_type`, `ridge_mode`, `n_sublines`, `n_level_coords` and the
        raw `level_ends`. `level` is the base plane, as for a slab; a mesh has no
        `thickness` (`skirt_level` is how far its solid drops below the base plane).
      - wall / beam / column / object / lamp: `axis` = ((bx,by,bz), (ex,ey,ez)); a column
        and an object are points, so begin == end. beam / column also carry
        `section_width`, `section_height`, `n_segments`; object / lamp carry `x_ratio`,
        `y_ratio`, `reflected` and `library_part_name` (`plan_angle` is the rotation).
      - fill, additionally: `pen` (the contour pen), `fill_pen` and `fill_bg_pen`.
        A fill is a 2D polygon with NO thickness, level or elevation — those keys
        are present but zero. It has no `closed` flag either: a fill is closed by
        definition, which is what makes it read like a slab rather than a polyline.
      - polyline: `footprint` = [(x, y), ...] and `footprint_arcs`, the SAME keys as a
        slab contour so one piece of caller code reads both — but it is a vertex CHAIN,
        and `closed` says whether it wraps back to its first vertex. Area is only
        meaningful when `closed` is True; length is `polygon_perimeter(fp, arcs)` for a
        closed one, and the open chain's own walk otherwise. `holes` is never present.
        `pen` is the contour pen — the join key a survey drawing relies on.
    Unsupported / missing guids come back found=False, kind="", empty geometry — plus
    `reason` ("notFound" vs "unsupportedType"), `type_name` (localized) and `type_id`, so a
    bulk read can report WHICH types are still unspoken rather than a list of guids.
    Returns [] for empty input.
    """
    guids = list(guids)
    if not guids:
        return []

    # ⚠️ THE WIRE IS {elements:[{elementId:{guid}}]}, NOT {guids:[…]}. This wrapper
    # was left on the pre-E16.0 flat spelling when the native command moved to the
    # typed identity, and the native input schema is closed
    # (additionalProperties:false, elements required) — so every call failed
    # SchemaValidationFailed before the handler ever ran. Nothing in the response
    # mapping below can catch that, which is why it survived.
    elements = [{"elementId": {"guid": str(guid)}} for guid in guids]
    data = call("Tapioca.GetElementDetails", {"elements": elements}).data or {}
    records = data.get("detailsOfElements", [])

    result = []
    for entry in records:
        entry = entry or {}
        kind = entry.get("kind", "")
        # Only the fields meaningful for a kind are on the wire, so every read here
        # defaults. That is what lets a new kind land natively without touching this.
        d = entry.get("details") or {}

        rec = {
            "guid": (entry.get("elementId") or {}).get("guid", ""),
            "found": bool(entry.get("found", False)),
            "kind": kind,
            "floor_ind": entry.get("floorInd", 0),
            "thickness": d.get("thickness", 0.0),
            "height": d.get("height", 0.0),
            "level": d.get("level", 0.0),
            "plan_angle": d.get("planAngle", 0.0),
            "slant_angle": d.get("slantAngle", 0.0),
            "is_slanted": bool(d.get("isSlanted", False)),
            # Only meaningful when found is False: "notFound" (no such guid) vs
            # "unsupportedType" (a real element of a kind this read does not speak),
            # with the element's localized type name. On a bulk read the misses are
            # the interesting part — this is what names the still-unspoken types.
            "reason": entry.get("reason", ""),
            "type_name": entry.get("typeName", ""),
            "type_id": entry.get("typeId", 0),
            # The Element Settings "ID" box. Present on EVERY record, including the
            # misses — it is read type-agnostically (see ids() below), so it is the
            # one detail this call can give for a kind it cannot otherwise speak.
            # ⚠️ It rides in `value`; `elementId` is the typed IDENTITY record, and
            # reading the ID box out of it silently yielded a dict, then "".
            "element_id": entry.get("value", ""),
        }
        if kind in _POLYGON_KINDS:
            rec["footprint"] = _ring(d.get("polygonOutline"))
            rec["footprint_arcs"] = list(d.get("polygonArcs") or [])
            # Holes are self-similar to the contour on the wire, so they read the same way.
            rec["holes"] = [_ring(h.get("polygonOutline")) for h in (d.get("holes") or [])]
            rec["hole_arcs"] = [list(h.get("polygonArcs") or [])
                                for h in (d.get("holes") or [])]
            rec["has_holes"] = bool(d.get("hasHoles", False))
            if kind == "roof":
                # The pivot — what the pitch is measured FROM (E15). `roof_class` says
                # which half is real: a "plane" roof hinges on `base_line` with
                # `pos_sign` for the up-slope side, a "poly" roof on `pivot_outline`
                # with a pitch PER LEVEL in `levels`. Both halves are always present
                # (zeros/empties on the side that does not apply), so a zero here is
                # not evidence of anything — read `roof_class` first.
                rec["roof_class"] = d.get("roofClass", "")
                base = d.get("baseLine") or {}
                beg = base.get("begCoordinate") or {}
                end = base.get("endCoordinate") or {}
                rec["base_line"] = ((beg.get("x", 0.0), beg.get("y", 0.0)),
                                    (end.get("x", 0.0), end.get("y", 0.0)))
                rec["pos_sign"] = bool(d.get("posSign", False))
                rec["pivot_outline"] = _ring(d.get("pivotOutline"))
                rec["pivot_arcs"] = list(d.get("pivotArcs") or [])
                rec["levels"] = [(lv.get("levelAngle", 0.0), lv.get("levelHeight", 0.0))
                                 for lv in (d.get("levels") or [])]
                rec["level_num"] = d.get("levelNum", 0)
                rec["eaves_overhang"] = d.get("eavesOverHang", 0.0)
                rec["overhang_type"] = d.get("overHangType", 0)

            if kind == "fill":
                # A drawing uses pens to say what a fill MEANS, so they are the
                # fill's equivalent of the polyline's join key.
                rec["pen"] = d.get("pen", 0)
                rec["fill_pen"] = d.get("fillPen", 0)
                rec["fill_bg_pen"] = d.get("fillBGPen", 0)

            if kind in _TERRAIN_KINDS:
                # `footprint_z[i]` is the elevation of `footprint[i]` — a parallel array,
                # exactly like `footprint_arcs`, so the 2D contour keeps working with every
                # polygon helper and the third dimension is opt-in. A length mismatch means
                # the meshPolyZ memo read failed, so callers should compare, not assume.
                rec["footprint_z"] = list(d.get("polygonZ") or [])
                rec["hole_z"] = [list(h.get("polygonZ") or [])
                                 for h in (d.get("holes") or [])]
                # Interior level lines (the UI's "ridges"), each a list of (x, y, z).
                rec["sublines"] = [
                    [(c.get("x", 0.0), c.get("y", 0.0), c.get("z", 0.0))
                     for c in (s.get("coordinates") or [])]
                    for s in (d.get("sublines") or [])
                ]
                rec["subline_vertex_ids"] = [list(s.get("vertexIds") or [])
                                             for s in (d.get("sublines") or [])]
                rec["skirt_level"] = d.get("skirtLevel", 0.0)
                rec["skirt_type"] = d.get("skirtType", "")
                rec["ridge_mode"] = d.get("ridges", "")
                rec["n_sublines"] = d.get("nSubLines", 0)
                rec["n_level_coords"] = d.get("nLevelCoords", 0)
                # RAW meshLevelEnds. Kept because the level-line memo convention is the
                # open question behind EvP.CreateMesh's ridge indexing: this is the
                # evidence, and `sublines` above is one interpretation of it.
                rec["level_ends"] = list(d.get("levelEnds") or [])
        elif kind in _CHAIN_KINDS:
            # Same keys as a footprint on purpose (native spells them the same way), so
            # `_ring` and the polygon helpers apply — `closed` is what makes them mean
            # something. An OPEN polyline keeps its last vertex; the native side detects
            # the closing repeat rather than assuming it.
            rec["footprint"] = _ring(d.get("polygonOutline"))
            rec["footprint_arcs"] = list(d.get("polygonArcs") or [])
            rec["closed"] = bool(d.get("closed", False))
            # Native has always emitted this; it was simply not mapped. It is the
            # join key a survey drawing uses — a breakline and its spot-height
            # label share a pen and nothing else.
            rec["pen"] = d.get("pen", 0)
        elif kind in ("wall",) + _SECTION_KINDS + _OBJECT_KINDS:
            beg = d.get("begCoordinate") or {}
            end = d.get("endCoordinate") or {}
            rec["axis"] = (
                (beg.get("x", 0.0), beg.get("y", 0.0), beg.get("z", 0.0)),
                (end.get("x", 0.0), end.get("y", 0.0), end.get("z", 0.0)),
            )
            if kind in _SECTION_KINDS:
                rec["section_width"] = d.get("sectionWidth", 0.0)
                rec["section_height"] = d.get("sectionHeight", 0.0)
                rec["n_segments"] = d.get("nSegments", 0)
            if kind in _OBJECT_KINDS:
                rec["x_ratio"] = d.get("xRatio", 0.0)
                rec["y_ratio"] = d.get("yRatio", 0.0)
                rec["reflected"] = bool(d.get("reflected", False))
                rec["library_part_name"] = d.get("libraryPartName", "")
        result.append(rec)
    return result


def wall_plan_outlines(guids):
    """A wall's outline AS THE FLOOR PLAN DRAWS IT, aligned to `guids`.

    This is NOT `details()['axis']` and it is not a top view of the model. Archicad
    trims a wall where it meets other walls, and that trimmed shape — the CONNECTION
    polygon — is what the plan shows. It is the anchor geometry for the plan overlay:
    the thing our drawing can be checked against, because Archicad already drew it.

    Each dict carries {guid, succeeded, error, wall_shape, outline, outline_arcs,
    holes, hole_arcs, outline_source, memo_present, memo_outline, memo_outline_arcs,
    connected_walls}:
      - `outline` = [(x, y), ...] in meters, distinct vertices, no closing repeat;
        `outline_arcs` is one signed angle (radians) per vertex for the edge LEAVING
        it, 0 for straight — the same spelling `details()` uses for a footprint, so
        the polygon helpers in this module apply unchanged.
      - `holes` / `hole_arcs` follow the same shape, per ring.
      - `outline_source` is "connectionPolygon" or "none" (nothing readable).
      - `memo_outline` is the wall's OWN untrimmed polygon, the before-junctions
        shape. ⚠️ `memo_present` is often False: Archicad stores that polygon only
        for polygon-based walls, so an ordinary straight wall has none. Read
        `wall_shape` ("straight" | "trapezoid" | "polygon") alongside it. The
        connection polygon is the one that exists for every wall.
      - `connected_walls` = {at_begin, at_end, to_reference_line, on_reference_line,
        crossing} — how many walls did the trimming, which is what makes a surprising
        outline explainable rather than just wrong.
    Returns [] for empty input.
    """
    guids = list(guids)
    if not guids:
        return []

    elements = [{"elementId": {"guid": str(guid)}} for guid in guids]
    data = call("Tapioca.GetWallPlanOutlines", {"elements": elements}).data or {}

    result = []
    for entry in data.get("outlines", []):
        entry = entry or {}
        connected = entry.get("connectedWalls") or {}
        result.append({
            "guid": (entry.get("elementId") or {}).get("guid", ""),
            "succeeded": bool(entry.get("succeeded", False)),
            "error": entry.get("error", ""),
            "wall_shape": entry.get("wallShape", ""),
            "outline": _ring(entry.get("outline")),
            "outline_arcs": list(entry.get("outlineArcs") or []),
            "holes": [_ring(h.get("outline")) for h in (entry.get("holes") or [])],
            "hole_arcs": [list(h.get("outlineArcs") or []) for h in (entry.get("holes") or [])],
            "outline_source": entry.get("outlineSource", ""),
            "memo_present": bool(entry.get("memoPolygonPresent", False)),
            "memo_outline": _ring(entry.get("memoOutline")),
            "memo_outline_arcs": list(entry.get("memoOutlineArcs") or []),
            "connected_walls": {
                "at_begin": connected.get("atBegin", 0),
                "at_end": connected.get("atEnd", 0),
                "to_reference_line": connected.get("toReferenceLine", 0),
                "on_reference_line": connected.get("onReferenceLine", 0),
                "crossing": connected.get("crossing", 0),
            },
        })
    return result


def set_plan_anchors(guids, enabled=True, width_pixels=2.0, color="FF3B30C0",
                     arc_sign=1, plan_z=0.0):
    """Draw those walls' plan outlines over the floor plan as ANCHORS (PLAT-RE65).

    Anchors exist to be COMPARED, not admired: if our outlines sit exactly on the
    lines Archicad drew, the analysis layer on top of them is in the right place.
    The viewer must already be running as a plan overlay — this hands geometry to
    it, and deliberately does not start one.

    `color` is 8 hex digits, RRGGBBAA. `width_pixels` is a SCREEN width, constant
    at every zoom, because that is what makes an anchor comparable with Archicad's
    own linework. Non-walls in `guids` are skipped rather than refused, so passing
    a whole selection is fine; the returned `count` says how many were walls.

    ⚠️ `arc_sign` (+1 / -1) picks which way a CURVED wall's arc bulges. The repo
    contradicts itself about Archicad's sign convention, so this is a knob to
    turn against a real curved wall rather than a guess baked into the draw.
    Straight walls are unaffected.

    Returns {count, rings, vertices, accepted}. ⚠️ `accepted` only means the
    viewer took the geometry — read evp.api.call("Tapioca.DiligentViewportState")
    for `planAnchorVertices` to find out what became of it.
    """
    guids = list(guids)
    elements = [{"elementId": {"guid": str(guid)}} for guid in guids]
    data = call("Tapioca.SetPlanAnchors", {
        "elements": elements,
        "enabled": bool(enabled),
        "widthPixels": float(width_pixels),
        "color": str(color),
        "arcSign": 1 if arc_sign >= 0 else -1,
        "planZ": float(plan_z),
    }).data or {}
    return {
        "count": data.get("count", 0),
        "rings": data.get("rings", 0),
        "vertices": data.get("vertices", 0),
        "accepted": bool(data.get("accepted", False)),
    }


def ids(guids):
    """The Element Settings "ID" box (the API's "compound info string"), per guid.

    Works for EVERY element type, because it is not read off an element struct at
    all — it comes from the guid alone. That is what makes it the natural key for a
    numbering pass over a mixed selection: no per-type branch, and no "this kind is
    not supported yet".

    Returns a list of dicts aligned to `guids`:
      {guid, found, element_id, type_name, type_id, reason}
    `found` False means either the guid is stale (`reason` "notFound") or that type
    genuinely has no ID field (`reason` "noInfoString") — different bugs in a caller,
    so they are kept apart. `element_id` is "" on a miss. Returns [] for empty input.

    `details()` already carries `element_id` on every record, so use this only when
    the ID is all you want (it is much cheaper than a full details read).
    """
    guids = list(guids)
    if not guids:
        return []

    elements = [{"elementId": {"guid": str(guid)}} for guid in guids]
    data = call("Tapioca.GetElementIds", {"elements": elements}).data or {}
    result = []
    for entry in data.get("identities", []):
        entry = entry or {}
        guid = (entry.get("elementId") or {}).get("guid", "")
        result.append({
            "guid": guid,
            "found": bool(entry.get("found", False)),
            "element_id": entry.get("value", ""),
            "type_name": entry.get("typeName", ""),
            "type_id": entry.get("typeId", 0),
            "reason": entry.get("reason", ""),
        })
    return result


def set_ids(pairs, tx=None):
    """Write the Element Settings "ID" box. `pairs` is {guid: id} or [(guid, id), ...].

    A WRITE — pass `tx` (an open evp.transaction) to fuse it with the rest of a run
    into ONE undo step; without one the dispatcher wraps this single call and the
    user gets its own undo step.

        with evp.transaction() as tx:
            evp.elements.set_ids({g: f"T-{i:03d}" for i, g in enumerate(guids)}, tx=tx)

    An empty string CLEARS the ID (a legal value); to leave one alone, omit the guid
    from `pairs` entirely.

    Returns a list of dicts aligned to the input: {guid, ok, error}. Elements whose
    type has no ID field come back ok=False with the reason in `error` rather than
    failing the batch, because a mixed selection is the normal input — check the
    list if you need all-or-nothing. Returns [] for empty input.

    ⚠️ Inside a transaction this returns a **Handle**, not data: nothing has run
    yet. After the `with` block commits, read it with `handle.result()` -- and note
    that `result()` gives the RAW wire response (camelCase keys), not the dict this
    function returns on the direct path. `handle.<anything>` is NOT an accessor: it
    silently yields a Ref for use as a later step's input.
    """
    if hasattr(pairs, "items"):
        pairs = list(pairs.items())
    items = [{"elementId": {"guid": str(guid)},
              "value": "" if value is None else str(value)}
             for guid, value in pairs]
    if not items:
        return []

    params = {"identities": items}
    if tx is not None:
        return tx.call("Tapioca.SetElementIds", params)

    data = call("Tapioca.SetElementIds", params).data or {}
    return [{"guid": (r.get("elementId") or {}).get("guid", ""),
             "ok": bool(r.get("succeeded", False)), "error": r.get("error", "")}
            for r in (data.get("results") or [])]


# The settings `set_details` will write, per kind, in the SNAKE_CASE spelling
# `details()` returns — so a caller reads a record, changes a key, and sends that
# same key back. A MIRROR of the native table
# (NativeCommands/ElementModifyCommands.cpp -> WritableFields); the add-on is the
# authority and does the rejecting. Advisory here, for a dry-run that wants to say
# "that field would be refused" without a round trip.
WRITABLE_DETAIL_FIELDS = {
    "slab":   ("level", "thickness"),
    "roof":   ("level", "thickness", "slant_angle"),   # slant_angle: PLANE roofs only
    "mesh":   ("level", "skirt_level"),
    "wall":   ("level", "thickness", "height"),
    "beam":   ("level",),
    "column": ("level", "height", "plan_angle"),
    "object": ("level", "plan_angle", "x_ratio", "y_ratio", "reflected"),
    "lamp":   ("level", "plan_angle", "x_ratio", "y_ratio", "reflected"),
    "polyline": (),
    "fill":     (),
}

# snake_case (what `details()` emits) -> the wire spelling the native command wants.
# The two halves of the round trip are named differently ONLY because Layer 2
# flattens and snake_cases every read; this table is the whole of that difference.
# A camelCase key passes through untouched, so raw-wire callers still work.
_DETAIL_FIELD_WIRE = {
    "level": "level",
    "thickness": "thickness",
    "height": "height",
    "slant_angle": "slantAngle",
    "skirt_level": "skirtLevel",
    "plan_angle": "planAngle",
    "x_ratio": "xRatio",
    "y_ratio": "yRatio",
    "reflected": "reflected",
}


def set_details(edits, tx=None):
    """Write scalar settings back into elements — the symmetric write for `details()`.

    `edits` is {guid: {field: value}} or [(guid, {field: value}), ...], with the same
    snake_case field names `details()` returns. Read, change one key, send it back:

        rec = evp.elements.details([guid])[0]
        evp.elements.set_details({guid: {"level": rec["level"] - 0.15}})

    ⚠️ SPARSE — send ONLY the keys you changed, never a whole record from `details()`.
    Echoing one back is REJECTED, deliberately: a read emits both halves of a union
    (a poly roof reports a zeroed `base_line`), and most of the record is a fact about
    the element rather than a setting (`roof_class`, `n_segments`, `library_part_name`,
    the footprint). A field this command will not write comes back as an error naming
    it — never a silent no-op.

    Writable per kind: WRITABLE_DETAIL_FIELDS above. Footprint/memo geometry (contours,
    holes, mesh sublines, roof pivot) is read-only here — that is a memo rewrite, not a
    field poke. `slant_angle` on wall/beam/column is read-only too: the slant is coupled
    to `is_slanted` and the axis endpoints there, so writing the angle alone would
    half-write the element. On a roof it IS writable, but only for a plane roof — a poly
    roof's pitch is per level.

    A WRITE — pass `tx` (an open evp.transaction) to fuse it with the rest of a run into
    ONE undo step; without one the dispatcher wraps this single call.

    Returns a list of dicts aligned to the input: {guid, ok, kind, applied, error}, where
    `applied` lists the fields actually written (empty on failure) and `kind` is the read
    side's vocabulary. Returns [] for empty input.

    ⚠️ Inside a transaction this returns a **Handle**, not data — same rule as
    `set_ids()`: nothing has run until the `with` block commits, and `handle.result()`
    gives the RAW wire response (camelCase), not these dicts.
    """
    if hasattr(edits, "items"):
        edits = list(edits.items())
    items = []
    for guid, fields in edits:
        wire = {_DETAIL_FIELD_WIRE.get(key, key): value for key, value in dict(fields).items()}
        items.append({"guid": guid, "details": wire})
    if not items:
        return []

    params = {"edits": items}
    if tx is not None:
        return tx.call("Tapioca.SetElementDetails", params)

    data = call("Tapioca.SetElementDetails", params).data or {}
    return [{
        "guid": r.get("guid", ""),
        "ok": bool(r.get("ok", False)),
        "kind": r.get("kind", ""),
        "applied": list(r.get("applied") or ()),
        "error": r.get("error", ""),
    } for r in (data.get("results") or [])]


def set_level_offset(pairs, tx=None):
    """Set each element's level offset — its height above its OWN home story.

    `pairs` is {guid: metres} or [(guid, metres), ...]. A thin spelling of
    `set_details(..., {"level": z})`, which is the one field meaning the same thing on
    every kind that has it — the native command owns the mapping onto slab/roof/mesh
    `level`, wall/column `bottomOffset`, beam `level` and object/lamp `level`.

    This is what a pull-to-mesh pass writes:
    `new_level = surface_z - story_elevation[rec["floor_ind"]]`. Negative is legal and
    required — an element whose home story sits at +3.00 m but which must rest at world
    Z 0.997 gets level = -2.003.

    ⚠️ A BEAM's `level` is its TOP relative to the story level, not its underside
    (API_BeamType::level, and `details()` reports the same field). To rest a beam ON a
    surface, subtract its own `section_height`.

    Returns what `set_details` returns; inside a transaction, a Handle.
    """
    if hasattr(pairs, "items"):
        pairs = list(pairs.items())
    return set_details([(guid, {"level": float(value)}) for guid, value in pairs], tx=tx)
