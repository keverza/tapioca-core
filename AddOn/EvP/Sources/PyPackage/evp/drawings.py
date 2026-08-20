"""Layer 2 — the crop (clip polygon) of Drawings placed on layouts.

    placed = evp.drawings.clip_polygons(guids)              # read
    result = evp.drawings.set_clip_polygon(guid, ring, tx=tx)   # write
    assert result["verified"]                               # ← always check this

THE JSON API CANNOT DO THE WRITE BY ANY PATH — all three candidates were ruled out
from source, and one of them lies:
  * Tapir `SetDetailsOfElements` — its TypeSpecificSettings is Wall + Zone only. A
    DrawingSettings object comes back SUCCESS and is silently discarded; confirmed
    live, the visible region did not change.
  * `CreateDrawings` (delete + recreate) — no clipPolygon in its schema, and it
    creates with an empty memo, so a recreated Drawing is UNCLIPPED.
  * Tapir `UpdateDrawings` — a content refresh; it never touches the clip.

⚠️ AND THE NATIVE WRITE IS ITSELF UNPROVEN: `API_DrawingID` has no row in the
`ACAPI_Element_Change` support table (that table names, per type, which memo handles
Change may edit — Slab, Roof, Mesh, Zone, Hatch, PolyLine, Detail and Worksheet all
list their polygon handles; Drawing lists nothing). So `set_clip_polygon` reads the
polygon back and reports `verified`. A NoError from the API is NOT evidence here —
branch on `verified`, never on `ok` alone.
"""

from .api import call


def clip_polygons(guids):
    """Read the crop and placement of each Drawing, aligned to `guids`.

    The layout must be the ACTIVE database — a Drawing on some other layout reads as
    found=False. Change window first (`Tapir.ChangeWindow` with the layout's
    navigatorItemId).

    Each dict: {guid, found, name, is_cut_with_frame, clip_polygon, arcs, pos,
    bounds, ratio, drawing_scale, error} where `clip_polygon` is [(x, y), ...] in the
    Drawing's own (model) coordinates, distinct points with no closing repeat — the
    same form `set_clip_polygon` takes back. When `is_cut_with_frame` is False the
    polygon is just the bounding box, not a real crop. Returns [] for empty input.
    """
    guids = list(guids)
    if not guids:
        return []

    data = call("EvP.GetDrawingClipPolygon", {"guids": guids}).data or {}
    result = []
    for entry in data.get("drawings", []):
        entry = entry or {}
        flat = entry.get("clipPolygon") or []
        pos = entry.get("pos") or {}
        bounds = entry.get("bounds") or {}
        result.append({
            "guid": entry.get("guid", ""),
            "found": bool(entry.get("found", False)),
            "name": entry.get("name", ""),
            "is_cut_with_frame": bool(entry.get("isCutWithFrame", False)),
            "clip_polygon": [(flat[i], flat[i + 1]) for i in range(0, len(flat) - 1, 2)],
            "arcs": list(entry.get("arcs") or []),
            "pos": (pos.get("x", 0.0), pos.get("y", 0.0)),
            "bounds": (bounds.get("xMin", 0.0), bounds.get("yMin", 0.0),
                       bounds.get("xMax", 0.0), bounds.get("yMax", 0.0)),
            "ratio": entry.get("ratio", 0.0),
            "drawing_scale": entry.get("drawingScale", 0.0),
            "error": entry.get("error", ""),
        })
    return result


def set_clip_polygon(guid, ring, arcs=None, cut_with_frame=True, tx=None):
    """Re-crop a placed Drawing to `ring` = [(x, y), ...] in its model coordinates.

    At least 3 distinct points; do NOT repeat the first at the end (the ring closes
    itself). `arcs` optionally gives one arc angle per edge for a curved crop.
    `cut_with_frame` defaults True because a clip polygon that is not switched on is
    exactly the silent no-op this exists to avoid — with it False the polygon is only
    a bounding box.

    Returns {ok, guid, verified, points_written, points_read_back, note, error}.

    ⚠️ Inside a transaction this returns a **Handle**, not data: nothing has run
    yet. After the `with` block commits, read it with `handle.result()` -- and note
    that `result()` gives the RAW wire response (camelCase keys), not the dict this
    function returns on the direct path. `handle.<anything>` is NOT an accessor: it
    silently yields a Ref for use as a later step's input.

    ⚠️ CHECK `verified`, not `ok`. `ok` says the API returned NoError; `verified`
    says a fresh read-back actually shows the new polygon. See the module docstring
    for why those can differ here.
    """
    flat = []
    for point in ring:
        flat.extend((point[0], point[1]))
    if len(flat) < 6:
        raise ValueError("a clip polygon needs at least 3 points (got %d)" % (len(flat) // 2))

    params = {"guid": guid, "clipPolygon": flat, "isCutWithFrame": bool(cut_with_frame)}
    if arcs:
        params["arcs"] = list(arcs)

    if tx is not None:
        return tx.call("EvP.SetDrawingClipPolygon", params)

    data = call("EvP.SetDrawingClipPolygon", params).data or {}
    return {
        "ok": bool(data.get("ok", False)),
        "guid": data.get("guid", guid),
        "verified": bool(data.get("verified", False)),
        "points_written": data.get("pointsWritten", 0),
        "points_read_back": data.get("pointsReadBack", 0),
        "note": data.get("note", ""),
        "error": data.get("error", ""),
    }


def place_from_view(view_name=None, x=0.0, y=0.0, layout_name=None, name=None,
                    ratio=None, anchor=None, angle=None, clip_polygon=None, arcs=None,
                    model_offset=None, layer=None, view_guid=None, layout_guid=None,
                    tx=None):
    """Place a Drawing of a saved project VIEW on a LAYOUT. Returns {ok, guid, ...}.

    `view_name` and `layout_name` are the names as shown in the Navigator —
    `evp.layouts.views()` and `evp.layouts.databases(["layout"])` list them.
    `x, y` are metres in the LAYOUT's space.

    ⚠️ NAMES, NOT GUIDS, are the primary inputs — a user cannot read a navigator
    guid out of Archicad's UI, so a command that demands one cannot be driven by a
    person. `view_guid` / `layout_guid` remain for script-to-script use, where one
    command's output feeds the next. An ambiguous name is a REFUSAL listing the
    candidates, never a first-match: placing a drawing of the wrong view looks like
    success and is only caught by eye.

    The source must be a SAVED VIEW, not a Project Map item. If you name a
    project-map item the refusal says so and tells you to clone it into the View
    Map first (`API.CloneProjectMapItemToViewMap`).

    ⚠️⚠️ LAYOUTS ONLY — and that is Archicad's rule, not ours. A Drawing sourced
    from a project VIEW can be placed only on a layout; one sourced from the
    FILESYSTEM can go in any 2D view. The AC29 header says it outright: "Drawing
    elements can be placed both in the model space (except which come from an
    internal view) and onto layouts." So this cannot put a view on a worksheet,
    and it refuses with that explanation rather than failing obscurely. To get 2D
    content onto a worksheet you need real 2D geometry there.

    `layout_guid` (from `evp.layouts.databases(["layout"])`) activates the target
    layout for the duration of the call and restores the previous database
    afterwards. Omitted, the caller must have activated a layout already.

    PREFER THIS OVER `set_clip_polygon` WHEN YOU CAN. Passing `clip_polygon`
    (a list of (x, y), at least 3 points, do NOT repeat the first) sets the crop at
    CREATE time via the ordinary polygon-memo path — whereas `set_clip_polygon`
    edits it afterwards through `ACAPI_Element_Change`, which for Drawings is
    undocumented (see this module's header). Placing it right beats re-cropping.
    `model_offset` is (dx, dy): which part of the source shows through the frame.

    ⚠️ Inside a transaction this returns a **Handle**, not data — see
    `set_clip_polygon` for what that means.
    """
    if not view_name and not view_guid:
        raise ValueError("place_from_view needs view_name (as shown in the Navigator) "
                         "or view_guid; evp.layouts.views() lists the placeable ones")

    params = {"x": float(x), "y": float(y)}
    if view_name:
        params["viewName"] = view_name
    if view_guid:
        params["viewGuid"] = view_guid
    if layout_name:
        params["layoutName"] = layout_name
    if layout_guid:
        params["layoutGuid"] = layout_guid
    if name:
        params["name"] = name
    if ratio is not None:
        params["ratio"] = float(ratio)
    if anchor:
        params["anchor"] = anchor
    if angle is not None:
        params["angle"] = float(angle)
    if layer:
        params["layer"] = layer
    if clip_polygon:
        flat = []
        for point in clip_polygon:
            flat.extend((float(point[0]), float(point[1])))
        if len(flat) < 6:
            raise ValueError("a clip polygon needs at least 3 points (got %d)" % (len(flat) // 2))
        params["clipPolygon"] = flat
        if arcs:
            params["arcs"] = list(arcs)
    if model_offset is not None:
        params["modelOffset"] = {"x": float(model_offset[0]), "y": float(model_offset[1])}

    if tx is not None:
        return tx.call("EvP.PlaceDrawingFromView", params)

    data = call("EvP.PlaceDrawingFromView", params).data or {}
    return {
        "ok": bool(data.get("ok", False)),
        "guid": data.get("guid", ""),
        "view_name": data.get("viewName", ""),
        "layout": data.get("layout", ""),
        "cropped": bool(data.get("cropped", False)),
        "error": data.get("error", ""),
    }


def delete_layouts(navigator_item_ids):
    """Delete layouts by NAVIGATOR ITEM id. Returns a list of {ok, error}.

    Kept here for the callers that already use it; the implementation now lives in
    `evp.layouts.delete_navigator_items`, which is the same `API.DeleteNavigatorItems`
    route and also handles subsets, views and folders. Prefer that one for new code —
    "layouts" was always too narrow a name for what the call does.
    """
    from .layouts import delete_navigator_items
    return delete_navigator_items(navigator_item_ids)
