# peaks-path-20261116.debug.json

A virtual-editor debug scene exported live on 2026-11-16 (File > Export debug scene).

- **Source terrain:** an Archicad Mesh, level at -0.2 m on a 6.9 m grid, with two raised
  points: an inner peak at (17.549, 10.999, 1.018) and a corner at (-3.043, -3.244, 0.988).
- **Design:** one path (id 1) graded by its outline. The scene was exported before a build, so
  it has no outcome.

**Used by:** `EditRegionTerrain.SignatureLinesKeepTheSourceTerrainsPeaks177`. The test replays
the scene through an editor session (capture, build, Signature plan). It checks that the build
leaves both peaks untouched, and that the planned Archicad Mesh keeps each one as a point of a
line or of its outline.

**What it caught (#177):** the first Signature plan ringed the inner peak with contours, so the
created Mesh lost its top.
