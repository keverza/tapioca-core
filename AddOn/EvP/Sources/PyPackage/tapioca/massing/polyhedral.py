"""Compact planar envelope faces; triangles are only the final display adapter.

For a convex parcel, intersect the setback half-planes once. Sweep each original
edge's piecewise-linear terrain datum inward at the rule slope and intersect the
resulting height planes analytically. No grid, repeated offset rings or terrain
bottom. Coplanar roof cells merge; each footprint side owns one vertical face.

Simple concave parcels use their mitered inset, decomposed into convex cells.
Height distance is local to each finite edge, not its infinite supporting line;
endpoint cones use a conservative 32-sided planar approximation (<=0.49% distance
error). Split/self-touching insets and holes still require a boolean offset solver.
"""

from __future__ import annotations

import math
from collections import Counter, defaultdict
from dataclasses import dataclass, replace

from . import setbackgeom as sg

_TOL = 1e-7


@dataclass(frozen=True)
class Face:
    indices: tuple
    kind: str
    source: str = ""


@dataclass
class _Cell:
    polygon: list
    plane: tuple | None
    source: str


def _cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _clean(points, collinear=False):
    out = []
    for p in points:
        if not out or math.dist(p, out[-1]) > _TOL:
            out.append(tuple(p))
    if len(out) > 1 and math.dist(out[0], out[-1]) <= _TOL:
        out.pop()
    if collinear:
        changed = True
        while changed and len(out) > 3:
            changed = False
            for i, p in enumerate(out):
                a, b = out[i - 1], out[(i + 1) % len(out)]
                if abs(_cross(a, p, b)) <= _TOL * max(1.0, math.dist(a, b)):
                    out.pop(i)
                    changed = True
                    break
    return out


def _clip(polygon, plane):
    """Convex XY polygon on the >=0 side of an affine line."""
    if not polygon:
        return []
    a, b, c = plane
    out = []
    previous = polygon[-1]
    before = a * previous[0] + b * previous[1] + c
    for current in polygon:
        after = a * current[0] + b * current[1] + c
        if (before >= 0.0) != (after >= 0.0):
            t = before / (before - after)
            out.append(tuple(previous[k] + t * (current[k] - previous[k]) for k in (0, 1)))
        if after >= 0.0:
            out.append(current)
        previous, before = current, after
    out = _clean(out)
    return out if len(out) >= 3 and abs(sg.signed_area(out)) > _TOL**2 else []


def _height(plane, point):
    return plane[0] * point[0] + plane[1] * point[1] + plane[2]


def _profile(seg, points):
    """Remove only redundant collinear datum samples, not real terrain kinks."""
    length = math.dist(seg.a, seg.b)
    if length <= _TOL:
        raise ValueError("%s is a degenerate parcel edge" % seg.id)
    if not points or math.dist(points[0][:2], seg.a) > _TOL or math.dist(points[-1][:2], seg.b) > _TOL:
        raise ValueError("%s lacks complete boundary terrain coverage" % seg.id)
    ux, uy = (seg.b[0] - seg.a[0]) / length, (seg.b[1] - seg.a[1]) / length
    out = []
    for x, y, z in points:
        s = (x - seg.a[0]) * ux + (y - seg.a[1]) * uy
        if not all(math.isfinite(v) for v in (s, z)):
            raise ValueError("%s has a non-finite terrain datum" % seg.id)
        out.append((s, float(z)))
        while len(out) >= 3:
            a, p, b = out[-3:]
            if b[0] - a[0] <= _TOL:
                raise ValueError("%s has a non-increasing terrain profile" % seg.id)
            interpolated = a[1] + (b[1] - a[1]) * (p[0] - a[0]) / (b[0] - a[0])
            if abs(p[1] - interpolated) > _TOL:
                break
            out.pop(-2)
    if len(out) < 2 or any(b[0] - a[0] <= _TOL for a, b in zip(out, out[1:], strict=False)):
        raise ValueError("%s has an invalid terrain profile" % seg.id)
    return (ux, uy), out


def _edge_planes(seg, rule, normal, direction, profile, origin):
    """(domain half-planes, height plane); endpoint datums clamp, not extrapolate."""
    ux, uy = direction
    ax, ay = seg.a[0] - origin[0], seg.a[1] - origin[1]
    slope = 1.0 / rule.run_per_rise if rule.has_height_slope and rule.run_per_rise > 0 else 0.0
    pieces = [(-math.inf, profile[0][0], profile[0][1], 0.0)]
    for (s, z), (end_s, end_z) in zip(profile, profile[1:], strict=False):
        gradient = (end_z - z) / (end_s - s)
        pieces.append((s, end_s, z - gradient * s, gradient))
    pieces.append((profile[-1][0], math.inf, profile[-1][1], 0.0))
    merged = []
    for low, high, datum, gradient in pieces:
        if merged and abs(merged[-1][2] - datum) < _TOL and abs(merged[-1][3] - gradient) < _TOL:
            merged[-1] = (merged[-1][0], high, datum, gradient)
        else:
            merged.append((low, high, datum, gradient))
    for low, high, datum, gradient in merged:
        a, b = gradient * ux + slope * normal[0], gradient * uy + slope * normal[1]
        plane = (a, b, datum + rule.base_height - slope * rule.base_setback - a * ax - b * ay)
        domain = []
        if math.isfinite(low):
            domain.append((ux, uy, -ux * ax - uy * ay - low))
        if math.isfinite(high):
            domain.append((-ux, -uy, ux * ax + uy * ay + high))
        yield domain, plane


def _apply(cells, domain, plane, source):
    result = []
    for cell in cells:
        cuts = list(domain)
        if cell.plane is not None:
            difference = tuple(old - new for old, new in zip(cell.plane, plane, strict=True))
            cuts.append(difference)
        controlled = cell.polygon
        for cut in cuts:
            controlled = _clip(controlled, cut)
        if not controlled or (
            cell.plane is not None and max(_height(cell.plane, p) - _height(plane, p) for p in controlled) <= _TOL
        ):
            result.append(cell)
            continue
        remainder = cell.polygon
        for cut in cuts:
            outside = _clip(remainder, tuple(-value for value in cut))
            if outside:
                result.append(_Cell(outside, cell.plane, cell.source))
            remainder = _clip(remainder, cut)
            if not remainder:
                break
        if remainder:
            result.append(_Cell(remainder, plane, source))
    return result


_ENDPOINT_SIDES = 32


def _local_edge_planes(seg, rule, normal, direction, profile, origin):
    """Continuous finite-edge distance: side strips and polygonal endpoint cones.

    A concave parcel may lie on either side of an unrelated edge's line. Clamping
    only its terrain datum while extrapolating signed distance would overcut it.
    The cone's inscribed support directions underestimate Euclidean distance,
    hence never allow a height above the corresponding exact radial limit.
    """
    ux, uy = direction
    ax, ay = seg.a[0] - origin[0], seg.a[1] - origin[1]
    length = profile[-1][0]
    start = (ux, uy, -ux * ax - uy * ay)
    end = (-ux, -uy, ux * ax + uy * ay + length)
    for sign in (1, -1):
        side = (sign * normal[0], sign * normal[1])
        for domain, plane in _edge_planes(seg, rule, side, direction, profile, origin):
            yield domain + [start, end, (*side, -side[0] * ax - side[1] * ay)], plane
    slope = 1.0 / rule.run_per_rise if rule.has_height_slope and rule.run_per_rise > 0 else 0.0
    directions = [
        (
            math.cos(i * math.tau / _ENDPOINT_SIDES) * ux + math.sin(i * math.tau / _ENDPOINT_SIDES) * normal[0],
            math.cos(i * math.tau / _ENDPOINT_SIDES) * uy + math.sin(i * math.tau / _ENDPOINT_SIDES) * normal[1],
        )
        for i in range(_ENDPOINT_SIDES)
    ]
    for (x, y), z, bound in (
        (seg.a, profile[0][1], tuple(-v for v in start)),
        (seg.b, profile[-1][1], tuple(-v for v in end)),
    ):
        x, y = x - origin[0], y - origin[1]
        for i, (dx, dy) in enumerate(directions):
            domain = [bound]
            for other in (directions[i - 1], directions[(i + 1) % _ENDPOINT_SIDES]):
                a, b = dx - other[0], dy - other[1]
                domain.append((a, b, -a * x - b * y))
            yield domain, (slope * dx, slope * dy, z + rule.base_height - slope * (rule.base_setback + dx * x + dy * y))


def _local_distance(seg, normal, point):
    q, distance, t = sg.closest_point_on_segment(*point, *seg.a, *seg.b)
    if 0 < t < 1:
        return distance, q
    length = math.dist(seg.a, seg.b)
    ux, uy = (seg.b[0] - seg.a[0]) / length, (seg.b[1] - seg.a[1]) / length
    along = (point[0] - q[0]) * ux + (point[1] - q[1]) * uy
    across = (point[0] - q[0]) * normal[0] + (point[1] - q[1]) * normal[1]
    return max(
        math.cos(i * math.tau / _ENDPOINT_SIDES) * along + math.sin(i * math.tau / _ENDPOINT_SIDES) * across
        for i in range(_ENDPOINT_SIDES)
    ), q


def _key(point):
    return tuple(round(value / _TOL) for value in point[:2])


def _split_edges(polygon, candidates):
    out = []
    for a, b in zip(polygon, polygon[1:] + polygon[:1], strict=True):
        length = math.dist(a, b)
        split = []
        for p in candidates:
            _, distance, t = sg.closest_point_on_segment(*p, *a, *b)
            if distance <= _TOL and _TOL / length < t < 1.0 - _TOL / length:
                split.append((t, p))
        out.append(a)
        out.extend(p for _, p in sorted(split))
    return _clean(out)


def _loops(polygons):
    edges = Counter()
    positions = {}
    for polygon in polygons:
        for a, b in zip(polygon, polygon[1:] + polygon[:1], strict=True):
            ka, kb = _key(a), _key(b)
            positions[ka], positions[kb] = a, b
            if edges[(kb, ka)]:
                edges[(kb, ka)] -= 1
            else:
                edges[(ka, kb)] += 1
    outgoing = defaultdict(list)
    for (a, b), count in edges.items():
        if count:
            outgoing[a].extend([b] * count)
    if any(len(targets) != 1 for targets in outgoing.values()):
        return None
    loops = []
    while outgoing:
        start = next(iter(outgoing))
        loop, current = [], start
        while True:
            loop.append(positions[current])
            targets = outgoing.pop(current, None)
            if targets is None:
                return None
            current = targets[0]
            if current == start:
                break
        loops.append(loop)
    return loops


def _merge_roof(cells):
    candidates = list({_key(p): p for cell in cells for p in cell.polygon}.values())
    groups = defaultdict(list)
    for cell in cells:
        groups[(cell.plane, cell.source)].append(_split_edges(cell.polygon, candidates))
    roof = []
    for (plane, source), polygons in groups.items():
        loops = _loops(polygons)
        # A cell union with holes or touching islands is left as convex planar
        # faces. Never fill a hole with a centroid fan.
        if loops is None or any(sg.signed_area(loop) <= 0 for loop in loops):
            loops = polygons
        roof.extend(_Cell(_clean(loop, collinear=True), plane, source) for loop in loops)
    candidates = list({_key(p): p for cell in roof for p in cell.polygon}.values())
    for cell in roof:
        cell.polygon = _split_edges(cell.polygon, candidates)
    return roof


def face_normal(vertices, indices):
    origin = vertices[indices[0]]
    normal = [0.0, 0.0, 0.0]
    for i, index in enumerate(indices):
        p, q = vertices[index], vertices[indices[(i + 1) % len(indices)]]
        a, b = [p[k] - origin[k] for k in range(3)], [q[k] - origin[k] for k in range(3)]
        normal[0] += a[1] * b[2] - a[2] * b[1]
        normal[1] += a[2] * b[0] - a[0] * b[2]
        normal[2] += a[0] * b[1] - a[1] * b[0]
    length = math.sqrt(sum(value * value for value in normal))
    if length <= _TOL**2:
        raise ValueError("degenerate envelope face")
    return tuple(value / length for value in normal)


def triangulate_face(vertices, face):
    """Minimal ear clipping; keep edge vertices so neighbouring faces share seams."""
    normal = face_normal(vertices, face.indices)
    drop = max(range(3), key=lambda k: abs(normal[k]))
    axes = [k for k in range(3) if k != drop]
    polygon = [tuple(vertices[i][k] for k in axes) for i in face.indices]
    sign = sg.orientation(polygon)
    remaining = list(range(len(polygon)))
    triangles = []
    while len(remaining) > 3:
        for position, current in enumerate(remaining):
            previous, following = remaining[position - 1], remaining[(position + 1) % len(remaining)]
            a, b, c = polygon[previous], polygon[current], polygon[following]
            if sign * _cross(a, b, c) <= _TOL**2:
                continue
            if any(
                min(sign * _cross(a, b, polygon[i]), sign * _cross(b, c, polygon[i]), sign * _cross(c, a, polygon[i]))
                >= -(_TOL**2)
                for i in remaining
                if i not in (previous, current, following)
            ):
                continue
            triangles.append(tuple(face.indices[i] for i in (previous, current, following)))
            remaining.pop(position)
            break
        else:
            raise ValueError("cannot triangulate an envelope face without losing boundary vertices")
    if abs(_cross(*(polygon[i] for i in remaining))) <= _TOL**2:
        raise ValueError("degenerate envelope face cap")
    triangles.append(tuple(face.indices[i] for i in remaining))
    return triangles


def _footprint(outer, holes, segments, rules):
    if holes:
        raise ValueError("compact envelope does not yet support parcel holes; no dense fallback")
    if len(outer) < 3 or any(not math.isfinite(v) for p in outer for v in p):
        raise ValueError("invalid parcel ring")
    origin = outer[0]
    footprint = [(x - origin[0], y - origin[1]) for x, y in outer]
    if sg.orientation(footprint) < 0:
        footprint.reverse()
    footprint = _clean(footprint, collinear=True)
    if sg.ring_self_intersects(footprint) or sg.signed_area(footprint) <= _TOL**2:
        raise ValueError("compact envelope needs a simple non-degenerate parcel")
    concave = not sg.is_convex(footprint)
    normals = {seg.id: sg.inward_normal(outer, seg) for seg in segments}
    regulated = [seg for seg in segments if sg.is_regulated(rules.get(seg.rule_id))]
    if concave:
        inset, notes = sg.miter_ring(outer, segments, rules)
        if inset is None:
            raise ValueError("cannot build parcel inset: " + "; ".join(notes))
        # An inverted edge is a collapsed corridor, not a new allowed island.
        for seg in segments:
            a, b = inset[seg.edge_index], inset[(seg.edge_index + 1) % len(inset)]
            if (b[0] - a[0]) * (seg.b[0] - seg.a[0]) + (b[1] - a[1]) * (seg.b[1] - seg.a[1]) <= _TOL:
                raise ValueError("setbacks consume or split the parcel: empty footprint or collapsed corridor")
        footprint = [(x - origin[0], y - origin[1]) for x, y in inset]
        if sg.orientation(footprint) < 0:
            footprint.reverse()
        if sg.ring_self_intersects(footprint) or any(
            not sg.point_in_ring(p, outer) and sg.closest_point_on_ring(outer, p)[1] > _TOL for p in inset
        ):
            raise ValueError("setbacks split or self-intersect the parcel inset; boolean offset required")
    else:
        for seg in regulated:
            nx, ny = normals[seg.id]
            ax, ay = seg.a[0] - origin[0], seg.a[1] - origin[1]
            footprint = _clip(footprint, (nx, ny, -nx * ax - ny * ay - rules[seg.rule_id].base_setback))
            if not footprint:
                raise ValueError("setbacks consume the parcel: empty footprint")
    footprint = _clean(footprint, collinear=True)
    return footprint, origin, concave, normals, regulated


def allowed_footprint(outer, holes, segments, rules):
    """The same inset used by the envelope, available before terrain is defined."""
    footprint, origin, _, _, _ = _footprint(outer, holes, segments, rules)
    return [(x + origin[0], y + origin[1]) for x, y in footprint]


def build(outer, holes, segments, rules, datum_curves, project_max_height=0.0, base_z=-1.0, face_budget=256):
    footprint, origin, concave, normals, regulated = _footprint(outer, holes, segments, rules)
    profiles = {seg.id: _profile(seg, datum_curves.get(seg.id)) for seg in regulated}
    capped = project_max_height > 0
    if not regulated and not capped:
        raise ValueError("no regulated height or project cap: envelope is unbounded")
    initial = (
        [
            list(footprint[i] for i in tri)
            for tri in triangulate_face([(*p, 0.0) for p in footprint], Face(tuple(range(len(footprint))), "base"))
        ]
        if concave
        else [footprint]
    )
    cells = [
        _Cell(p, (0.0, 0.0, project_max_height) if capped else None, "project-cap" if capped else "") for p in initial
    ]
    for seg in regulated:
        direction, profile = profiles[seg.id]
        planes = _local_edge_planes if concave else _edge_planes
        for domain, plane in planes(seg, rules[seg.rule_id], normals[seg.id], direction, profile, origin):
            cells = _apply(cells, domain, plane, seg.id)
            if len(cells) > face_budget * 8:
                raise ValueError("terrain datum exceeds the compact envelope cell budget")
    if any(cell.plane is None for cell in cells):
        raise ValueError("unbounded envelope region")
    roof = _merge_roof(cells)
    if len(roof) + len(footprint) + 1 > face_budget:
        raise ValueError("terrain datum exceeds the compact envelope face budget (%d)" % face_budget)

    vertices, top_index, faces = [], {}, []
    for cell in roof:
        indices = []
        for p in cell.polygon:
            key, z = _key(p), _height(cell.plane, p)
            if key in top_index:
                index = top_index[key]
                if abs(vertices[index][2] - z) > _TOL * 10:
                    raise ValueError("discontinuous roof datum: cannot close envelope")
            else:
                index = len(vertices)
                top_index[key] = index
                vertices.append((p[0] + origin[0], p[1] + origin[1], z))
            indices.append(index)
        faces.append(Face(tuple(indices), "roof", cell.source))
    if not vertices or not math.isfinite(base_z) or base_z >= min(v[2] for v in vertices) - _TOL:
        raise ValueError("flat base must be below the entire envelope roof")
    perimeter = _loops([cell.polygon for cell in roof])
    if perimeter is None or len(perimeter) != 1:
        raise ValueError("roof has open or disconnected boundaries")

    solid_vertices = list(vertices)
    bottom_indices = []
    for x, y in footprint:
        bottom_indices.append(len(solid_vertices))
        solid_vertices.append((x + origin[0], y + origin[1], base_z))
    for i, a in enumerate(footprint):
        b = footprint[(i + 1) % len(footprint)]
        edge_points = []
        for p in perimeter[0]:
            _, distance, t = sg.closest_point_on_segment(*p, *a, *b)
            if distance < _TOL * 2:
                edge_points.append((t, top_index[_key(p)]))
        edge_points.sort()
        top = [index for _, index in edge_points]
        if len(top) < 2:
            raise ValueError("missing envelope wall edge")
        indices = [top[0], bottom_indices[i], bottom_indices[(i + 1) % len(footprint)]] + top[:0:-1]
        faces.append(Face(tuple(indices), "wall"))
    faces.append(Face(tuple(reversed(bottom_indices)), "base"))

    directed_edges = Counter()
    top_triangles, base_triangles, solid_triangles = [], [], []
    for face in faces:
        for a, b in zip(face.indices, face.indices[1:] + face.indices[:1], strict=True):
            directed_edges[(a, b)] += 1
        triangles = triangulate_face(solid_vertices, face)
        solid_triangles.extend(triangles)
        if face.kind == "roof":
            top_triangles.extend(triangles)
        elif face.kind == "base":
            base_triangles.extend(triangles)
    if any(count != 1 or directed_edges[(b, a)] != 1 for (a, b), count in directed_edges.items()):
        raise ValueError("envelope face topology is not a closed oriented shell")
    if len(solid_triangles) > face_budget * 8:
        raise ValueError("compact envelope exceeds the display triangle budget")
    constraints = []
    for x, y, z in vertices:
        candidates = []
        for seg in regulated:
            rule = rules[seg.rule_id]
            q, _, _ = sg.closest_point_on_segment(x, y, *seg.a, *seg.b)
            nx, ny = normals[seg.id]
            distance = (x - seg.a[0]) * nx + (y - seg.a[1]) * ny
            if concave:
                distance, q = _local_distance(seg, normals[seg.id], (x, y))
            datum = sg.polyline_z_at(datum_curves[seg.id], q)
            rise = (
                (distance - rule.base_setback) / rule.run_per_rise
                if rule.has_height_slope and rule.run_per_rise > 0
                else 0.0
            )
            raw = datum + rule.base_height + rise
            candidates.append(
                sg.Constraint(raw, seg.id, rule.id, distance, datum, clipped=capped and raw > project_max_height + _TOL)
            )
        controlling = min(candidates, key=lambda c: (c.z, c.segment_id)) if candidates else None
        constraints.append(replace(controlling, z=z) if controlling else None)
    world_footprint = [(x + origin[0], y + origin[1]) for x, y in footprint]
    bounds = tuple(min(v[k] for v in solid_vertices) for k in range(3)) + tuple(
        max(v[k] for v in solid_vertices) for k in range(3)
    )
    notes = [
        "compact: %d planar faces (%d roof, %d walls, one flat base), %d display triangles"
        % (len(faces), len(roof), len(footprint), len(solid_triangles)),
        "flat base Z %.2f m; no grid/ring subdivisions" % base_z,
    ]
    if concave:
        notes.append("concave inset; finite-edge height distances, conservative 32-sided endpoint cones")
    return sg.Envelope(
        vertices,
        top_triangles,
        constraints,
        bounds,
        {"kind": "polyhedral", "footprint": world_footprint},
        notes,
        base_vertices=solid_vertices[len(vertices) :],
        base_triangles=base_triangles,
        solid_vertices=solid_vertices,
        solid_triangles=solid_triangles,
        wall_quads=sum(face.kind == "wall" and len(face.indices) == 4 for face in faces),
        outer_ring=[vertices[top_index[_key(p)]] for p in perimeter[0]],
        faces=faces,
    )


def display_mesh(envelope):
    """Separate face normals prevent lit shading from rounding the planar shell."""
    vertices, normals, triangles = [], [], []
    for face in envelope.faces:
        normal = face_normal(envelope.solid_vertices, face.indices)
        start = len(vertices)
        remap = {index: start + i for i, index in enumerate(face.indices)}
        vertices.extend(envelope.solid_vertices[index] for index in face.indices)
        normals.extend([normal] * len(face.indices))
        triangles.extend(
            tuple(remap[index] for index in tri) for tri in triangulate_face(envelope.solid_vertices, face)
        )
    return vertices, triangles, normals


def feature_edges(envelope, min_angle_degrees=30.0):
    """Boundary/corner creases; omit coplanar and shallow seams, never GPU diagonals.

    The default matches the existing overlay mesh feature-edge angle (degrees).
    This affects display only, not the terrain-based shell or its topology.
    """
    if not math.isfinite(min_angle_degrees) or not 0 <= min_angle_degrees <= 180:
        raise ValueError("feature edge angle must be between 0 and 180 degrees")
    threshold = math.cos(math.radians(min_angle_degrees))
    adjacent = defaultdict(list)
    for face in envelope.faces:
        normal = face_normal(envelope.solid_vertices, face.indices)
        for a, b in zip(face.indices, face.indices[1:] + face.indices[:1], strict=True):
            adjacent[tuple(sorted((a, b)))].append(normal)
    return [
        (envelope.solid_vertices[a], envelope.solid_vertices[b])
        for (a, b), normals in adjacent.items()
        if len(normals) != 2 or sum(x * y for x, y in zip(*normals, strict=True)) < threshold - 1e-8
    ]
