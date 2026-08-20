"""Layer 2 — the STRUCTURED 3D model: bodies, polygons, edges, surfaces, NURBS.

    info  = evp.model.info()                       # counts + bounds of the 3D model
    elems = evp.model.elements(types=["wall"])     # the model's element table
    body  = evp.model.body_geometry(guid)          # one body's mesh BREP
    for loop in evp.model.polygon_loops(body):     # contours, holes separated
        ...
    mats  = evp.model.materials()                  # resolve polygon materialIndex

The counterpart to `evp.geometry`, and the two do not overlap:

| | `evp.geometry` | `evp.model` (here) |
|---|---|---|
| What | welded triangle soup | the modeler's own topology |
| Shape | numpy views, zero-copy | JSON records + flat index arrays |
| Speed | gate-free, thousands of rays | one gate hop per call |
| Answers | "what does the ray hit" | "which faces, which holes, what material" |

Use `evp.geometry` for anything spatial and bulk. Use this when the QUESTION is
about structure — coplanar faces, an outline with its holes, whether a body is
closed, what surface is on a face, whether a wall is actually curved.

⚠️ EVERYTHING HERE IS 1-BASED, because ModelerAPI is: element indices, body
indices, vertex/edge/polygon indices, and the corner index within a polygon.
`evp.geometry`'s numpy views are 0-based. The helpers below (`polygon_loops`,
`polygon_vertices`) do the conversion for you; if you index `vertices` by hand,
subtract one.

⚠️ POINT-IN-TIME, like every read. Nothing here is cached and nothing updates
after a write — call again.

Works in both zones: this is ordinary bus traffic, not zero-copy memory, so a
`runtime="external"` command can use all of it.
"""

from .api import call


def _element_id(guid):
    """guid -> {"elementId": {"guid": …}} — the wire's typed element identity.

    ⚠️ EVERY COMMAND IN THIS MODULE TAKES THIS, NEVER A FLAT `guid`, and their
    input schemas are closed (additionalProperties:false), so a flat one fails
    SchemaValidationFailed before the handler runs. All five call sites here had
    it wrong from E24 until 2026-08-16 — GetBodyGeometry, GetModelLights,
    GetNurbsBody, GetPointClouds and GetCutPolygons — because every one of those
    commands names its schema through a `constexpr` constant, and dryrun_command
    only read schemas written inline: they were 5 of the 21 commands it validated
    nothing about. Both halves are fixed; this helper is so there is one place
    left to get wrong. Same shape as evp.properties._element_ids.
    """
    return {"guid": str(guid)}


# --------------------------------------------------------------------------- #
#  Model + elements                                                            #
# --------------------------------------------------------------------------- #

def info():
    """The current 3D model: {guid, bounds, elementCount, materialCount, ...}.

    Cheap. Call it first — `elementCount` is what tells you whether `elements()`
    needs paging, and `guid` changes when the model is regenerated, so it is how
    you tell a cached read from a stale one.
    """
    return call("EvP.GetModelInfo").data or {}


def elements(guids=None, types=None, skip_empty=False, bounds=True,
             transform=False, offset=0, limit=0, coordinate_system="world"):
    """The model's element table -> list of records.

    Each record: {index, guid, type, typeName, tessellatedBodyCount,
    meshBodyCount, nurbsBodyCount, pointCloudCount, lightCount, bounds?,
    transform?}. `index` is what body_geometry()/nurbs_body() take.

    `types` are the lowercase names from `typeName` ("wall", "slab", "cwPanel").
    `limit`/`offset` page; the response's `totalCount` is the unpaged total.

    ⚠️ Composite elements (stair, railing, curtain wall, column, beam) appear in
    the model as their SUB-PARTS, under different guids than the one you
    selected. Filter by `types` rather than by the parent's guid.
    """
    params = {"coordinateSystem": coordinate_system,
              "include": _include(bounds=bounds, transform=transform)}
    if guids is not None:
        # ⚠️ THE WIRE IS {elements:[{elementId:{guid}}]}, NOT {guids:[…]} — the same
        # correction evp.elements.details already carries, and for the same reason.
        # This wrapper was left on the flat spelling from E24 until 2026-08-16, and
        # the native input schema is closed (additionalProperties:false), so every
        # call with a guid filter failed SchemaValidationFailed before the handler
        # ran. Nothing caught it offline: GetModelElements names its schema through
        # a `constexpr` constant and dryrun_command only read schemas written
        # inline, so this was one of 21 commands it never validated.
        params["elements"] = [{"elementId": _element_id(guid)} for guid in guids]
    if types is not None:
        params["types"] = list(types)
    if skip_empty:
        params["skipEmpty"] = True
    if offset:
        params["offset"] = int(offset)
    if limit:
        params["limit"] = int(limit)
    records = (call("EvP.GetModelElements", params).data or {}).get("elements", [])
    # The RESPONSE carries the same typed identity: `elementId: {guid}`, with no
    # top-level `guid` (the output schema is closed too). Flattening it here is
    # what makes the docstring above true, and it is the shape every caller reads
    # — a raw pass-through gave them `rec.get("guid") is None` for every record,
    # which reads as "the model has no such elements" rather than as a bug.
    return [dict(rec, guid=(rec.get("elementId") or {}).get("guid", ""))
            for rec in records]


def element_count():
    """How many elements the 3D model has, without listing them."""
    return int(info().get("elementCount", 0))


# --------------------------------------------------------------------------- #
#  Mesh bodies                                                                 #
# --------------------------------------------------------------------------- #

def body_geometry(guid=None, element_index=None, body=1, source="tessellated",
                  coordinate_system="world", include=None,
                  max_vertices=0, max_polygons=0, max_edges=0):
    """One body's mesh BREP: vertices, polygons, and optionally edges/convex/normals.

    `source` picks WHICH body, and the two are genuinely different:
      * "tessellated" — curved surfaces already broken into planar polygons.
        What a renderer wants, and what the snapshot path uses.
      * "mesh"        — faces as modelled; a curved surface is ONE polygon.
        What "how many faces does this have" wants.

    `include` is a list of sections: "vertices", "polygons", "edges", "convex",
    "normals", "vertexHardFlags", or "all". Default is vertices + polygons;
    edges and the convex decomposition are off because they are large.

    The `max_*` caps are a safety valve for a dense body — the response says
    `truncated` when one bites. Read `body.vertexCount` first if in doubt.
    """
    params = {"body": int(body), "source": source,
              "coordinateSystem": coordinate_system}
    if guid is not None:
        params["elementId"] = _element_id(guid)
    if element_index is not None:
        params["elementIndex"] = int(element_index)
    if include is not None:
        params["include"] = list(include)
    for key, value in (("maxVertices", max_vertices), ("maxPolygons", max_polygons),
                       ("maxEdges", max_edges)):
        if value:
            params[key] = int(value)
    return call("EvP.GetBodyGeometry", params).data or {}


def vertices(body):
    """A body response's flat `vertices` -> [(x, y, z), ...], 0-based.

    Vertex index i from `polygons.vertexIndices` (1-based) is `result[i - 1]`.
    """
    flat = body.get("vertices") or []
    return [(flat[i], flat[i + 1], flat[i + 2]) for i in range(0, len(flat) - 2, 3)]


def polygon_vertices(body, polygon_index):
    """The raw corner vertex INDICES of one polygon (1-based within the body).

    `polygon_index` is 1-based, matching the wire format. Includes the hole
    markers untouched — use polygon_loops() if you want them resolved.
    """
    polygons = body.get("polygons") or {}
    counts = polygons.get("edgeCounts") or []
    indices = polygons.get("vertexIndices") or []
    start = sum(counts[:polygon_index - 1])
    return indices[start:start + counts[polygon_index - 1]]


def polygon_loops(body, polygon_index):
    """One polygon as [outer_loop, hole1, ...], each a list of (x, y, z).

    THE REASON THIS EXISTS. A polygon's corners arrive as two parallel lists —
    vertex indices and SIGNED edge indices — and an edge index of ZERO is a
    CONTOUR BREAK: everything after it belongs to a hole, not the outer boundary.
    Read the vertex list alone and a window opening becomes part of the wall
    face's outline, which looks like a wall with a bite taken out of it and is
    not detectably wrong until you draw it.

    So: split on the zero markers, and drop the marker corners themselves. The
    first loop is the outer boundary; the rest are holes. A simple polygon comes
    back as a single loop, so this is always the right call.

    Needs `include` to have contained "polygons" (the default) and "vertices".
    """
    polygons = body.get("polygons") or {}
    counts = polygons.get("edgeCounts") or []
    if not 1 <= polygon_index <= len(counts):
        return []
    vertex_indices = polygons.get("vertexIndices") or []
    edge_indices = polygons.get("edgeIndices") or []
    points = vertices(body)

    start = sum(counts[:polygon_index - 1])
    stop = start + counts[polygon_index - 1]

    loops, current = [], []
    for i in range(start, stop):
        # A zero EDGE index ends the contour. The corner it sits on is the
        # marker, not a real vertex, so it is not emitted.
        if i < len(edge_indices) and edge_indices[i] == 0:
            if current:
                loops.append(current)
            current = []
            continue
        vi = vertex_indices[i]
        if 1 <= vi <= len(points):
            current.append(points[vi - 1])
    if current:
        loops.append(current)
    return loops


def triangles(body):
    """The convex decomposition as fan triangles: [((x,y,z), (x,y,z), (x,y,z)), ...].

    Reproduces in Python what GeometryExtractor does in C++, from the same
    source data — useful to audit a snapshot, or to triangulate ONE body without
    building a whole snapshot. Needs `include=["vertices", "convex"]`.

    For bulk work use `evp.geometry` instead: this goes through JSON and the
    snapshot path does not.
    """
    convex = body.get("convex") or {}
    counts = convex.get("vertexCounts") or []
    indices = convex.get("vertexIndices") or []
    points = vertices(body)

    out, cursor = [], 0
    for n in counts:
        corners = [points[i - 1] for i in indices[cursor:cursor + n]
                   if 1 <= i <= len(points)]
        cursor += n
        for k in range(1, len(corners) - 1):
            out.append((corners[0], corners[k], corners[k + 1]))
    return out


def edges(body):
    """A body response's `edges` block -> [{v1, v2, p1, p2, invisible}, ...].

    `p1`/`p2` are the polygons sharing the edge, -1 when there is none — which
    is how you find the open boundary of a surface body. Needs
    `include=["edges"]`.
    """
    block = body.get("edges") or {}
    v1 = block.get("vertex1") or []
    v2 = block.get("vertex2") or []
    p1 = block.get("polygon1") or []
    p2 = block.get("polygon2") or []
    invisible = block.get("invisible") or []
    return [
        {"v1": v1[i], "v2": v2[i],
         "p1": p1[i] if i < len(p1) else -1,
         "p2": p2[i] if i < len(p2) else -1,
         "invisible": bool(invisible[i]) if i < len(invisible) else False}
        for i in range(len(v1))
    ]


# --------------------------------------------------------------------------- #
#  Appearance                                                                  #
# --------------------------------------------------------------------------- #

def materials(indices=None):
    """The model's surface pool -> list of material records.

    What a polygon's `materialIndex` (and a snapshot's `triMaterial`) actually
    means. Colours are 0..1 doubles, transparency 0..1 where 1 is fully
    transparent.

    ⚠️ These are MODEL indices, not Archicad attribute indices — the model pool
    only contains surfaces the model uses, renumbered. `evp.api.call
    ("EvP.GetAttributeInfo", ...)` speaks the other kind.
    """
    params = {"indices": [int(i) for i in indices]} if indices else {}
    return (call("EvP.GetModelMaterials", params).data or {}).get("materials", [])


def material_map(indices=None):
    """materials() keyed by modelIndex — the join a per-surface export wants."""
    return {m["modelIndex"]: m for m in materials(indices)}


def colors(indices=None):
    """The pen-colour pool -> [{modelIndex, red, green, blue}, ...]."""
    return (call("EvP.GetModelColors", {"indices": [int(i) for i in indices]}
                 if indices else {}).data or {}).get("colors", [])


def textures(used_only=False, indices=None):
    """Texture METADATA (never pixels) -> list of records with checksum/size/flags.

    `checksum` identifies the image: an exporter writes each bitmap once and
    references it N times off this. Use texture_pixels() for the samples.
    """
    params = {"usedOnly": bool(used_only)}
    if indices:
        params["indices"] = [int(i) for i in indices]
    return (call("EvP.GetModelTextures", params).data or {}).get("textures", [])


def texture_pixels(index=None, name=None, x=0, y=0, width=0, height=0, max_pixels=0):
    """A REGION of one texture as ARGB bytes: {width, height, truncated, pixels}.

    ⚠️ Capped, deliberately. A 2048x2048 texture is 4.2M pixels and as JSON
    integers that is not a slow response, it is a hung Archicad. The default cap
    is 65536 pixels (256x256) and truncation is by whole ROWS, so the result is
    always a decodable rectangle. Walk a big image in tiles via x/y/width/height.

    `pixels` is flat, four bytes per pixel, ARGB order (not RGBA).
    """
    params = {"x": int(x), "y": int(y)}
    if index is not None:
        params["index"] = int(index)
    if name is not None:
        params["name"] = name
    for key, value in (("width", width), ("height", height), ("maxPixels", max_pixels)):
        if value:
            params[key] = int(value)
    return call("EvP.GetTexturePixels", params).data or {}


def lights_report(guid=None, element_index=None, specials=True, parameters=False,
                  coordinate_system="world"):
    """The FULL light response: {count, lights, skippedCount, skipped?, ...}.

    ⚠️ USE THIS, NOT lights(), WHEN THE COUNT HAS TO BE TRUSTED. A light that
    cannot be described is skipped by index rather than taking the whole call
    down with it, so `lights` can legitimately be shorter than the model's
    `lightCount` — and `skipped` is the only place that says so. Confirmed live
    2026-08-03: a project reporting 280 lights returned 3, because model light
    indices 1..3 are the synthetic ambient/camera/sun (Light.hpp) and every real
    lamp after them failed to describe. Reading `lights` alone, that looks like a
    project with three lights.
    """
    params = {"specials": bool(specials), "coordinateSystem": coordinate_system,
              "include": ["parameters"] if parameters else []}
    if guid is not None:
        params["elementId"] = _element_id(guid)
    if element_index is not None:
        params["elementIndex"] = int(element_index)
    return call("EvP.GetModelLights", params).data or {}


def lights(guid=None, element_index=None, specials=True, parameters=False,
           coordinate_system="world"):
    """Every light in the model, or one element's -> list of light records.

    With `specials` (default) the ambient/camera/SUN lights come too. The sun is
    the one most callers want: it is the shadow direction, and nothing else in
    EvP exposes it. Look for `typeName == "sun"`.

    ⚠️ This can be SHORTER than the model's lightCount — see lights_report().
    """
    return lights_report(guid, element_index, specials, parameters,
                         coordinate_system).get("lights", [])


def sun():
    """The sun light record, or None. Shorthand for the shadow direction."""
    for light in lights(specials=True):
        if light.get("typeName") == "sun":
            return light
    return None


def texture_coordinates(guid, polygon, points, body=1, source="tessellated"):
    """UV coordinates for WORLD-space points on one polygon -> [(u, v), ...].

    `points` is a sequence of (x, y, z) IN WORLD COORDINATES that lie ON that
    polygon. The API projects rather than validates, so a point off the face
    returns a plausible, meaningless UV — take the points from the same body's
    `vertices` and you cannot get this wrong.
    """
    flat = [float(v) for p in points for v in p]
    data = call("EvP.GetTextureCoordinates",
                {"guid": guid, "polygon": int(polygon), "body": int(body),
                 "source": source, "points": flat}).data or {}
    us, vs = data.get("u") or [], data.get("v") or []
    return list(zip(us, vs))


# --------------------------------------------------------------------------- #
#  NURBS + point clouds                                                        #
# --------------------------------------------------------------------------- #

def nurbs_body(guid=None, element_index=None, body=1, include=None,
               coordinate_system="world"):
    """One NURBS body: the full vertex->edge->trim->loop->face->shell->lump tree.

    Shells, morphs and revolved objects are NURBS; walls and slabs are not.
    Check `nurbsBodyCount` from elements() first — this refuses rather than
    inventing an empty tree.

    Topology sections are on by default. The GEOMETRY pools — "curves2d",
    "curves3d", "surfaces" — are OFF by default because each control net and
    knot vector is real bulk; ask for them explicitly when you need the actual
    curve, e.g. `include=["faces", "surfaces"]`.

    ⚠️ The accessors are 1-based but the index VALUES they return are 0-based
    into the body's pools. That mismatch is the API's; the response passes both
    through unchanged rather than silently normalising one side.
    """
    params = {"body": int(body), "coordinateSystem": coordinate_system}
    if guid is not None:
        params["elementId"] = _element_id(guid)
    if element_index is not None:
        params["elementIndex"] = int(element_index)
    if include is not None:
        params["include"] = list(include)
    return call("EvP.GetNurbsBody", params).data or {}


def point_clouds(guid=None, element_index=None, coordinate_system="world"):
    """Surveyed point clouds -> [{guid, cloudIndex, bounds, transform}, ...].

    Bounds and the data->world matrix (16 floats, row-major); never the points —
    a cloud is millions of them and no bus should carry that.
    """
    params = {"coordinateSystem": coordinate_system}
    if guid is not None:
        params["elementId"] = _element_id(guid)
    if element_index is not None:
        params["elementIndex"] = int(element_index)
    return (call("EvP.GetPointClouds", params).data or {}).get("pointClouds", [])


# --------------------------------------------------------------------------- #
#  C API component database                                                    #
# --------------------------------------------------------------------------- #

def component_counts():
    """{bodyCount, lightCount, materialCount} of the active 3D database.

    Only these three are countable — polygons/edges/vertices are counted PER
    BODY (on the body record), not globally.
    """
    return call("EvP.Get3DComponentCounts").data or {}


def element_3d_info(guids):
    """Which BODY indices belong to each element -> list of records, parallel to input.

    ⚠️ This CONVERTS THE ELEMENT TO 3D on demand — it works with the 3D window
    never opened, and its result deliberately contains no perspective cuts or 3D
    cut planes. It is the element's own solid, not what is on screen. That is
    what you want for a measurement and never what you want for a render.

    Per the DevKit this is also the only route to WALL HOLE information: walk the
    body's polygons and look for normals horizontal and perpendicular to the
    wall's reference line.
    """
    return (call("EvP.GetElement3DInfo", {"guids": list(guids)}).data or {}).get("elements", [])


def body_components(body, include=None):
    """One C-API body's record and sub-components: vertices, polygons, polyEdges, edges.

    THE CONTOUR WALK, which is why this shape exists. A polygon holds
    `firstPolyEdge..lastPolyEdge`, a range into `polyEdges.edge`; each polyEdge
    is a SIGNED edge index (negative = traverse that edge backwards, ZERO = end
    of contour, a hole follows); each edge holds its two vertices. Use
    `body_polygon_loops()` rather than reassembling that by hand.

    ⚠️ STATEFUL. Fetching the body is what makes its sub-components reachable,
    which is why one call does both and there is no standalone "get polygon".
    Anything else you call afterwards that reads sub-components acts on THIS
    body.
    """
    params = {"body": int(body)}
    if include is not None:
        params["include"] = list(include)
    return call("EvP.GetBodyComponents", params).data or {}


def body_polygon_loops(components, polygon_index):
    """A C-API polygon as [outer_loop, hole1, ...], each a list of (x, y, z).

    `polygon_index` is 1-based. Needs `include` to have covered vertices,
    polygons, polyEdges and edges — i.e. `include=["all"]`, or the default plus
    "edges".
    """
    polygons = components.get("polygons") or {}
    poly_edges = (components.get("polyEdges") or {}).get("edge") or []
    edge_block = components.get("edges") or {}
    edge_v1 = edge_block.get("vertex1") or []
    edge_v2 = edge_block.get("vertex2") or []
    flat = components.get("vertices") or []
    points = [(flat[i], flat[i + 1], flat[i + 2]) for i in range(0, len(flat) - 2, 3)]

    first = polygons.get("firstPolyEdge") or []
    last = polygons.get("lastPolyEdge") or []
    if not 1 <= polygon_index <= len(first):
        return []

    loops, current = [], []
    for pe in range(first[polygon_index - 1], last[polygon_index - 1] + 1):
        if not 1 <= pe <= len(poly_edges):
            continue
        signed = poly_edges[pe - 1]
        if signed == 0:                 # contour break: a hole starts here
            if current:
                loops.append(current)
            current = []
            continue
        edge = abs(signed)
        if not 1 <= edge <= len(edge_v1):
            continue
        # A negative polyEdge means traverse the edge the other way, so the
        # STARTING vertex of the directed edge is vert2 rather than vert1.
        vi = edge_v1[edge - 1] if signed > 0 else edge_v2[edge - 1]
        if 1 <= vi <= len(points):
            current.append(points[vi - 1])
    if current:
        loops.append(current)
    return loops


def decompose_polygon(polygon):
    """Split one 3D-database polygon into CONVEX pieces -> [[vertex_index, ...], ...].

    Generates no new points: the indices are the body's own VERT indices.
    ⚠️ The polygon must belong to the body you last fetched with
    body_components().
    """
    data = call("EvP.DecomposePolygon", {"polygon": int(polygon)}).data or {}
    counts = data.get("vertexCounts") or []
    indices = data.get("vertexIndices") or []
    out, cursor = [], 0
    for n in counts:
        out.append(indices[cursor:cursor + n])
        cursor += n
    return out


def texture_coord_at_point(elem_idx, body_idx, polygon, points):
    """The C-API UV route -> [(u, v), ...].

    ⚠️ DIFFERENT CONVENTIONS FROM texture_coordinates(), in both directions:
    `elem_idx`/`body_idx` are 0-BASED (they are `elemIndexPlus1 - 1` and
    `bodyIndexPlus1 - 1` from body_components()' body record), and `points` are
    LOCAL, not world. Getting either wrong yields plausible wrong numbers rather
    than an error.
    """
    flat = [float(v) for p in points for v in p]
    data = call("EvP.GetTextureCoordAtPoint",
                {"elemIdx": int(elem_idx), "bodyIdx": int(body_idx),
                 "polygon": int(polygon), "points": flat}).data or {}
    return list(zip(data.get("u") or [], data.get("v") or []))


# --------------------------------------------------------------------------- #
#  Sections, building materials, connections                                   #
# --------------------------------------------------------------------------- #

def cut_polygons(guid=None, elem_idx=None, body=None, base_point=(0.0, 0.0, 0.0),
                 normal=(0.0, 0.0, 1.0), separate_components=False):
    """Archicad's own cross-section of a solid at a plane, with the AREA.

    Returns {totalArea, bodies:[{bodyIdx, area, polygons:[...]}]}. Each polygon
    carries `coords` (flat world xyz), `uv` (flat, in the cut plane) and
    `contourVertexCounts` — first contour is the outer boundary, the rest holes.

    `separate_components=True` regenerates the element split into its components
    first, so a composite wall yields one body per skin and each area is per
    material. That is the takeoff mode, and it lines up body-for-body with
    building_materials().

    Distinct from `evp.geometry.slice_z`, which cuts the TRIANGLE snapshot: this
    is the real solid and it gives you the area.
    """
    params = {"plane": {"basePoint": _xyz(base_point), "normal": _xyz(normal)},
              "separateComponents": bool(separate_components)}
    if guid is not None:
        params["elementId"] = _element_id(guid)
    if elem_idx is not None:
        params["elemIdx"] = int(elem_idx)
    if body is not None:
        params["body"] = int(body)
    return call("EvP.GetCutPolygons", params).data or {}


def building_materials(guids):
    """Which BUILDING MATERIAL each body of each element is -> list of records.

    The structural material, not the visible surface. With the separate-
    components model a composite wall is one body PER SKIN, so this is what
    makes a composite legible from its geometry.
    """
    return (call("EvP.GetBodyBuildingMaterials", {"guids": list(guids)}).data
            or {}).get("elements", [])


def connections(guids=None):
    """Which elements TOUCH, and over what surface -> list of pair records.

    ⚠️ Not the same question as `evp.topology.collisions`. That reports elements
    whose solids OVERLAP (a clash test); this reports elements that are
    CONNECTED and hands back the connecting polygon. A slab sitting exactly on a
    wall clashes with nothing and connects over its whole bearing area.

    Omit `guids` and every element with 3D geometry is considered — that is a
    whole-project operation, so pass a list when you can.
    """
    params = {"guids": list(guids)} if guids else {}
    return (call("EvP.GetConnectionTable", params).data or {}).get("connections", [])


# --------------------------------------------------------------------------- #
#  internals                                                                   #
# --------------------------------------------------------------------------- #

def _include(**sections):
    """{"bounds": True, "transform": False} -> ["bounds"].

    The native side reads an ABSENT `include` as "the per-section defaults" and a
    PRESENT one as an exact list, so this always sends a list — the keyword
    arguments above are the documented default, not the C++ side's.
    """
    return [name for name, wanted in sections.items() if wanted]


def _xyz(point):
    """(x, y, z) -> {"x": …, "y": …, "z": …}, floats.

    ⚠️ The float() is NOT cosmetic. ObjectState::Get returns FALSE on a JSON type
    mismatch rather than converting, so a literal `(0, 0, 1)` written without
    decimal points arrives as integers and is dropped on the floor — which looks
    exactly like "the cut found nothing". Same trap `evp.geometry._vec` documents.
    """
    return {"x": float(point[0]), "y": float(point[1]), "z": float(point[2])}
