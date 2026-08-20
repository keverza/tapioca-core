"""Layer 2 — zero-copy geometry as numpy arrays.

    snap = evp.geometry.snapshot()          # builds + describes the live snapshot
    for mesh in snap.meshes:
        v = mesh.vertices()                 # (n, 3) float64 — a VIEW, no copy
        t = mesh.triangles()                # (m, 3) uint32  — a VIEW, no copy

LIFETIME (why this is safe): every array holds a strong reference to the snapshot
through its `base` chain — the buffer object owns a token that is a shared_ptr on
the C++ side. `EvP.ReleaseSnapshot` drops only the STORE's reference, so the
memory survives until the last view dies. No dangling pointer is possible.

STALENESS: views are explicitly POINT-IN-TIME. Writing does NOT update any
snapshot — rebuild with snapshot() again if you need post-write geometry. Every
buffer carries its snapshot_id so a trace shows which state a read came from.

Arrays are read-only (`WRITEABLE=False`): snapshots are immutable and shared.

ZONE B ONLY — the one place the two zones are NOT transparent, and unavoidably so:
zero-copy means pointing numpy at the add-on's own memory, which a `runtime="external"`
subprocess does not share. Importing this module is always safe; calling a view from
Zone C raises with an explanation rather than failing at import (which would break
`import evp` for every external command, geometry or not).
"""

import json as _json

from .api import call


def _bridge():
    """The in-process bridge, or a clear explanation of why there isn't one."""
    try:
        import _evp
    except ImportError:
        raise RuntimeError(
            "evp.geometry needs zero-copy access to the add-on's snapshot memory, "
            "which only exists inside Archicad's own process. This command declares "
            "runtime=\"external\", so it runs in a subprocess with no shared memory. "
            "Use runtime=\"embedded\" for geometry work, or read geometry over the "
            "HTTP data plane (/meshes, /mesh) if it must stay external."
        ) from None
    return _evp


class Mesh:
    """One element's geometry, as views into the live snapshot."""

    __slots__ = ("index", "guid", "elem_type", "vertex_count", "triangle_count", "snapshot_id")

    def __init__(self, index, guid, elem_type, vertex_count, triangle_count, snapshot_id):
        self.index = index
        self.guid = guid
        self.elem_type = elem_type
        self.vertex_count = vertex_count
        self.triangle_count = triangle_count
        self.snapshot_id = snapshot_id

    def _view(self, kind):
        import numpy as np

        buffer, meta_json = _bridge().acquire_buffer(
            _json.dumps({"kind": kind, "mesh": self.index})
        )
        meta = _json.loads(meta_json)
        # frombuffer does not copy; `buffer` becomes the array's base, which is
        # what keeps the snapshot alive for exactly as long as the array lives.
        array = np.frombuffer(buffer, dtype=meta["dtype"])
        rows, cols = meta["shape"]
        return array.reshape(rows, cols) if cols > 1 else array

    def vertices(self):
        """(n, 3) float64 world-space metres. Read-only view."""
        return self._view("vertices")

    def normals(self):
        """(n, 3) float32 per-vertex normals. Read-only view."""
        return self._view("normals")

    def triangles(self):
        """(m, 3) uint32 vertex indices. Read-only view."""
        return self._view("triangles")

    def tri_material(self):
        """(m,) int32 material index per triangle. Read-only view."""
        return self._view("triMaterial")

    def __repr__(self):
        return "<evp.Mesh %s type=%d verts=%d tris=%d>" % (
            self.guid, self.elem_type, self.vertex_count, self.triangle_count
        )


class Snapshot:
    __slots__ = ("id", "scope", "meshes")

    def __init__(self, info):
        self.id = info.get("snapshotId")
        self.scope = info.get("scope")
        guids = info.get("guids") or []
        elem_types = info.get("elemTypes") or []
        vertex_counts = info.get("vertexCounts") or []
        triangle_counts = info.get("triangleCounts") or []
        self.meshes = [
            Mesh(i, guids[i], elem_types[i], vertex_counts[i], triangle_counts[i], self.id)
            for i in range(len(guids))
        ]

    def release(self):
        """Hand the snapshot back to Archicad.

        Only drops the STORE's reference — any numpy view you still hold keeps
        its memory alive, and stays valid.
        """
        call("EvP.ReleaseSnapshot", raise_on_error=False)

    def __repr__(self):
        return "<evp.Snapshot id=%s scope=%s meshes=%d>" % (self.id, self.scope, len(self.meshes))


# ---------------------------------------------------------------------------
# E2 — the data plane. These reach gate-free native commands (no main-thread hop),
# so they are cheap enough to call in a loop; prefer ray_all_batch anyway when the
# ray count is large, because the per-call envelope then dominates.
# ---------------------------------------------------------------------------

def _vec(values):
    """Coerce to a list of floats before it crosses the bus.

    ⚠️ NOT cosmetic. The C++ side reads these as GS::Array<double>, and
    ObjectState::Get returns FALSE on a type mismatch rather than converting —
    silently, leaving the array empty. A Python literal like [0, 0, 1] or a -100
    written without a decimal point therefore arrives as JSON integers and can be
    dropped on the floor, which looks exactly like "the query found nothing".
    Coercing here means no caller has to remember to write 0.0 instead of 0.
    """
    return [float(v) for v in values]

def raycast(origin, direction, max_dist=0.0):
    """First surface hit. {hit, t, guid, elemType, point, normal} — hit False if none."""
    return call("EvP.Raycast", {"origin": _vec(origin), "direction": _vec(direction),
                                "maxDist": float(max_dist)}).data or {}


def ray_all(origin, direction, max_dist=0.0, max_hits=0):
    """Every surface the ray passes through, sorted by t.

    Returns {"hits": [{t, enter, guid, elemType, point, normal}, ...], "truncated": bool}.
    Back faces are NOT culled, so a solid contributes an entry AND an exit — that is
    what `enter` is for, and what solid_spans() consumes.
    """
    data = call("EvP.RaycastAll", {"origin": _vec(origin), "direction": _vec(direction),
                                   "maxDist": float(max_dist), "maxHits": int(max_hits)}).data or {}
    return {"hits": _unpack_hits(data), "truncated": bool(data.get("truncated", False))}


def ray_all_batch(origins, directions, max_dist=0.0, max_hits=0):
    """N pierce rays in ONE call -> list of per-ray {"hits": [...], "truncated": bool}.

    A clearance heatmap is thousands of rays; one envelope per ray would dominate
    the runtime even without a gate hop. `origins`/`directions` are sequences of
    (x, y, z).
    """
    flat_o = [float(v) for p in origins for v in p]
    flat_d = [float(v) for p in directions for v in p]
    data = call("EvP.RaycastAllBatch", {"origins": flat_o, "directions": flat_d,
                                        "maxDist": float(max_dist),
                                        "maxHits": int(max_hits)}).data or {}

    counts = data.get("hitCounts") or []
    truncated = data.get("truncated") or []
    out, start = [], 0
    for i, n in enumerate(counts):
        out.append({
            "hits": _unpack_hits(data, start, start + n),
            "truncated": bool(truncated[i]) if i < len(truncated) else False,
        })
        start += n
    return out


def _unpack_hits(data, start=0, stop=None):
    """Flat parallel arrays -> a list of hit dicts.

    The wire format is flat because GS::Array<GS::ObjectState> is unproven in this
    codebase (see GetSnapshotInfo); the unpacking lives here so no caller has to
    know that.
    """
    ts = data.get("t") or []
    stop = len(ts) if stop is None else stop
    enters = data.get("enter") or []
    guids = data.get("guids") or []
    types = data.get("elemTypes") or []
    points = data.get("points") or []
    normals = data.get("normals") or []

    hits = []
    for i in range(start, min(stop, len(ts))):
        hits.append({
            "t": ts[i],
            "enter": bool(enters[i]) if i < len(enters) else False,
            "guid": guids[i] if i < len(guids) else None,
            "elemType": types[i] if i < len(types) else None,
            "point": points[i * 3:i * 3 + 3],
            "normal": normals[i * 3:i * 3 + 3],
        })
    return hits


def closest_point(point, max_dist=0.0):
    """Nearest point on any surface. {found, dist, guid, point}."""
    return call("EvP.ClosestPoint", {"point": _vec(point), "maxDist": float(max_dist)}).data or {}


def nearest_elements(point, k=5):
    """The k nearest elements by AABB distance. {guids, dists, count}."""
    return call("EvP.NearestElements", {"point": _vec(point), "k": int(k)}).data or {}


def query_box(minimum, maximum):
    """GUIDs whose bounding box overlaps this box. Broadphase — AABB, not exact."""
    return (call("EvP.Query", {"shape": "box", "min": _vec(minimum),
                               "max": _vec(maximum)}).data or {}).get("guids", [])


def query_sphere(center, radius):
    """GUIDs whose bounding box overlaps this sphere. Broadphase."""
    return (call("EvP.Query", {"shape": "sphere", "center": _vec(center),
                               "radius": float(radius)}).data or {}).get("guids", [])


def query_polygon(polygon_xy, zmin, zmax):
    """GUIDs inside a prism: polygon [x0,y0,x1,y1,...] extruded over [zmin,zmax]."""
    return (call("EvP.Query", {"shape": "polygon",
                               "polygon": [float(v) for v in polygon_xy],
                               "zmin": float(zmin), "zmax": float(zmax)}).data or {}).get("guids", [])


def solid_spans(hits, tol=1e-3):
    """Collapse a pierce stack into solid spans [(t_in, t_out), ...] along the ray.

    ⚠️ Do NOT read the stack as "first exit = floor, next enter = ceiling". That
    breaks whenever a floor is built from stacked elements (structural slab plus a
    separate topping): their shared interface produces an exit and an enter at the
    SAME t, so the naive rule sees floor == ceiling and reports ~0 clearance,
    blanking the whole floor.

    Instead walk a solid-depth counter (+1 enter, -1 exit) and — the part that
    actually matters — NET all hits at coincident t before testing the depth. At a
    slab/topping interface the exit and enter cancel (delta 0), so depth never dips
    and the two elements read as one solid. A plain single slab reduces to the
    naive rule, so nothing is lost.

    `tol` (metres) also absorbs sub-millimetre modelling gaps between stacked
    elements, which would otherwise register as a spurious void.

    Handles a ray that starts INSIDE solid geometry: depth is renormalised so it
    never goes negative, and such a span starts at t = 0.
    """
    if not hits:
        return []
    ordered = sorted(hits, key=lambda h: h["t"])

    # Group coincident hits and net their deltas.
    groups = []
    i, n = 0, len(ordered)
    while i < n:
        t0 = ordered[i]["t"]
        delta = 0
        j = i
        while j < n and ordered[j]["t"] - t0 < tol:
            delta += 1 if ordered[j]["enter"] else -1
            j += 1
        groups.append((t0, delta))
        i = j

    # If the ray began inside solids the running depth would go negative; lift the
    # starting depth so it never does.
    depth, lowest = 0, 0
    for _, d in groups:
        depth += d
        lowest = min(lowest, depth)
    depth = -lowest

    spans = []
    start = 0.0 if depth > 0 else None
    for t, d in groups:
        previous = depth
        depth += d
        if previous <= 0 and depth > 0:
            start = t
        elif previous > 0 and depth <= 0 and start is not None:
            spans.append((start, t))
            start = None
    return spans


def clear_height(x, y, z_from=-1000.0, z_to=1000.0, tol=1e-3):
    """Floor-top and ceiling-underside above a point, read off the pierce stack.

    Correct with stepped floors, mezzanines, hanging services — and with floors
    assembled from stacked elements, which a naive first-exit/next-enter read
    silently reports as 0 clearance. See solid_spans().

    Fires a ray straight up from (x, y, z_from), which must be BELOW the floor.
    Returns {floor_z, ceiling_z, height, floor_guid, ceiling_guid, truncated}, or
    None when there is no floor or nothing above it.
    """
    result = ray_all([x, y, z_from], [0.0, 0.0, 1.0], max_dist=(z_to - z_from))
    hits = result["hits"]
    spans = solid_spans(hits, tol)
    if len(spans) < 2:
        return None                        # need a floor AND something above it

    floor_t = spans[0][1]                  # top of the (possibly layered) floor
    ceiling_t = spans[1][0]                # underside of the next solid up

    def at(t, entering):
        return next((h for h in hits
                     if abs(h["t"] - t) < tol and h["enter"] == entering), None)

    floor_hit, ceiling_hit = at(floor_t, False), at(ceiling_t, True)
    return {
        "floor_z": z_from + floor_t,
        "ceiling_z": z_from + ceiling_t,
        "height": ceiling_t - floor_t,
        "floor_guid": floor_hit["guid"] if floor_hit else None,
        "ceiling_guid": ceiling_hit["guid"] if ceiling_hit else None,
        # If the stack was capped, surfaces above the cap are missing entirely and
        # this number is wrong rather than approximate.
        "truncated": bool(result.get("truncated", False)),
    }


def slice_z(z, types=None, guids=None):
    """Horizontal cross-section at world height *z* — plan outlines of elements.

    Returns ``[{guid, elem_type, loops: [[(x, y), ...]]}]``, one entry per source
    element. Loops are 2D (z dropped). This is the shape consumers need for
    per-element drawing (walls vs beams vs openings on different layers) and
    per-element masking (grid points inside a wall plan are never sampled).

    Pure Python over the native ``EvP.SliceZ`` flat parallel arrays — no C++ change.
    """
    params = {"z": float(z)}
    if types is not None:
        params["types"] = [int(t) for t in types]
    if guids is not None:
        # ⚠️ The wire is {elements:[{elementId:{guid}}]}, not {guids:[…]}, and
        # SliceZ's input schema is closed. See evp.model._element_id.
        params["elements"] = [{"elementId": {"guid": str(guid)}} for guid in guids]
    res = call("EvP.SliceZ", params, raise_on_error=False)
    # ⚠️ The ENVELOPE is the only success channel. SliceZ's registered output is
    # {zUsed, nudged, loopCount, coords, loopPointCounts, loopClosed, loopGuids,
    # loopElemTypes} with additionalProperties:false — there is no `ok` in `data`
    # and there never can be, so testing for one returned [] from every call that
    # had in fact succeeded.
    if not res.ok:
        return []
    data = res.data or {}

    coords = data.get("coords") or []
    counts = data.get("loopPointCounts") or []
    closed = data.get("loopClosed") or []
    loop_guids = data.get("loopGuids") or []
    loop_types = data.get("loopElemTypes") or []

    raw_loops = []
    j = 0
    for n in counts:
        pts = []
        for _ in range(int(n)):
            pts.append((coords[3 * j], coords[3 * j + 1]))
            j += 1
        raw_loops.append(pts)

    from collections import OrderedDict
    by_guid = OrderedDict()
    for idx, loop_pts in enumerate(raw_loops):
        guid = loop_guids[idx] if idx < len(loop_guids) else None
        etype = loop_types[idx] if idx < len(loop_types) else None
        key = guid or ("_unknown_%d" % idx)
        if key not in by_guid:
            by_guid[key] = {"guid": guid, "elem_type": etype, "loops": []}
        by_guid[key]["loops"].append(loop_pts)
    return list(by_guid.values())


def snapshot(scope="all", rebuild=True, exclude_types=None):
    """Build (by default) and describe the live snapshot.

    Pass rebuild=False to describe whatever is already live.
    """
    if rebuild:
        params = {"scope": scope}
        if exclude_types:
            params["excludeTypes"] = list(exclude_types)
        call("EvP.BuildSnapshot", params)
    return Snapshot(call("EvP.GetSnapshotInfo").data or {})
