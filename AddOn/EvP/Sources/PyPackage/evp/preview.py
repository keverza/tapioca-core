"""What the palette's preview band SHOWS — built by the command, capped by this.

    def preview(inputs, ctx):
        ctx.scene.mesh(verts, tris, role="add", label="1 of 12 - all identical")
        ctx.scene.lines(axis, role="guide")
        ctx.scene.note("12 walls planned; one shown")

A preview is a FRAGMENT, chosen by the command because it is representative — one
wall of a stack, one label placement, the first bay of a grid. It is not the model
and it is not the whole plan. Only the command knows which fragment answers the
question the user is actually asking, which is why the framework does not try to
derive one.

⚠️ THE PREVIEW BAND NEVER RUNS THE EXTRACTION THREAD. `ArchVizPanel`'s viewport
reads the whole model through ACAPI on the main thread; a preview that did the
same on every parameter change would make the palette unusable on a real project.
The geometry here crosses the bus from the command instead — which is affordable
precisely because a fragment is small, and stays affordable because of the budget
below. If you find yourself wanting the real model in the band, you want the
popout (`ArchVizPanel`), not this.

⚠️ THE BUDGET IS A REFUSAL, NOT A TRUNCATION. Over the cap raises, naming the cap
and what exceeded it. A preview that silently dropped half its geometry would be a
picture the user would trust and should not — the whole point of the band is that
what it shows is what would happen.

The band type (`3d` / `plan2d`) is DECLARED on the command, not inferred from what
lands here: the palette has to size the band before any command code runs.
"""

from __future__ import annotations

import json as _json

__all__ = ["PreviewScene", "PreviewBudgetError", "ROLES",
           "MAX_TRIANGLES", "MAX_VERTICES", "MAX_MESHES", "MAX_LINE_POINTS"]


#: What a piece of preview geometry MEANS, and therefore how it is drawn. The
#: colours belong to the framework so that green means "would be created" in
#: every command's preview, the way the diff text already reads the same way.
ROLES = {
    "add":     "would be created",
    "remove":  "would be deleted",
    "modify":  "would change",
    "context": "already there, for orientation",
    "guide":   "not geometry - an axis, a bound, a reference line",
}

# The caps. Deliberately small: this is a thumbnail of an intent, not a viewer.
# A command that needs more is showing the model rather than a fragment, and the
# refusal says so.
#
# Sized against what actually crosses the bus: a triangle is 3 uint32 indices and
# its vertices are 3 float64 each, JSON-encoded. 20k triangles is roughly a
# megabyte of text — a tenth of a second to serialise, which is under the debounce
# and therefore invisible.
MAX_TRIANGLES = 20000
MAX_VERTICES = 30000
MAX_MESHES = 64
MAX_LINE_POINTS = 20000


class PreviewBudgetError(ValueError):
    """A scene asked for more than the preview band will carry."""


class PreviewScene:
    """The geometry and notes one preview shows. Built up, then sent once.

    Not frozen and not validated per call beyond the budget: a planner builds this
    in a loop, and a model round trip per triangle would cost more than the
    picture is worth.
    """

    __slots__ = ("kind", "meshes", "polylines", "notes", "_triangles", "_vertices",
                 "_line_points")

    def __init__(self, kind="3d"):
        #: "3d" or "plan2d" — what the command declared. Carried so the payload is
        #: self-describing; the palette already knows it from the scan.
        self.kind = kind
        self.meshes = []
        self.polylines = []
        self.notes = []
        self._triangles = 0
        self._vertices = 0
        self._line_points = 0

    # -- building ----------------------------------------------------------

    def mesh(self, vertices, triangles, role="add", label="", normals=None):
        """Add one surface. `vertices` is (n,3) or flat xyz; `triangles` is (m,3)
        or flat indices.

        `normals` is optional — omitted, per-vertex normals are averaged from the
        faces here rather than on the render thread, which is the same rule
        `SceneCmdQueue::ElementUpload` already states for the extraction path: the
        producer does the per-vertex work, never the frame loop.
        """
        _check_role(role)
        flat_vertices = _flatten(vertices, 3, "vertices")
        flat_triangles = [int(index) for index in _flatten(triangles, 3, "triangles")]

        vertex_count = len(flat_vertices) // 3
        triangle_count = len(flat_triangles) // 3
        if vertex_count == 0 or triangle_count == 0:
            raise ValueError(
                "mesh(role=%r) has %d vertices and %d triangles. An empty mesh in a "
                "preview is a bug in the planner, not an empty picture — say it with "
                "note() if there is genuinely nothing to show."
                % (role, vertex_count, triangle_count))

        highest = max(flat_triangles)
        if highest >= vertex_count:
            raise ValueError(
                "mesh(role=%r) indexes vertex %d but only has %d. An out-of-range "
                "index reads as scrambled geometry rather than an error, so it is "
                "caught here." % (role, highest, vertex_count))

        self._spend(triangles=triangle_count, vertices=vertex_count, meshes=1)

        self.meshes.append({
            "role": role,
            "label": str(label),
            "vertices": flat_vertices,
            "normals": (_flatten(normals, 3, "normals") if normals is not None
                        else _vertex_normals(flat_vertices, flat_triangles)),
            "triangles": flat_triangles,
        })
        return self

    def lines(self, points, role="guide", label="", closed=False):
        """Add one polyline. `points` is (n,3) or flat xyz; in a plan2d scene the
        z is carried but ignored by the band."""
        _check_role(role)
        flat = _flatten(points, 3, "points")
        count = len(flat) // 3
        if count < 2:
            raise ValueError(
                "lines(role=%r) needs at least two points, got %d." % (role, count))
        self._spend(line_points=count)
        self.polylines.append({"role": role, "label": str(label),
                               "closed": bool(closed), "points": flat})
        return self

    def note(self, text):
        """One line of explanation, shown under the band.

        This is where "12 planned, 1 shown" goes. A fragment that does not say it
        is a fragment is a picture the user will read as the whole answer.
        """
        self.notes.append(str(text))
        return self

    # -- reading -----------------------------------------------------------

    @property
    def triangle_count(self):
        return self._triangles

    @property
    def is_empty(self):
        return not (self.meshes or self.polylines)

    def bounds(self):
        """(min_xyz, max_xyz) over everything in the scene, or None when empty.

        The band frames the camera on this, so a scene with one tiny mesh opens
        looking at that mesh rather than at the origin.
        """
        low = [float("inf")] * 3
        high = [float("-inf")] * 3
        found = False
        for flat in ([m["vertices"] for m in self.meshes]
                     + [p["points"] for p in self.polylines]):
            for start in range(0, len(flat), 3):
                found = True
                for axis in range(3):
                    value = flat[start + axis]
                    low[axis] = min(low[axis], value)
                    high[axis] = max(high[axis], value)
        return (tuple(low), tuple(high)) if found else None

    def to_payload(self):
        """The parameters for `Tapioca.SetPreviewScene`.

        ⚠️ Meshes and polylines cross as ARRAYS OF JSON STRINGS, the same shape
        `evp.ui.table` uses for its rows. A bare nested array does not survive
        ObjectState on the C++ side — that is a wire fact, not a style choice, and
        it has been re-learned once already.
        """
        payload = {
            "kind": self.kind,
            "meshes": [_json.dumps(mesh) for mesh in self.meshes],
            "lines": [_json.dumps(line) for line in self.polylines],
            "notes": list(self.notes),
        }
        extent = self.bounds()
        if extent is not None:
            payload["boundsMin"] = list(extent[0])
            payload["boundsMax"] = list(extent[1])
        return payload

    def send(self):
        """Hand the scene to the palette's preview band. Fire-and-forget.

        Returns the envelope rather than raising, like `evp.ui`: a palette with no
        preview band open is a normal state, not a script error.
        """
        from .api import call  # deferred — `import evp` must not need a transport

        return call("Tapioca.SetPreviewScene", self.to_payload(), raise_on_error=False)

    # -- the budget --------------------------------------------------------

    def _spend(self, triangles=0, vertices=0, meshes=0, line_points=0):
        checks = (
            (self._triangles + triangles, MAX_TRIANGLES, "triangles"),
            (self._vertices + vertices, MAX_VERTICES, "vertices"),
            (len(self.meshes) + meshes, MAX_MESHES, "meshes"),
            (self._line_points + line_points, MAX_LINE_POINTS, "polyline points"),
        )
        for total, cap, what in checks:
            if total > cap:
                raise PreviewBudgetError(
                    "the preview would hold %d %s, over the cap of %d. The band is a "
                    "thumbnail of an intent, not a viewer: show ONE representative "
                    "element and say so with note(), or send the user to the popout "
                    "for the whole model." % (total, what, cap))
        self._triangles += triangles
        self._vertices += vertices
        self._line_points += line_points


# ---------------------------------------------------------------------------

def _check_role(role):
    if role not in ROLES:
        raise ValueError(
            "unknown preview role %r. Known: %s. The role is what colours the "
            "geometry, and the colours mean the same thing in every command's "
            "preview — which is why this is a fixed set."
            % (role, ", ".join(sorted(ROLES))))


def _flatten(values, stride, what):
    """Accept (n, stride) rows, a flat sequence, or a numpy array. Return a flat
    list of floats.

    Three shapes because all three arrive honestly: `evp.geometry` hands back
    numpy, a planner building points by hand writes tuples, and a serialised
    fixture is flat.
    """
    if values is None:
        raise ValueError("%s is None" % what)

    tolist = getattr(values, "tolist", None)  # numpy, without importing it
    if tolist is not None:
        values = tolist()

    values = list(values)
    if values and isinstance(values[0], (list, tuple)):
        flat = []
        for row in values:
            row = list(row)
            if len(row) != stride:
                raise ValueError(
                    "%s has a row of %d, expected %d" % (what, len(row), stride))
            flat.extend(float(component) for component in row)
        return flat

    if len(values) % stride:
        raise ValueError(
            "%s has %d values, which is not a multiple of %d"
            % (what, len(values), stride))
    return [float(component) for component in values]


def _vertex_normals(vertices, triangles):
    """Area-weighted per-vertex normals, in pure Python.

    Area-weighted because it costs nothing extra — the un-normalised cross product
    IS twice the face area — and it stops a fan of slivers from dominating the
    normal at a shared corner. Pure Python because numpy is not guaranteed in
    every runtime this package imports into, and the budget above keeps the loop
    small enough that it does not matter.
    """
    normals = [0.0] * len(vertices)
    for start in range(0, len(triangles), 3):
        a, b, c = (triangles[start] * 3, triangles[start + 1] * 3, triangles[start + 2] * 3)
        ux, uy, uz = (vertices[b] - vertices[a],
                      vertices[b + 1] - vertices[a + 1],
                      vertices[b + 2] - vertices[a + 2])
        vx, vy, vz = (vertices[c] - vertices[a],
                      vertices[c + 1] - vertices[a + 1],
                      vertices[c + 2] - vertices[a + 2])
        nx, ny, nz = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
        for corner in (a, b, c):
            normals[corner] += nx
            normals[corner + 1] += ny
            normals[corner + 2] += nz

    for start in range(0, len(normals), 3):
        length = (normals[start] ** 2 + normals[start + 1] ** 2
                  + normals[start + 2] ** 2) ** 0.5
        if length > 1e-12:
            normals[start] /= length
            normals[start + 1] /= length
            normals[start + 2] /= length
        else:
            # A vertex used only by degenerate faces. Up is arbitrary but stable;
            # leaving a zero normal makes the shader produce a black hole instead.
            normals[start], normals[start + 1], normals[start + 2] = 0.0, 0.0, 1.0
    return normals
