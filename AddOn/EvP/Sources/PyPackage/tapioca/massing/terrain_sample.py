"""Shared terrain surface sampling for the setbacks workflow.

A local, minimal adaptation of `private/Commands/TopographyAnalysis/terrain_kernel.py`
(pure numpy, unit-tested there). It is copied rather than imported because
TopographyAnalysis has no `_exports.py`; when the production command lands, one
copy should be promoted to a shared module and both should import it.

The snapshot delivers a terrain mesh as a full solid box, so `build()` keeps only
upward-facing triangles — that is the surface Z is measured on. `TerrainSurface.z_at`
is vectorised over the triangles. Boundary profiles include exact triangle-edge
crossings rather than relying on a fixed sampling interval.
"""

from __future__ import annotations

import numpy as np

_UP_FACE_DOT_MIN = 0.001


def triangle_normals(vertices, triangles):
    a = vertices[triangles[:, 0]]
    b = vertices[triangles[:, 1]]
    c = vertices[triangles[:, 2]]
    normals = np.cross(b - a, c - a)
    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
    lengths[lengths == 0.0] = 1.0
    return normals / lengths


class TerrainSurface:
    """An upward-facing terrain surface; `z_at` interpolates in XY."""

    __slots__ = ("vertices", "triangles", "_a", "_b", "_c", "_den", "_edges")

    def __init__(self, vertices, triangles):
        self.vertices = np.ascontiguousarray(vertices, dtype=np.float64)
        self.triangles = np.ascontiguousarray(triangles, dtype=np.uint32)
        if len(self.triangles):
            self._a = self.vertices[self.triangles[:, 0]]
            self._b = self.vertices[self.triangles[:, 1]]
            self._c = self.vertices[self.triangles[:, 2]]
            v0x = self._b[:, 0] - self._a[:, 0]
            v0y = self._b[:, 1] - self._a[:, 1]
            v1x = self._c[:, 0] - self._a[:, 0]
            v1y = self._c[:, 1] - self._a[:, 1]
            self._den = v0x * v1y - v1x * v0y
        else:
            self._a = self._b = self._c = np.empty((0, 3), dtype=np.float64)
            self._den = np.empty((0,), dtype=np.float64)
        pairs = np.concatenate([self.triangles[:, [0, 1]], self.triangles[:, [1, 2]], self.triangles[:, [2, 0]]])
        self._edges = self.vertices[np.unique(np.sort(pairs, axis=1), axis=0)]

    @property
    def vertex_count(self):
        return len(self.vertices)

    @property
    def triangle_count(self):
        return len(self.triangles)

    @property
    def is_empty(self):
        return len(self.triangles) == 0

    def bounds(self):
        if not len(self.vertices):
            return None
        return (
            self.vertices.min(axis=0).astype(float).tolist(),
            self.vertices.max(axis=0).astype(float).tolist(),
        )

    def z_at(self, x, y):
        """Interpolated Z at (x, y), or None when outside every triangle.

        Deterministic: the lowest triangle index wins on an edge or shared vertex.
        """
        if self.is_empty:
            return None
        px, py = float(x), float(y)
        v2x = px - self._a[:, 0]
        v2y = py - self._a[:, 1]
        ok = np.abs(self._den) > 1e-12
        if not ok.any():
            return None
        den = np.where(ok, self._den, 1.0)
        u = np.where(ok, (v2x * (self._c[:, 1] - self._a[:, 1]) - (self._c[:, 0] - self._a[:, 0]) * v2y) / den, 0.0)
        v = np.where(ok, ((self._b[:, 0] - self._a[:, 0]) * v2y - v2x * (self._b[:, 1] - self._a[:, 1])) / den, 0.0)
        w = 1.0 - u - v
        hit = ok & (u >= -1e-9) & (v >= -1e-9) & (w >= -1e-9)
        indices = np.flatnonzero(hit)
        if not len(indices):
            return None
        i = int(indices[0])
        # u and v multiply B-A and C-A: A's weight is w, NOT u. The old
        # permutation looked plausible on flat fixtures but jumped at every ridge.
        z = self._a[i, 2] * w[i] + self._b[i, 2] * u[i] + self._c[i, 2] * v[i]
        return float(z)

    def segment_profiles(self, a, b):
        """Exact vertical cut of AB against the terrain, as separate valid runs.

        Insert triangle-edge crossings, including endpoints of collinear edges.
        Never connect across an uncovered interval. Every remaining interval is
        inside one planar terrain triangle, so its endpoint chord is exact.
        """
        a, b = np.asarray(a, dtype=float)[:2], np.asarray(b, dtype=float)[:2]
        direction = b - a
        length_sq = float(direction @ direction)
        if self.is_empty or length_sq < 1e-18:
            return []
        c, d = self._edges[:, 0, :2], self._edges[:, 1, :2]
        edge = d - c
        offset = c - a
        denominator = direction[0] * edge[:, 1] - direction[1] * edge[:, 0]
        parallel = np.abs(denominator) < 1e-12
        safe_den = np.where(parallel, 1.0, denominator)
        t = (offset[:, 0] * edge[:, 1] - offset[:, 1] * edge[:, 0]) / safe_den
        u = (offset[:, 0] * direction[1] - offset[:, 1] * direction[0]) / safe_den
        hit = ~parallel & (t >= 0.0) & (t <= 1.0) & (u >= -1e-9) & (u <= 1.0 + 1e-9)
        collinear = parallel & (np.abs(offset[:, 0] * direction[1] - offset[:, 1] * direction[0]) < 1e-9)
        params = [0.0, 1.0] + t[hit].tolist()
        for endpoint in (c[collinear], d[collinear]):
            params.extend(((endpoint - a) @ direction / length_sq).tolist())
        params = sorted(max(0.0, min(1.0, float(value))) for value in params)
        unique = []
        for value in params:
            if not unique or value - unique[-1] > 1e-9:
                unique.append(value)

        runs = []
        current = []
        for start, end in zip(unique, unique[1:], strict=False):
            p, q = a + start * direction, a + end * direction
            middle = a + (start + end) * 0.5 * direction
            z_p, z_q = self.z_at(*p), self.z_at(*q)
            if self.z_at(*middle) is None or z_p is None or z_q is None:
                if current:
                    runs.append(current)
                    current = []
                continue
            first = (float(p[0]), float(p[1]), z_p)
            last = (float(q[0]), float(q[1]), z_q)
            if not current:
                current.append(first)
            current.append(last)
        if current:
            runs.append(current)
        return runs


def build(vertices, triangles, normals=None):
    """Upward-facing terrain surface, compacted. Empty input is allowed."""
    vertices = np.asarray(vertices, dtype=np.float64)
    triangles = np.asarray(triangles, dtype=np.uint32)
    if vertices.ndim != 2 or vertices.shape[1] != 3:
        raise ValueError("vertices must be (n, 3)")
    if triangles.ndim != 2 or triangles.shape[1] != 3:
        raise ValueError("triangles must be (m, 3)")
    if len(triangles) == 0:
        return TerrainSurface(vertices, triangles)
    if normals is None:
        normals = triangle_normals(vertices, triangles)
    else:
        normals = np.asarray(normals, dtype=np.float64)
        if normals.shape != (len(triangles), 3):
            raise ValueError("normals must be (m, 3)")
    up = np.flatnonzero(normals[:, 2] > _UP_FACE_DOT_MIN)
    if not len(up):
        return TerrainSurface(np.empty((0, 3), dtype=np.float64), np.empty((0, 3), dtype=np.uint32))
    used = np.unique(triangles[up].ravel())
    remap = {int(old): new for new, old in enumerate(used)}
    compact = np.array(
        [[remap[int(i)] for i in triangle] for triangle in triangles[up]],
        dtype=np.uint32,
    )
    return TerrainSurface(vertices[used], compact)
