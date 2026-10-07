"""Native DG bake's bounded write adapter; no UI, default guesses or write retries.

Slabs and optional walls use one atomic native transaction. Envelope Morphs use
Tapir creation plus native finalization. No failed write is automatically retried.
"""

import math
import uuid
from collections import Counter

import tapioca


def _attribute(settings, key):
    value = settings.get(key)
    if not isinstance(value, dict) or not value.get("name"):
        raise ValueError("Choose an existing project attribute: " + key)
    identifier = uuid.UUID(value["guid"])
    if not identifier.int:
        raise ValueError("Invalid attribute identifier: " + key)
    return value


def _length(settings, key):
    value = settings.get(key)
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or not 0 < value <= 10
    ):
        raise ValueError("Invalid positive bake thickness: " + key)
    return value


def _ring(points):
    if not isinstance(points, list) or not 3 <= len(points) <= 4096:
        raise ValueError("Bake contour needs 3..4096 points")
    for point in points:
        if not isinstance(point, dict) or set(point) != {"x", "y"}:
            raise ValueError("Invalid bake point")
        if any(
            isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or abs(v) > 1e9
            for v in point.values()
        ):
            raise ValueError("Invalid bake coordinate")
    return points


def _solid(item):
    if set(item) != {"basePoint", "body"} or item["body"].get("bodyType") != "Solid":
        raise ValueError("Envelope bake needs a validated solid shell")
    body = item["body"]
    vertices, polygons = body.get("vertices"), body.get("polygons")
    if not isinstance(vertices, list) or not 4 <= len(vertices) <= 100000:
        raise ValueError("Envelope vertex budget exceeded or empty body")
    if not isinstance(polygons, list) or not 4 <= len(polygons) <= 100000:
        raise ValueError("Envelope face budget exceeded or empty body")
    for point in [item["basePoint"]] + vertices:
        if set(point) != {"x", "y", "z"} or any(
            isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or abs(v) > 1e9
            for v in point.values()
        ):
            raise ValueError("Invalid Morph coordinate")
    edges, volumes = Counter(), []
    for polygon in polygons:
        ids = polygon.get("vertexIds")
        if not isinstance(ids, list) or len(ids) != 3 or len(set(ids)) != 3:
            raise ValueError("Invalid triangulated Morph face")
        if any(type(i) is not int or not 0 <= i < len(vertices) for i in ids):
            raise ValueError("Invalid Morph vertex index")
        edges.update(zip(ids, ids[1:] + ids[:1], strict=True))
        a, b, c = [vertices[i] for i in ids]
        volumes.append(
            (
                a["x"] * (b["y"] * c["z"] - b["z"] * c["y"])
                + a["y"] * (b["z"] * c["x"] - b["x"] * c["z"])
                + a["z"] * (b["x"] * c["y"] - b["y"] * c["x"])
            )
            / 6
        )
    if any(count != 1 or edges[(b, a)] != 1 for (a, b), count in edges.items()) or math.fsum(volumes) <= 1e-9:
        raise ValueError("Morph needs a closed outward solid; no box substitute")


def plan(request):
    """Validate the entire captured request before touching the project."""
    kind, items, settings = request["kind"], request["items"], request["settings"]
    if kind not in {"slices", "envelope", "collapse"} or not isinstance(items, list) or not 1 <= len(items) <= 2048:
        raise ValueError("Empty or over-budget bake")
    layer = _attribute(settings, "layer")["name"]
    primary = []
    wall_count = 0
    points = 0
    if kind == "envelope":
        material = {"guid": _attribute(settings, "material")["guid"]}
        surface = {"guid": _attribute(settings, "surface")["guid"]}
        for item in items:
            # C++ Geometry validates closed, outward, welded shells before DG.
            _solid(item)
            primary.append({**item, "buildingMaterialId": material, "surfaceId": surface})
        return {
            "kind": kind,
            "create": "Tapir.CreateMorphs",
            "field": "morphsData",
            "primary": primary,
            "wallCount": 0,
            "finishes": [
                {"morphMaterial": settings["material"]["name"], "morphSurface": settings["surface"]["name"]}
                for _ in items
            ],
            "layer": layer,
        }
    if kind == "slices":
        composite = settings.get("compositeOn", False)
        attribute = _attribute(settings, "composite" if composite else "material")["name"]
        thickness = _length(settings, "slabThickness")
        with_walls = settings.get("walls", False)
        if with_walls:
            wall_composite = settings.get("wallCompositeOn", False)
            wall_attribute = _attribute(settings, "wallComposite" if wall_composite else "wallMaterial")
            wall_layer = _attribute(settings, "wallLayer")["name"]
            wall_thickness = _length(settings, "wallThickness")
    else:
        fill, line = _attribute(settings, "fill")["name"], _attribute(settings, "line")["name"]
        for key in ("pen", "fillPen"):
            if type(settings.get(key)) is not int or not 1 <= settings[key] <= 255:
                raise ValueError("Choose a native project pen: " + key)
    for item in items:
        outer = _ring(item["outer"])
        holes = item.get("holes", [])
        if not isinstance(holes, list) or len(holes) > 1024:
            raise ValueError("Bake hole budget exceeded")
        rings = [outer] + [_ring(hole) for hole in holes]
        points += sum(len(ring) for ring in rings)
        if points > 100000:
            raise ValueError("Bake contour budget exceeded")
        hole_records = [{"polygonOutline": hole} for hole in holes]
        if kind == "collapse":
            if "arcs" in item:
                for ring, angles in zip(rings, [item["arcs"]] + item["holeArcs"], strict=True):
                    if len(angles) != len(ring) or any(
                        isinstance(angle, bool)
                        or not isinstance(angle, (int, float))
                        or not math.isfinite(angle)
                        or abs(angle) > math.pi
                        for angle in angles
                    ):
                        raise ValueError("Invalid collapse circular edges")
                for hole, angles in zip(hole_records, item["holeArcs"], strict=True):
                    hole["arcAngles"] = angles
            primary.append(
                {
                    "polygonOutline": outer,
                    "holes": hole_records,
                    "layer": layer,
                    "fill": fill,
                    "lineType": line,
                    "pen": settings["pen"],
                    "fillPen": settings["fillPen"],
                    "fillBGPen": 0,
                }
            )
            if "arcs" in item:
                primary[-1]["arcAngles"] = item["arcs"]
            continue
        z, height = item["z"], item["height"]
        if not math.isfinite(z) or abs(z) > 1e9 or not math.isfinite(height) or height <= 0:
            raise ValueError("Invalid physical slice elevation/height")
        primary.append(
            {
                "group": item.get("group", "slab:unidentified"),
                "z": z,
                "height": height,
                "layer": layer,
                "structure": "composite" if composite else "basic",
                "attribute": attribute,
                "thickness": thickness,
                "polygonOutline": outer,
                "holes": hole_records,
            }
        )
        if with_walls:
            primary[-1]["wall"] = {
                "layer": wall_layer,
                "structure": "composite" if wall_composite else "basic",
                "attribute": wall_attribute["name"],
                "thickness": wall_thickness,
            }
            for ring in rings:
                for begin, end in zip(ring, ring[1:] + ring[:1], strict=True):
                    if begin == end:
                        raise ValueError("Zero-length perimeter wall")
                    wall_count += 1
    if len(primary) + wall_count > 2048:
        raise ValueError("Bake element budget exceeded (2048, including perimeter walls)")
    return {
        "kind": kind,
        "create": "Tapioca.BakeMassingSlices" if kind == "slices" else "Tapioca.CreateFills",
        "field": "slabs" if kind == "slices" else "fills",
        "primary": primary,
        "wallCount": wall_count,
        "layer": layer,
        "wallLayer": wall_layer if kind == "slices" and with_walls else None,
    }


def execute(request, call=None):
    """No success without unique created GUIDs and completed chosen settings."""
    call = call or tapioca.api.call
    created, stages = [], []
    stage = "validation"

    def guard():
        if not call("Tapioca.MassingBakeGuard", {"token": request["token"]}).data.get("current"):
            raise RuntimeError("Bake session closed/replaced; remaining writes refused")

    def identifiers(data, expected, field):
        records = data.get(field, [])
        verified = []
        failures = []
        for record in records:
            try:
                if record.get("error") or record.get("succeeded") is False:
                    raise ValueError(str(record.get("error") or "creation refused"))
                guid = str(uuid.UUID(record["elementId"]["guid"]))
                if uuid.UUID(guid).int == 0 or guid in created:
                    raise ValueError("null/duplicate created GUID")
                created.append(guid)
                verified.append({"guid": guid})
            except (KeyError, ValueError, TypeError) as exc:
                failures.append(str(exc))
        if (
            failures
            or not verified
            or (expected is not None and (len(records) != expected or len(verified) != expected))
        ):
            raise RuntimeError("Incomplete creation response: " + "; ".join(failures))
        return verified

    try:
        prepared = plan(request)
        guard()
        if prepared["kind"] == "envelope":
            stage = "Tapir capability check"
            commands = [prepared["create"].split(".")[1]]
            for command in commands:
                available = call(
                    "API.IsAddOnCommandAvailable",
                    {"addOnCommandId": {"commandNamespace": "TapirCommand", "commandName": command}},
                ).data
                if not available.get("available"):
                    raise RuntimeError("Required Tapir command unavailable: " + command)
        stage = prepared["create"]
        guard()
        params = {prepared["field"]: prepared["primary"]}
        if prepared["kind"] == "slices":
            params["token"] = request["token"]
        if prepared["kind"] == "collapse":
            params["failOnError"] = True
            params["floorPlan"] = True
        response = call(stage, params).data
        primary_ids = identifiers(
            response,
            None if prepared["kind"] == "slices" else len(prepared["primary"]),
            "results" if prepared["kind"] == "collapse" else "elements",
        )
        stages.append(stage)
        if prepared["kind"] == "slices":
            if response.get("count") != len(primary_ids):
                raise RuntimeError("Incomplete native slice bake response")
            return {
                "state": "complete",
                "createdGuids": created,
                "stages": stages,
                "note": "Created slabs and optional walls with chosen settings. One Undo step. Not massing sources.",
            }
        if prepared["kind"] == "collapse":
            return {
                "state": "complete",
                "createdGuids": created,
                "stages": stages,
                "note": "Created collapse-zone fills. One Undo step.",
            }
        final = [
            {"elementId": guid, "layer": prepared["layer"], **finish}
            for guid, finish in zip(primary_ids, prepared["finishes"], strict=True)
        ]
        stage = "Tapioca.FinishMassingBake"
        guard()
        finalized = call(stage, {"token": request["token"], "elements": final}).data
        if finalized.get("count") != len(final):
            raise RuntimeError("Incomplete native finalization response")
        stages.append(stage)
        return {
            "state": "complete",
            "createdGuids": created,
            "stages": stages,
            "note": "Created copies with chosen settings. %d separate Undo stages." % len(stages),
        }
    except Exception as exc:  # Return the durable ledger even for an uncertain transport failure.
        return {
            "state": "partial" if created else "refused",
            "createdGuids": created,
            "stages": stages,
            "note": "%s: %s. No writes retried. Inspect the project/Undo before another bake; "
            "a failed transport may have completed a write." % (stage, exc),
        }
