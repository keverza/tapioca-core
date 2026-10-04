"""Shared pure setback geometry for the Allowed Building Envelope workflow.

No tapioca, numpy or Archicad imports: this module is the calculation core, so it
runs under plain pytest and can be reused unchanged by the production command.

Conventions
-----------
- Coordinates are model metres, XY plan. Z is project world Z.
- `footprint` is a vertex chain; `arcs[i]` is the signed included angle (radians)
  of the edge LEAVING vertex i — the `details()` contract — 0 = straight.
- A `SetbackRule` constrains an edge. `NONE` contributes no setback and no
  height. All numeric rule values are parameters; the STR 193 values here are a
  preset (data), not a code path.

The allowed footprint
---------------------
The spec resolves corners by extending adjacent offset lines until they meet
("extend their offset lines and intersect them"). Applying every edge's infinite
half-plane globally is WRONG on a concave parcel: an edge on the far side of a
notch would cut the opposite arm of the site. So the region is classified per
point by its CLOSEST boundary feature:

- the closest point on the boundary lies in the interior of edge e -> the point
  must be at least e's setback from e's line;
- the closest point is a vertex v -> the point must be at least the setback from
  the line of every regulated edge incident to v (the extended-line/miter rule,
  with a `NONE` edge contributing a zero setback, i.e. the original boundary).

For convex parcels ALL regulated half-planes apply, even when the closest edge
is NONE. Closest-feature classification remains an experimental concave
predicate, not proof of a valid closed offset or a mixed-distance offset solver.
The smoke uses `polyhedral.build` for compact convex face output; the grid below
is retained for reference tests only, never as a live fallback.

The height datum for an edge is the terrain elevation at the closest point on
the ORIGINAL edge, sampled along it (`terrain_datum_curve`), not the terrain
under the query point.
"""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass, field, replace

EPS = 1e-9
_ARC_EPS = 1e-6


# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class SetbackRule:
    """One regulatory rule. `run_per_rise` is metres of run per 1 m of rise."""

    id: str
    name: str = ""
    base_setback: float = 0.0
    base_height: float = 0.0
    run_per_rise: float = 0.0
    has_height_slope: bool = False
    requires_terrain_boundary_reference: bool = True
    source: str = ""


# The spec's default preset, as data. `source` is deliberately empty: the
# regulation citation is project input, never invented here.
STR_193 = SetbackRule(
    id="LT_STR_193",
    name="STR 193",
    base_setback=3.0,
    base_height=8.5,
    run_per_rise=0.5,
    has_height_slope=True,
    requires_terrain_boundary_reference=True,
)
NONE = SetbackRule(id="NONE", name="None")


def is_regulated(rule: SetbackRule | None) -> bool:
    """A rule constrains geometry when it sets a height or a positive setback."""
    if rule is None:
        return False
    return rule.base_height > 0.0 or rule.base_setback > 0.0


# ---------------------------------------------------------------------------
# Segments
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Segment:
    id: str
    edge_index: int
    a: tuple
    b: tuple
    arc_angle: float = 0.0
    rule_id: str = "NONE"
    fingerprint: str = ""


def segment_fingerprint(seg: Segment, quantization: float = 1e-4) -> str:
    """Stable hash of the segment's geometry, tolerant of display rounding."""

    def q(value):
        return int(round(float(value) / quantization))

    key = "%d,%d|%d,%d|%d" % (q(seg.a[0]), q(seg.a[1]), q(seg.b[0]), q(seg.b[1]), q(seg.arc_angle))
    return hashlib.sha1(key.encode("ascii")).hexdigest()[:16]


def segments_from_footprint(footprint, arcs=None, rules=None, closed=True):
    """Build `Segment`s from a footprint chain.

    `rules` may be a rule id (all edges), a dict {edge_index: rule_id} (with an
    optional "default" key), or a parallel sequence.
    """
    points = [(float(x), float(y)) for x, y in footprint]
    if len(points) < 2:
        raise ValueError("a footprint needs at least two vertices")
    edge_count = len(points) if closed else len(points) - 1
    arc_list = [float(a) for a in (arcs or [])]
    out = []
    for index in range(edge_count):
        a = points[index]
        b = points[(index + 1) % len(points)]
        arc = arc_list[index] if index < len(arc_list) else 0.0
        if isinstance(rules, str):
            rule_id = rules
        elif isinstance(rules, dict):
            rule_id = str(rules.get(index, rules.get("default", "NONE")))
        elif isinstance(rules, (list, tuple)) and index < len(rules):
            rule_id = str(rules[index])
        else:
            rule_id = "NONE"
        seg = Segment(
            id="S%02d" % (index + 1),
            edge_index=index,
            a=a,
            b=b,
            arc_angle=arc,
            rule_id=rule_id,
        )
        out.append(replace(seg, fingerprint=segment_fingerprint(seg)))
    return out


def remap_assignments(old_segments, new_segments, tolerance_m=0.5):
    """Carry rules from old segments to new ones after the ring changed.

    Order: exact fingerprint -> same endpoint pair -> nearest segment (distance
    from the new midpoint to the old SEGMENT, so a split edge's halves both match
    the segment they came from). Many new segments may map to one old segment
    (a split); a merge picks the nearest and is reported as "nearest", not
    "fingerprint". Returns (assignments, unresolved_ids).
    """
    assignments = []
    for new in new_segments:
        match = None
        confidence = "unresolved"
        for old in old_segments:
            if old.fingerprint and old.fingerprint == new.fingerprint:
                match, confidence = old, "fingerprint"
                break
        if match is None:
            for old in old_segments:
                if _same_endpoints(old, new, tolerance_m):
                    match, confidence = old, "endpoints"
                    break
        if match is None:
            mx = (new.a[0] + new.b[0]) / 2.0
            my = (new.a[1] + new.b[1]) / 2.0
            best = None
            for old in old_segments:
                _, dist, _ = closest_point_on_segment(mx, my, old.a[0], old.a[1], old.b[0], old.b[1])
                if best is None or dist < best[0]:
                    best = (dist, old)
            if best is not None and best[0] <= tolerance_m:
                match, confidence = best[1], "nearest"
        assignments.append(
            {
                "segment_id": new.id,
                "rule_id": match.rule_id if match else "NONE",
                "confidence": confidence,
            }
        )
    unresolved = [a["segment_id"] for a in assignments if a["confidence"] == "unresolved"]
    return assignments, unresolved


def _same_endpoints(old, new, tolerance_m):
    def near(p, q):
        return math.hypot(p[0] - q[0], p[1] - q[1]) <= tolerance_m

    return (near(old.a, new.a) and near(old.b, new.b)) or (near(old.a, new.b) and near(old.b, new.a))


# ---------------------------------------------------------------------------
# Arcs
# ---------------------------------------------------------------------------


def tessellate_arcs(footprint, arcs, tolerance_m=0.05):
    """Replace arced edges with chords whose sagitta is at most `tolerance_m`.

    Returns (points, arcs, notes) with all-zero arcs. The sign convention —
    positive angle sweeps counter-clockwise from A to B — is documented but NOT
    yet live-verified against Archicad's `arcAngle`; the smoke command reports
    the tessellation it performed so the convention can be checked visually.
    """
    points = [(float(x), float(y)) for x, y in footprint]
    arc_list = [float(a) for a in (arcs or [])]
    if not any(abs(a) > _ARC_EPS for a in arc_list):
        return points, [0.0] * len(points), []
    new_points = []
    new_arcs = []
    notes = []
    for index, a in enumerate(points):
        b = points[(index + 1) % len(points)]
        theta = arc_list[index] if index < len(arc_list) else 0.0
        new_points.append(a)
        new_arcs.append(0.0)
        if abs(theta) <= _ARC_EPS:
            continue
        interior = _arc_interior_points(a, b, theta, tolerance_m)
        new_points.extend(interior)
        new_arcs.extend([0.0] * len(interior))
        notes.append("edge %d: %.1f deg arc -> %d chords" % (index, math.degrees(theta), len(interior) + 1))
    return new_points, new_arcs, notes


def _arc_interior_points(a, b, theta, tolerance_m):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    chord = math.hypot(dx, dy)
    abs_theta = abs(theta)
    if chord <= EPS or abs_theta <= _ARC_EPS:
        return []
    if abs_theta >= 2.0 * math.pi - _ARC_EPS:
        raise ValueError("a full-circle edge (%.3f rad) cannot be a ring edge" % theta)
    radius = chord / (2.0 * math.sin(abs_theta / 2.0))
    h = radius * math.cos(abs_theta / 2.0)
    mx, my = (ax + bx) / 2.0, (ay + by) / 2.0
    lx, ly = -dy / chord, dx / chord  # left normal of A -> B
    sign = 1.0 if theta > 0 else -1.0
    cx, cy = mx + sign * lx * h, my + sign * ly * h
    if radius <= tolerance_m:
        step = abs_theta
    else:
        step = 2.0 * math.acos(max(-1.0, min(1.0, 1.0 - tolerance_m / radius)))
    count = max(1, int(math.ceil(abs_theta / step - 1e-9)))
    start = math.atan2(ay - cy, ax - cx)
    points = []
    for k in range(1, count):
        angle = start + theta * (k / float(count))
        points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return points


# ---------------------------------------------------------------------------
# Plan predicates
# ---------------------------------------------------------------------------


def signed_area(ring):
    total = 0.0
    count = len(ring)
    if not count:
        return 0.0
    ox, oy = ring[0]
    for i in range(count):
        x1, y1 = ring[i]
        x2, y2 = ring[(i + 1) % count]
        total += (x1 - ox) * (y2 - oy) - (x2 - ox) * (y1 - oy)
    return 0.5 * total


def orientation(ring):
    """+1 for counter-clockwise, -1 for clockwise."""
    return 1.0 if signed_area(ring) >= 0.0 else -1.0


def is_convex(ring):
    sign = orientation(ring)
    for i, p in enumerate(ring):
        a, b = ring[i - 1], ring[(i + 1) % len(ring)]
        cross = (p[0] - a[0]) * (b[1] - p[1]) - (p[1] - a[1]) * (b[0] - p[0])
        if sign * cross < -EPS:
            return False
    return len(ring) >= 3


def bbox(ring):
    xs = [p[0] for p in ring]
    ys = [p[1] for p in ring]
    return min(xs), min(ys), max(xs), max(ys)


def point_in_ring(point, ring):
    """Ray casting. A point exactly on the boundary is not guaranteed either way."""
    x, y = point
    inside = False
    count = len(ring)
    j = count - 1
    for i in range(count):
        xi, yi = ring[i]
        xj, yj = ring[j]
        if (yi > y) != (yj > y):
            xint = (xj - xi) * (y - yi) / (yj - yi) + xi
            if x < xint:
                inside = not inside
        j = i
    return inside


def point_in_region(point, outer, holes=()):
    if not point_in_ring(point, outer):
        return False
    for hole in holes or ():
        if point_in_ring(point, hole):
            return False
    return True


def closest_point_on_segment(px, py, ax, ay, bx, by):
    """((cx, cy), distance, t in [0, 1])."""
    dx, dy = bx - ax, by - ay
    length_sq = dx * dx + dy * dy
    if length_sq <= EPS:
        return (ax, ay), math.hypot(px - ax, py - ay), 0.0
    t = ((px - ax) * dx + (py - ay) * dy) / length_sq
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * dx, ay + t * dy
    return (cx, cy), math.hypot(px - cx, py - cy), t


def closest_point_on_ring(ring, point, closed=True):
    """((cx, cy), distance) on the ring; ties keep the lowest edge index."""
    px, py = point
    count = len(ring)
    edge_count = count if closed else count - 1
    best = None
    for i in range(edge_count):
        ax, ay = ring[i]
        bx, by = ring[(i + 1) % count]
        foot, dist, _ = closest_point_on_segment(px, py, ax, ay, bx, by)
        if best is None or dist < best[1] - EPS:
            best = (foot, dist)
    if best is None:
        raise ValueError("a ring needs at least two vertices")
    return best


# ---------------------------------------------------------------------------
# Allowed footprint: closest-feature mitered offset
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class EdgeConstraint:
    edge_index: int
    ax: float
    ay: float
    nx: float
    ny: float
    setback: float

    def line_distance(self, x, y):
        """Distance inward of the edge line (positive on the parcel side)."""
        return (x - self.ax) * self.nx + (y - self.ay) * self.ny


def inward_normal(ring, seg):
    """Unit normal pointing into the parcel for one segment."""
    dx, dy = seg.b[0] - seg.a[0], seg.b[1] - seg.a[1]
    length = math.hypot(dx, dy)
    if length <= EPS:
        return None
    nx, ny = -dy / length, dx / length
    if orientation(ring) < 0:
        nx, ny = -nx, -ny
    return (nx, ny)


def edge_constraints(ring, segments, rules):
    """One line constraint per regulated edge, normal pointing into the parcel."""
    out = []
    for seg in segments:
        rule = rules.get(seg.rule_id)
        if not is_regulated(rule) or rule.base_setback <= 0.0:
            continue
        normal = inward_normal(ring, seg)
        if normal is None:
            continue
        out.append(
            EdgeConstraint(
                edge_index=seg.edge_index,
                ax=seg.a[0],
                ay=seg.a[1],
                nx=normal[0],
                ny=normal[1],
                setback=rule.base_setback,
            )
        )
    return out


def closest_feature(x, y, segments):
    """(distance, edge_index, t) of the nearest boundary point on any segment."""
    best = None
    for seg in segments:
        _, dist, t = closest_point_on_segment(x, y, seg.a[0], seg.a[1], seg.b[0], seg.b[1])
        if best is None or dist < best[0] - EPS:
            best = (dist, seg.edge_index, t)
    return best


def allowed(point, outer, holes, segments, rules, constraints=None):
    """Is `point` inside the parcel and at least every regulated setback in?

    The mitered offset: the constraint(s) applied are those of the closest
    boundary feature (edge interior or vertex), which keeps concave parcels from
    being over-cut by an unrelated edge's line.
    """
    if not point_in_region(point, outer, holes):
        return False
    if constraints is None:
        constraints = edge_constraints(outer, segments, rules)
    if not constraints:
        return True
    x, y = point
    if is_convex(outer):
        return all(c.line_distance(x, y) >= c.setback - EPS for c in constraints)
    feature = closest_feature(x, y, segments)
    if feature is None:
        return True
    _, edge_index, t = feature
    by_edge = {c.edge_index: c for c in constraints}
    count = len(outer)
    if t <= EPS or t >= 1.0 - EPS:
        vertex = edge_index if t <= EPS else (edge_index + 1) % count
        incident = ((vertex - 1) % count, vertex)
        for incident_edge in incident:
            constraint = by_edge.get(incident_edge)
            if constraint is not None and constraint.line_distance(x, y) < constraint.setback - 1e-9:
                return False
        return True
    constraint = by_edge.get(edge_index)
    if constraint is None:
        return True
    return constraint.line_distance(x, y) >= constraint.setback - 1e-9


def _boundary_distance(x, y, outer, holes):
    """Distance to the parcel boundary (outer and holes), clamped to segments."""
    best = None
    for ring in [outer] + [hole for hole in (holes or ())]:
        count = len(ring)
        if count < 2:
            continue
        for i in range(count):
            ax, ay = ring[i]
            bx, by = ring[(i + 1) % count]
            _, dist, _ = closest_point_on_segment(x, y, ax, ay, bx, by)
            if best is None or dist < best:
                best = dist
    return 0.0 if best is None else best


def clearance_margin(x, y, outer, holes, segments, rules, constraints=None):
    """Signed clearance to the allowed-region boundary.

    Positive inside the allowed footprint, negative outside, zero on the boundary.
    This is the scalar field the envelope's boundary is extracted from, so it uses
    the same closest-feature rule as `allowed`: a point constrained by an edge
    interior gets that edge's margin, a point at a vertex gets the minimum of its
    incident edges' margins, and the parcel boundary caps everything so NONE edges
    still end at the original boundary.
    """
    inside = point_in_region((x, y), outer, holes)
    boundary = _boundary_distance(x, y, outer, holes)
    margin = boundary if inside else -boundary
    if not inside:
        return margin
    if constraints is None:
        constraints = edge_constraints(outer, segments, rules)
    if not constraints:
        return margin
    if is_convex(outer):
        return min([margin] + [c.line_distance(x, y) - c.setback for c in constraints])
    feature = closest_feature(x, y, segments)
    if feature is None:
        return margin
    _, edge_index, t = feature
    by_edge = {c.edge_index: c for c in constraints}
    count = len(outer)
    if t <= EPS or t >= 1.0 - EPS:
        vertex = edge_index if t <= EPS else (edge_index + 1) % count
        for incident_edge in ((vertex - 1) % count, vertex):
            constraint = by_edge.get(incident_edge)
            if constraint is None:
                continue
            value = constraint.line_distance(x, y) - constraint.setback
            if value < margin:
                margin = value
    else:
        constraint = by_edge.get(edge_index)
        if constraint is not None:
            value = constraint.line_distance(x, y) - constraint.setback
            if value < margin:
                margin = value
    return margin


def offset_line(seg, normal, distance, extend_m):
    """A display line parallel to `seg`, offset by `distance` along `normal`."""
    ax = seg.a[0] + normal[0] * distance
    ay = seg.a[1] + normal[1] * distance
    bx = seg.b[0] + normal[0] * distance
    by = seg.b[1] + normal[1] * distance
    dx, dy = seg.b[0] - seg.a[0], seg.b[1] - seg.a[1]
    length = math.hypot(dx, dy)
    if length <= EPS:
        return None
    ux, uy = dx / length, dy / length
    return [
        (ax - ux * extend_m, ay - uy * extend_m),
        (bx + ux * extend_m, by + uy * extend_m),
    ]


# ---------------------------------------------------------------------------
# Terrain datum curves and the height envelope
# ---------------------------------------------------------------------------


def terrain_datum_curve(seg, sample_z_xy, spacing_m=1.0):
    """Terrain Z sampled along the segment, as [(x, y, z), ...] with valid z only.

    `sample_z_xy(x, y)` returns a float or None (outside the terrain). Invalid
    samples are dropped; `polyline_z_at` then interpolates between the valid ones.
    """
    length = math.hypot(seg.b[0] - seg.a[0], seg.b[1] - seg.a[1])
    count = max(2, int(math.ceil(length / max(spacing_m, 1e-6))) + 1)
    samples = []
    for k in range(count):
        t = k / (count - 1.0)
        x = seg.a[0] + (seg.b[0] - seg.a[0]) * t
        y = seg.a[1] + (seg.b[1] - seg.a[1]) * t
        z = sample_z_xy(x, y)
        if z is not None and math.isfinite(float(z)):
            samples.append((x, y, float(z)))
    return samples


def polyline_z_at(points, q):
    """Z interpolated at the closest point on the XY polyline `points`."""
    if not points:
        return None
    if len(points) == 1:
        return points[0][2]
    best = None
    for i in range(len(points) - 1):
        ax, ay, az = points[i]
        bx, by, bz = points[i + 1]
        _, dist, t = closest_point_on_segment(q[0], q[1], ax, ay, bx, by)
        if best is None or dist < best[0] - EPS:
            best = (dist, az + (bz - az) * t)
    return None if best is None else best[1]


@dataclass(frozen=True)
class Constraint:
    """What controls the allowed height at one point."""

    z: float
    segment_id: str
    rule_id: str
    d: float
    z_ref: float
    clipped: bool = False


def z_constraint(x, y, segments, rules, datum_curves, project_max_height=0.0):
    """The lowest regulatory surface at (x, y), clipped by the project maximum.

    Returns None when no regulated edge has a usable terrain datum at the point.
    """
    constraints = []
    for seg in segments:
        rule = rules.get(seg.rule_id)
        if not is_regulated(rule):
            continue
        q, d, _ = closest_point_on_segment(x, y, seg.a[0], seg.a[1], seg.b[0], seg.b[1])
        if d < rule.base_setback - 1e-9:
            return None  # outside the allowed footprint for this edge
        z_ref = polyline_z_at(datum_curves.get(seg.id), q)
        if z_ref is None:
            continue
        if rule.has_height_slope and rule.run_per_rise > 0.0:
            z = z_ref + rule.base_height + (d - rule.base_setback) / rule.run_per_rise
        else:
            z = z_ref + rule.base_height
        constraints.append(Constraint(z=z, segment_id=seg.id, rule_id=rule.id, d=d, z_ref=z_ref))
    if not constraints:
        return None
    controlling = min(constraints, key=lambda c: (c.z, c.segment_id))
    z_final = controlling.z
    clipped = False
    if project_max_height and project_max_height > 0.0 and z_final > project_max_height:
        z_final = project_max_height
        clipped = True
    return Constraint(
        z=z_final,
        segment_id=controlling.segment_id,
        rule_id=controlling.rule_id,
        d=controlling.d,
        z_ref=controlling.z_ref,
        clipped=clipped,
    )


@dataclass
class Envelope:
    vertices: list
    triangles: list
    constraints: list
    bounds: tuple
    grid: dict
    notes: list = field(default_factory=list)
    base_vertices: list = field(default_factory=list)
    base_triangles: list = field(default_factory=list)
    solid_vertices: list = field(default_factory=list)
    solid_triangles: list = field(default_factory=list)
    wall_quads: int = 0
    outer_ring: list = field(default_factory=list)
    faces: list = field(default_factory=list)


def build_envelope(
    outer,
    holes,
    segments,
    rules,
    datum_curves,
    project_max_height=0.0,
    resolution=2.0,
    max_cells=250000,
    base_z_at=None,
):
    """Build the envelope as a closed solid: top + terrain base + vertical walls.

    The top is the lower regulatory surface. The base is the terrain under the
    allowed footprint (or a copy of the top when `base_z_at` is None). The walls
    rise where the allowed region ends, so the object reads as the buildable
    volume sitting on the ground rather than a floating sheet — the "8.5 m wall"
    the spec's diagram shows.

    The region boundary is cut at the zero level of `clearance_margin` with linear
    interpolation, and the top, base and walls share those boundary vertices, so
    there are no seams and no staircase. Grids over `max_cells` are refused, not
    truncated.
    """
    if resolution <= 0.0:
        raise ValueError("resolution must be positive")
    constraints = edge_constraints(outer, segments, rules)
    min_x, min_y, max_x, max_y = bbox(outer)
    nx = int(math.ceil((max_x - min_x) / resolution)) + 1
    ny = int(math.ceil((max_y - min_y) / resolution)) + 1
    if nx * ny > max_cells:
        raise ValueError(
            "envelope grid %dx%d = %d cells exceeds %d; raise the resolution" % (nx, ny, nx * ny, max_cells)
        )
    xs = [min(max_x, min_x + i * resolution) for i in range(nx)]
    ys = [min(max_y, min_y + j * resolution) for j in range(ny)]

    margin = [[0.0] * nx for _ in range(ny)]
    kept = [[False] * nx for _ in range(ny)]
    constraint_grid = [[None] * nx for _ in range(ny)]
    considered = 0
    for j in range(ny):
        for i in range(nx):
            considered += 1
            value = clearance_margin(xs[i], ys[j], outer, holes, segments, rules, constraints)
            if value < -1e-9:
                margin[j][i] = value
                continue
            constraint = z_constraint(xs[i], ys[j], segments, rules, datum_curves, project_max_height)
            if constraint is None:
                # Allowed but no terrain datum: end the region here rather than
                # inventing a height.
                margin[j][i] = min(value, 0.0)
                continue
            margin[j][i] = value
            kept[j][i] = True
            constraint_grid[j][i] = constraint

    vertices = []
    vertex_constraints = []
    top_index = {}
    for j in range(ny):
        for i in range(nx):
            if not kept[j][i]:
                continue
            top_index[(i, j)] = len(vertices)
            vertices.append((xs[i], ys[j], constraint_grid[j][i].z))
            vertex_constraints.append(constraint_grid[j][i])

    crossing_index = {}

    def crossing_vertex(i1, j1, i2, j2):
        key = ((min(i1, i2), min(j1, j2)), (max(i1, i2), max(j1, j2)))
        if key in crossing_index:
            return crossing_index[key]
        m1, m2 = margin[j1][i1], margin[j2][i2]
        denominator = m1 - m2
        t = 0.0 if abs(denominator) < 1e-12 else m1 / denominator
        t = max(0.0, min(1.0, t))
        x = xs[i1] + (xs[i2] - xs[i1]) * t
        y = ys[j1] + (ys[j2] - ys[j1]) * t
        constraint = z_constraint(x, y, segments, rules, datum_curves, project_max_height)
        if constraint is None:
            z1 = constraint_grid[j1][i1].z if kept[j1][i1] else None
            z2 = constraint_grid[j2][i2].z if kept[j2][i2] else None
            z = z1 if z1 is not None else (z2 if z2 is not None else 0.0)
        else:
            z = constraint.z
        index = len(vertices)
        vertices.append((x, y, z))
        vertex_constraints.append(constraint)
        crossing_index[key] = index
        return index

    corner_order = ((0, 0), (1, 0), (1, 1), (0, 1))
    triangles = []
    wall_pairs = []
    for j in range(ny - 1):
        for i in range(nx - 1):
            flags = [kept[j + dj][i + di] for di, dj in corner_order]
            if not any(flags):
                continue
            if all(flags):
                a, b, c, d = [top_index[(i + di, j + dj)] for di, dj in corner_order]
                triangles.append((a, b, c))
                triangles.append((a, c, d))
                continue
            poly = []
            edge_crossing = {}
            for k in range(4):
                di, dj = corner_order[k]
                if flags[k]:
                    poly.append(top_index[(i + di, j + dj)])
                di2, dj2 = corner_order[(k + 1) % 4]
                if flags[k] != flags[(k + 1) % 4]:
                    crossing = crossing_vertex(i + di, j + dj, i + di2, j + dj2)
                    edge_crossing[k] = crossing
                    poly.append(crossing)
            if len(poly) >= 3:
                for k in range(1, len(poly) - 1):
                    triangles.append((poly[0], poly[k], poly[k + 1]))
            if not edge_crossing:
                continue
            inside_xy = None
            for k in range(4):
                di, dj = corner_order[k]
                if flags[k]:
                    inside_xy = (xs[i + di], ys[j + dj])
                    break
            pairs = []
            if len(edge_crossing) == 2:
                keys = sorted(edge_crossing)
                pairs.append((edge_crossing[keys[0]], edge_crossing[keys[1]]))
            elif len(edge_crossing) == 4:
                center = sum(margin[j + dj][i + di] for di, dj in corner_order) / 4.0
                if center >= 0.0:
                    for k in range(4):
                        if not flags[k] and k in edge_crossing and (k - 1) % 4 in edge_crossing:
                            pairs.append((edge_crossing[k], edge_crossing[(k - 1) % 4]))
                else:
                    for k in range(4):
                        if flags[k] and k in edge_crossing and (k - 1) % 4 in edge_crossing:
                            pairs.append((edge_crossing[(k - 1) % 4], edge_crossing[k]))
            for a, b in pairs:
                ax, ay = vertices[a][0], vertices[a][1]
                bx, by = vertices[b][0], vertices[b][1]
                cross = (bx - ax) * (inside_xy[1] - ay) - (by - ay) * (inside_xy[0] - ax)
                wall_pairs.append((a, b) if cross >= 0.0 else (b, a))

    count = len(vertices)
    base_vertices = []
    for x, y, z in vertices:
        if base_z_at is None:
            base_vertices.append((x, y, z))
        else:
            base = base_z_at(x, y)
            base_vertices.append((x, y, z if base is None else float(base)))
    base_triangles = [(count + i, count + k, count + j) for (i, j, k) in triangles]
    solid_vertices = list(vertices) + list(base_vertices)
    solid_triangles = list(triangles) + list(base_triangles)
    for a, b in wall_pairs:
        base_a, top_a, base_b, top_b = count + a, a, count + b, b
        solid_triangles.append((base_a, base_b, top_b))
        solid_triangles.append((base_a, top_b, top_a))

    if vertices:
        vx = [v[0] for v in vertices]
        vy = [v[1] for v in vertices]
        vz = [v[2] for v in vertices]
        bounds = (min(vx), min(vy), min(vz), max(vx), max(vy), max(vz))
    else:
        bounds = (min_x, min_y, 0.0, max_x, max_y, 0.0)

    notes = [
        "resolution %.2f m: %d of %d grid points allowed, %d top triangles, "
        "%d wall quads, %d solid triangles"
        % (
            resolution,
            len(top_index),
            considered,
            len(triangles),
            len(wall_pairs),
            len(solid_triangles),
        )
    ]
    grid = {
        "x0": min_x,
        "y0": min_y,
        "nx": nx,
        "ny": ny,
        "resolution": resolution,
        "kept": len(top_index),
        "considered": considered,
    }
    return Envelope(
        vertices=vertices,
        triangles=triangles,
        constraints=vertex_constraints,
        bounds=bounds,
        grid=grid,
        notes=notes,
        base_vertices=base_vertices,
        base_triangles=base_triangles,
        solid_vertices=solid_vertices,
        solid_triangles=solid_triangles,
        wall_quads=len(wall_pairs),
    )


# ---------------------------------------------------------------------------
# Offset-line and terrain-intersection helpers (no repeated-ring solid builder)
# ---------------------------------------------------------------------------


def _line_intersection(p, direction, q, other_direction):
    """Intersection of two infinite lines; None when near-parallel."""
    denominator = direction[0] * other_direction[1] - direction[1] * other_direction[0]
    if abs(denominator) < 1e-12:
        return None
    t = ((q[0] - p[0]) * other_direction[1] - (q[1] - p[1]) * other_direction[0]) / denominator
    return (p[0] + direction[0] * t, p[1] + direction[1] * t)


def miter_ring(ring, segments, rules, extra=0.0):
    """The exact mitered offset contour: one vertex per parcel vertex.

    Each edge's offset line is shifted inward by its rule's setback plus `extra`
    (`NONE` = the original line). Vertex i is the intersection of the offset lines
    of edges i-1 and i — the spec's "extend their offset lines and intersect
    them". Near-parallel neighbours fall back to the shared vertex shifted along
    the average inward normal (tessellated arcs are the common case).

    Returns (points, notes) or (None, notes) when the contour cannot be built.
    """
    count = len(ring)
    if count < 3:
        return None, ["a ring needs at least three vertices"]
    by_index = {seg.edge_index: seg for seg in segments}
    normals = {}
    distances = {}
    lines = {}
    for i in range(count):
        seg = by_index.get(i)
        if seg is None:
            return None, ["segment %d is missing" % i]
        rule = rules.get(seg.rule_id)
        setback = 0.0 if not is_regulated(rule) else max(0.0, rule.base_setback)
        normal = inward_normal(ring, seg)
        if normal is None:
            return None, ["segment %d is degenerate" % i]
        normals[i] = normal
        distances[i] = setback + (max(0.0, extra) if is_regulated(rule) else 0.0)
        lines[i] = (
            (seg.a[0] + normal[0] * distances[i], seg.a[1] + normal[1] * distances[i]),
            (seg.b[0] - seg.a[0], seg.b[1] - seg.a[1]),
        )
    points = []
    notes = []
    for i in range(count):
        previous = (i - 1) % count
        p, direction = lines[previous]
        q, other = lines[i]
        point = _line_intersection(p, direction, q, other)
        if point is None:
            nx = normals[previous][0] + normals[i][0]
            ny = normals[previous][1] + normals[i][1]
            length = math.hypot(nx, ny)
            if length < 1e-9:
                return None, ["edges are parallel at vertex %d" % i]
            distance = max(distances[previous], distances[i])
            points.append((ring[i][0] + nx / length * distance, ring[i][1] + ny / length * distance))
            notes.append("vertex %d: near-parallel edges, normal offset used" % i)
        else:
            points.append(point)
    return points, notes


def _segments_intersect(a, b, c, d):
    """Proper (non-touching) 2D segment intersection."""

    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])

    d1 = cross(c, d, a)
    d2 = cross(c, d, b)
    d3 = cross(a, b, c)
    d4 = cross(a, b, d)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0))


def ring_self_intersects(points):
    count = len(points)
    for i in range(count):
        a, b = points[i], points[(i + 1) % count]
        for j in range(i + 2, count):
            if (j + 1) % count == i:
                continue
            c, d = points[j], points[(j + 1) % count]
            if _segments_intersect(a, b, c, d):
                return True
    return False


def _segment_intersection_param(a, b, c, d):
    """Parameter t on segment A->B where it crosses C->D, or None."""
    rx = b[0] - a[0]
    ry = b[1] - a[1]
    sx = d[0] - c[0]
    sy = d[1] - c[1]
    denominator = rx * sy - ry * sx
    if abs(denominator) < 1e-12:
        return None
    qx = c[0] - a[0]
    qy = c[1] - a[1]
    t = (qx * sy - qy * sx) / denominator
    u = (qx * ry - qy * rx) / denominator
    if -1e-9 <= t <= 1.0 + 1e-9 and -1e-9 <= u <= 1.0 + 1e-9:
        return max(0.0, min(1.0, t))
    return None


def terrain_split_params(points, edges, closed=True):
    """Per-segment parameters where a polyline crosses terrain edges (XY).

    `edges` is a sequence of ((x1, y1), (x2, y2)) terrain triangle edges. The
    returned parameters are what densifies the contour so its base follows the
    mesh ridges exactly instead of cutting chords through them.
    """
    count = len(points)
    segment_count = count if closed else count - 1
    params = []
    for i in range(segment_count):
        a = points[i]
        b = points[(i + 1) % count]
        found = []
        min_x, max_x = (a[0], b[0]) if a[0] <= b[0] else (b[0], a[0])
        min_y, max_y = (a[1], b[1]) if a[1] <= b[1] else (b[1], a[1])
        for c, d in edges:
            if max(c[0], d[0]) < min_x - 1e-9 or min(c[0], d[0]) > max_x + 1e-9:
                continue
            if max(c[1], d[1]) < min_y - 1e-9 or min(c[1], d[1]) > max_y + 1e-9:
                continue
            t = _segment_intersection_param(a, b, c, d)
            if t is not None and 1e-9 < t < 1.0 - 1e-9:
                found.append(t)
        found.sort()
        deduped = []
        for t in found:
            if not deduped or t - deduped[-1] > 1e-9:
                deduped.append(t)
        params.append(deduped)
    return params


def densify_ring(points, params):
    """Insert the same per-segment parameters into a ring (keeps 1:1 pairing)."""
    out = []
    count = len(points)
    for i in range(count):
        a = points[i]
        b = points[(i + 1) % count]
        out.append(a)
        for t in params[i] if i < len(params) else []:
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    return out
