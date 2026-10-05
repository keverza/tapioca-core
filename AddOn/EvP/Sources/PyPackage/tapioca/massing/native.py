"""Snapshot-only calculation entry point for native ImGui (no UI or ACAPI).

Uses the same planar solver as SetbacksSmoke. The native owner handles request
generation, cancellation, source validation and overlay publication. This module
never writes metadata, opens undo scopes, publishes layers or bakes Morphs.
"""

import math
from dataclasses import replace

import numpy as np

from . import polyhedral, refpoints, terrain_sample
from . import setbackgeom as sg


def _number(value, label, minimum=-1e9, maximum=1e9):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{label} must be finite")
    if not minimum <= value <= maximum:
        raise ValueError(f"{label} is out of range")
    return float(value)


def _definition(request):
    if request.get("version") != 1:
        raise ValueError("unsupported native massing request version")
    edges = request["edges"]
    if not 3 <= len(edges) <= 256:
        raise ValueError("property line needs 3..256 edges")
    ring, segments, rules = [], [], {}
    base = _number(request["baseHeight"], "base height", 0, 1000)
    run = _number(request["runPerRise"], "run per rise", 0.01, 1000)
    cap = _number(request["capZ"], "project cap Z", -1e6, 1e6)
    if not isinstance(request["capped"], bool) or (request["capped"] and cap <= 0):
        raise ValueError("enabled project cap must be a positive project Z")
    depth = _number(request["baseDepth"], "flat base depth", 0.01, 100)
    for i, edge in enumerate(edges):
        a = tuple(_number(v, "edge endpoint") for v in edge["a"])
        b = tuple(_number(v, "edge endpoint") for v in edge["b"])
        if len(a) != 2 or len(b) != 2 or math.dist(a, b) <= 1e-7:
            raise ValueError("invalid property-line edge")
        angle = _number(edge["arc"], "arc angle")
        if abs(angle) > 1e-9:
            raise ValueError("compact envelope does not yet support curved property-line edges")
        if edge.get("review", True):
            raise ValueError("review changed property-line assignments before calculating")
        if not isinstance(edge["regulated"], bool) or not isinstance(edge["reference"], bool):
            raise ValueError("height and endpoint flags must be booleans")
        setback = _number(edge["distance"], "setback", 0, 1000)
        vertical = edge.get("vertical", False)
        if not isinstance(vertical, bool):
            raise ValueError("vertical road flag must be a boolean")
        rule_id = f"edge-{i}"
        rules[rule_id] = (
            replace(sg.STR_193, id=rule_id, base_setback=setback, base_height=base, run_per_rise=run)
            if edge["regulated"] and not vertical
            else sg.NONE
        )
        ring.append(a)
        segments.append(sg.Segment(f"S{i + 1}", i, a, b, angle, rule_id))
    if any(math.dist(s.b, segments[(i + 1) % len(segments)].a) > 1e-7 for i, s in enumerate(segments)):
        raise ValueError("property line is not closed")
    return ring, segments, rules, cap, depth


def _surface(request):
    if request.get("terrain") is None:
        raise ValueError("Define one terrain Mesh to preview terrain-following lines and the envelope")
    vertices = np.asarray(request["terrain"]["vertices"], dtype=np.float64)
    indices = np.asarray(request["terrain"]["triangles"])
    if vertices.ndim != 1 or len(vertices) % 3 or not 9 <= len(vertices) <= 600000:
        raise ValueError("invalid terrain vertex buffer")
    if indices.ndim != 1 or len(indices) % 3 or not 3 <= len(indices) <= 600000:
        raise ValueError("invalid terrain triangle buffer")
    if not np.isfinite(vertices).all() or not np.issubdtype(indices.dtype, np.integer):
        raise ValueError("terrain buffers must contain finite vertices and integer indices")
    vertices = vertices.reshape((-1, 3))
    if np.any(indices < 0) or np.any(indices >= len(vertices)):
        raise ValueError("terrain triangle index is out of range")
    surface = terrain_sample.build(vertices, indices.reshape((-1, 3)))
    if surface.is_empty:
        raise ValueError("defined terrain has no upward-facing surface")
    return surface


def calculate(request):
    """Return finite, JSON-compatible geometry/mean-only figures from one snapshot."""
    ring, segments, rules, cap, depth = _definition(request)
    surface = _surface(request)
    curves, boundary = {}, []
    for segment in segments:
        runs = surface.segment_profiles(segment.a, segment.b)
        complete = (
            len(runs) == 1
            and math.dist(runs[0][0][:2], segment.a) <= 1e-7
            and math.dist(runs[0][-1][:2], segment.b) <= 1e-7
        )
        if not complete:
            raise ValueError(f"{segment.id} lacks complete boundary terrain coverage")
        curves[segment.id] = runs[0]
        boundary.append([v for point in runs[0] for v in point])
    selected = [ring[i] for i, edge in enumerate(request["edges"]) if edge["reference"]]
    references = refpoints.project_points_z(selected, surface.z_at)
    summary = refpoints.average_elevation(references)
    mean = summary["average"]
    altitude = request.get("originAltitude")
    if altitude is not None:
        altitude = _number(altitude, "project origin altitude")
    base_z = min(float(v[2]) for v in surface.vertices) - depth
    envelope = polyhedral.build(
        ring, [], segments, rules, curves, project_max_height=cap if request["capped"] else 0, base_z=base_z
    )
    display, triangles, normals = polyhedral.display_mesh(envelope)
    footprint = envelope.grid["footprint"]
    offsets = [
        [float(v) for point in profile for v in point]
        for a, b in zip(footprint, footprint[1:] + footprint[:1], strict=True)
        for profile in surface.segment_profiles(a, b)
    ]
    return {
        "version": 1,
        "hasEnvelope": True,
        "note": "",
        "offsetXY": [float(v) for point in footprint for v in point],
        "offsets": offsets,
        "vertices": [float(v) for point in display for v in point],
        "triangles": [int(v) for tri in triangles for v in tri],
        "normals": [float(v) for normal in normals for v in normal],
        "wires": [[float(v) for point in line for v in point] for line in polyhedral.feature_edges(envelope)],
        "boundary": boundary,
        "references": [
            float(v) for point in references if point["z"] is not None for v in (point["x"], point["y"], point["z"])
        ],
        "meanZ": mean,
        "meanASL": mean + altitude if mean is not None and altitude is not None else None,
        "parcelArea": abs(sg.signed_area(ring)),
        "allowedArea": abs(sg.signed_area(envelope.grid["footprint"])),
        "faces": len(envelope.faces),
    }


def preview(request):
    """Keep the parcel/inset usable while terrain or height inputs are incomplete.

    An unavailable shell never fabricates an allowed volume. Partial outputs are
    domain data; only Calculate's strict geometry path can set hasEnvelope.
    """
    ring, segments, rules, _, _ = _definition(request)
    result = {
        "version": 1,
        "hasEnvelope": False,
        "note": "",
        "offsetXY": [],
        "offsets": [],
        "vertices": [],
        "triangles": [],
        "normals": [],
        "wires": [],
        "boundary": [],
        "references": [],
        "meanZ": None,
        "meanASL": None,
        "parcelArea": abs(sg.signed_area(ring)),
        "allowedArea": 0,
        "faces": 0,
    }
    try:
        footprint = polyhedral.allowed_footprint(ring, [], segments, rules)
        result["offsetXY"] = [float(v) for point in footprint for v in point]
        result["allowedArea"] = abs(sg.signed_area(footprint))
        return calculate(request)
    except ValueError as error:
        result["note"] = str(error)
    try:
        surface = _surface(request)
        result["boundary"] = [
            [float(v) for point in profile for v in point]
            for segment in segments
            for profile in surface.segment_profiles(segment.a, segment.b)
        ]
        footprint = list(zip(result["offsetXY"][::2], result["offsetXY"][1::2], strict=True))
        result["offsets"] = [
            [float(v) for point in profile for v in point]
            for a, b in zip(footprint, footprint[1:] + footprint[:1], strict=True)
            for profile in surface.segment_profiles(a, b)
        ]
    except ValueError:
        pass  # No terrain means an inset in the HUD, never a fictitious 3D datum.
    return result
