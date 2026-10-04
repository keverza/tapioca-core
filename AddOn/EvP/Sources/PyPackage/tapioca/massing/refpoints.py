"""Reference elevation points for the Allowed Building Envelope workflow.

Two derived elevations are needed:
- the average parcel elevation, from 4-5 key points on the property line;
- the average building-zone elevation, from the same rule applied to a separately
  defined area.

Pure stdlib: runs under pytest and is reused by the production command. Projection
to the terrain is injected as `sample_z_xy`, so tests need no model.

Default placement prioritises property segment endpoints. Deterministic farthest-point
sampling spreads them around the parcel without depending on the ring start/winding.
Only when there are fewer distinct endpoints than requested do boundary midpoints
fill the remaining slots. Coincident references must never bias the elevation mean.
"""

from __future__ import annotations

import math

from .setbackgeom import closest_point_on_ring, point_in_ring

#: A point is "the same" as another below this separation (metres).
DEDUPE_TOLERANCE_M = 0.05


def bbox(ring):
    xs = [p[0] for p in ring]
    ys = [p[1] for p in ring]
    return min(xs), min(ys), max(xs), max(ys)


def snap_to_ring(ring, point_xy, closed=True):
    """The closest point on the ring, so every key point lies on the boundary."""
    foot, _ = closest_point_on_ring(ring, point_xy, closed=closed)
    return foot


def default_key_points(ring, count=4, dedupe_tolerance=DEDUPE_TOLERANCE_M, endpoint_vertices=None):
    """Prefer well-spaced endpoints; use midpoints only after exhausting endpoints."""
    if count < 1:
        raise ValueError("count must be at least 1")
    if not math.isfinite(dedupe_tolerance) or dedupe_tolerance <= 0:
        raise ValueError("dedupe tolerance must be positive and finite")
    # An arc-tessellated display ring may have many artificial vertices. The
    # caller can retain its source segment endpoints as the preferred candidates.
    endpoints = ring if endpoint_vertices is None else endpoint_vertices
    vertices = sorted({(float(p[0]), float(p[1])) for p in endpoints})
    if not vertices:
        raise ValueError("reference ring is empty")
    points = []
    _add_farthest_candidates(vertices, points, count, dedupe_tolerance)
    # Additional slots subdivide actual boundary spans, never snap through the interior.
    boundary = [(float(p[0]), float(p[1])) for p in ring]
    for _ in range(count):
        if len(points) >= count:
            break
        refined = []
        midpoints = []
        for a, b in zip(boundary, boundary[1:] + boundary[:1], strict=True):
            midpoint = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            refined.extend((a, midpoint))
            midpoints.append(midpoint)
        _add_farthest_candidates(sorted(set(midpoints)), points, count, dedupe_tolerance)
        boundary = refined
    return points


def _add_farthest_candidates(candidates, points, count, tolerance):
    """Stable coordinate tie-breaks, with no coincident/near-coincident references."""
    while len(points) < count:
        best = None
        for vertex in candidates:
            distance = min((math.dist(vertex, p) for p in points), default=math.inf)
            if distance < tolerance:
                continue
            if best is None or distance > best[0] + 1e-12:
                best = (distance, vertex)
        if best is None:
            break
        points.append(best[1])


def project_points_z(points_xy, sample_z_xy):
    """One record per point: {id, x, y, z, status} with status in
    "valid" | "outside" | "error". Outside points are kept, never given a fake Z."""
    records = []
    for index, point in enumerate(points_xy):
        x, y = float(point[0]), float(point[1])
        record = {"id": "P%d" % (index + 1), "x": x, "y": y, "z": None, "status": "error"}
        try:
            z = sample_z_xy(x, y)
        except Exception:
            records.append(record)
            continue
        if z is None or not math.isfinite(float(z)):
            record["status"] = "outside"
        else:
            record["z"] = float(z)
            record["status"] = "valid"
        records.append(record)
    return records


def average_elevation(records, minimum_valid=1):
    """Mean/min/max/population-std over the valid records.

    A point that projects outside the terrain is EXCLUDED and counted; the mean is
    never propped up with a substitute value. Fewer than `minimum_valid` valid
    points reports the average as unavailable.
    """
    values = [r["z"] for r in records if r.get("status") == "valid" and r.get("z") is not None]
    result = {
        "average": None,
        "minimum": None,
        "maximum": None,
        "std": None,
        "valid": len(values),
        "excluded": len(records) - len(values),
        "status": "unavailable",
    }
    if len(values) < max(1, int(minimum_valid)):
        return result
    mean = sum(values) / len(values)
    variance = sum((v - mean) ** 2 for v in values) / len(values)
    result.update(
        {
            "average": mean,
            "minimum": min(values),
            "maximum": max(values),
            "std": math.sqrt(variance),
            "status": "ok",
        }
    )
    return result


def nearest_marker(pick_xy, markers, tolerance_m):
    """Index of the marker nearest `pick_xy` within tolerance, else None.

    Deterministic: the lowest index wins a tie. `markers` is a sequence of
    (x, y) or of records with "x"/"y".
    """
    px, py = float(pick_xy[0]), float(pick_xy[1])
    best = None
    for index, marker in enumerate(markers):
        if isinstance(marker, dict):
            mx, my = float(marker["x"]), float(marker["y"])
        else:
            mx, my = float(marker[0]), float(marker[1])
        distance = math.hypot(px - mx, py - my)
        if best is None or distance < best[0] - 1e-12:
            best = (distance, index)
    if best is None or best[0] > tolerance_m + 1e-9:
        return None
    return best[1]


def inside_ring(ring, point_xy):
    """Convenience re-export for callers that classify a picked point."""
    return point_in_ring(point_xy, ring)
