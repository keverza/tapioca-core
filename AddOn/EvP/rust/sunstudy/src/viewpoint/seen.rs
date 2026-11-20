//! A sphere viewpoint's seen surfaces: the parts of the snapshot's triangles
//! the eye reaches (#83; port of `SeenSurfaces` in `ViewpointStudy.cpp`).
//!
//! Each triangle whose box comes within the radius carries an n x n
//! barycentric grid. A grid corner is seen when it lies within the radius and
//! the elevation band and the ray to it arrives. A cell seen at every corner is
//! kept whole; a cell seen at some is cut at the crossing on each mixed edge,
//! found by bisecting that edge to [`CROSSING_METRES`]. The cell side starts
//! from the radius and coarsens until the run fits [`MAX_SURFACE_CELLS`].
//!
//! ⚠️ CORNERS AND CROSSINGS, NOT CELL CENTRES. Keeping whole cells by their
//! centre stepped a patch's border cell by cell along the radius and every
//! shadow; a crossing puts it where the radius, the band or the shadow falls.
//!
//! Whether a point is seen depends on that point alone, so the corners of a
//! run of triangles go to the host as one batch and every edge still being
//! bisected takes one ray per round. The C++ pass asked the same questions one
//! at a time; the answers, the output order and the area sum are the same.

use super::{RADIANS, ViewpointError, ViewpointMode, ViewpointOptions, context_mask, validate};
use crate::raster::{cpp_max, cpp_min};
use crate::rays::{FirstHitRay, OcclusionRay, RayError, RayHit, RayQueries};
use crate::series::count_to_f64;
use crate::surface_sampling::SamplingMesh;
use std::collections::HashMap;

/// Cells tested per run, whatever the scene; the cell coarsens until it fits.
pub const MAX_SURFACE_CELLS: usize = 120_000;
/// Cells per triangle edge, at most.
const MAX_CELL_SPLITS: usize = 64;
/// A ray stopping this near its target still reaches it, metres.
const SEEN_SLACK_METRES: f64 = 0.02;
/// How closely a patch's border is bisected, metres.
const CROSSING_METRES: f64 = 0.005;
/// Bisection rounds per edge, at most.
const CROSSING_ROUNDS: usize = 16;
/// Coarsening passes over the cell side, at most.
const CELL_PASSES: usize = 12;
/// Grid corners gathered before a batch goes to the host.
const BATCH_CORNERS: usize = 1 << 16;

type Point = [f64; 3];
type Triangle = [Point; 3];

/// The seen surfaces.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SeenSurfaces {
    /// Triangle corners, three per triangle.
    pub triangles: Vec<Point>,
    /// Their area, m^2.
    pub area: f64,
    /// The cell side the run settled on, metres; 0 off the sphere.
    pub cell_metres: f64,
}

fn distance(a: &Point, b: &Point) -> f64 {
    ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]))
        .sqrt()
}

#[expect(
    clippy::manual_midpoint,
    reason = "0.5 * (a + b) as in C++; f64::midpoint takes another path near subnormals"
)]
fn midpoint(a: &Point, b: &Point) -> Point {
    [
        0.5 * (a[0] + b[0]),
        0.5 * (a[1] + b[1]),
        0.5 * (a[2] + b[2]),
    ]
}

fn triangle_area(triangle: &Triangle) -> f64 {
    let [first, second, third] = triangle;
    let along = [
        second[0] - first[0],
        second[1] - first[1],
        second[2] - first[2],
    ];
    let across = [
        third[0] - first[0],
        third[1] - first[1],
        third[2] - first[2],
    ];
    let normal = [
        along[1] * across[2] - along[2] * across[1],
        along[2] * across[0] - along[0] * across[2],
        along[0] * across[1] - along[1] * across[0],
    ];
    0.5 * (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]).sqrt()
}

/// C++ `std::max({a, b, c})`: the first of the largest.
fn max3(a: f64, b: f64, c: f64) -> f64 {
    cpp_max(cpp_max(a, b), c)
}

/// C++ `std::min({a, b, c})`: the first of the smallest.
fn min3(a: f64, b: f64, c: f64) -> f64 {
    cpp_min(cpp_min(a, b), c)
}

/// A face's corners, `None` when an index runs past the vertices.
fn corners_of(vertices: &[f64], face: &[u32; 3]) -> Option<Triangle> {
    let mut corners = [[0.0; 3]; 3];
    for (corner, index) in corners.iter_mut().zip(face) {
        let v = usize::try_from(*index).ok()?.checked_mul(3)?;
        *corner = [
            *vertices.get(v)?,
            *vertices.get(v + 1)?,
            *vertices.get(v + 2)?,
        ];
    }
    Some(corners)
}

/// How far `at` lies outside the span of three values, squared.
fn outside_squared(values: [f64; 3], at: f64) -> f64 {
    let low = min3(values[0], values[1], values[2]);
    let high = max3(values[0], values[1], values[2]);
    let outside = max3(low - at, at - high, 0.0);
    outside * outside
}

/// Squared distance from the point to the triangle's bounding box.
fn box_gap(t: &Triangle, point: &Point) -> f64 {
    outside_squared([t[0][0], t[1][0], t[2][0]], point[0])
        + outside_squared([t[0][1], t[1][1], t[2][1]], point[1])
        + outside_squared([t[0][2], t[1][2], t[2][2]], point[2])
}

/// The triangles whose bounding box comes within the radius, in mesh order.
fn near_triangles(meshes: &[SamplingMesh<'_>], options: &ViewpointOptions) -> Vec<Triangle> {
    let reach = options.radius * options.radius;
    meshes
        .iter()
        .flat_map(|mesh| {
            mesh.triangles
                .as_chunks::<3>()
                .0
                .iter()
                .filter_map(|face| corners_of(mesh.vertices, face))
        })
        .filter(|t| box_gap(t, &options.point) <= reach)
        .collect()
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "mirrors size_t(std::ceil(x)) of a non-negative length ratio"
)]
fn splits(t: &Triangle, cell: f64) -> usize {
    let longest = max3(
        distance(&t[0], &t[1]),
        distance(&t[1], &t[2]),
        distance(&t[2], &t[0]),
    );
    ((longest / cell).ceil() as usize).clamp(1, MAX_CELL_SPLITS)
}

/// The requested cell side, or one from the radius, coarsened by half again
/// until the run's cells fit the budget (at most [`CELL_PASSES`] times).
fn cell_side(near: &[Triangle], radius: f64, requested: f64) -> f64 {
    let mut cell = if requested > 0.0 {
        requested
    } else {
        cpp_min(1.0, cpp_max(0.1, radius / 80.0))
    };
    for _ in 0..CELL_PASSES {
        let cells: usize = near.iter().map(|t| splits(t, cell).pow(2)).sum();
        if cells <= MAX_SURFACE_CELLS {
            break;
        }
        cell *= 1.5;
    }
    cell
}

/// What one point needs: an answer already, or a ray (direction, reach).
enum Verdict {
    Known(bool),
    Ray(Point, f64),
}

/// The seen test: within the radius and band, and reached by the ray to it.
struct Sight<'a> {
    eye: Point,
    radius: f64,
    sin_low: f64,
    sin_high: f64,
    /// Context mesh mask; rays stop only at Context.
    mask: &'a [u8],
    /// Every mesh is Context, so any hit blocks (no mask needed).
    everything: bool,
    /// Context was named, so rays are cast at all.
    traced: bool,
    queries: &'a dyn RayQueries,
}

fn ray_failure(error: RayError) -> ViewpointError {
    match error {
        RayError::Cancelled => ViewpointError::Cancelled,
        RayError::Failed => ViewpointError::NoIndex,
    }
}

impl Sight<'_> {
    fn verdict(&self, at: &Point) -> Verdict {
        let d = distance(&self.eye, at);
        if d > self.radius {
            return Verdict::Known(false);
        }
        if d < 1.0e-9 {
            return Verdict::Known(true);
        }
        let e = self.eye;
        let dir = [(at[0] - e[0]) / d, (at[1] - e[1]) / d, (at[2] - e[2]) / d];
        if dir[2] < self.sin_low - 1.0e-12 || dir[2] > self.sin_high + 1.0e-12 {
            return Verdict::Known(false);
        }
        if !self.traced || d <= SEEN_SLACK_METRES {
            return Verdict::Known(true);
        }
        Verdict::Ray(dir, d - SEEN_SLACK_METRES)
    }

    /// Whether each point is seen; one host batch for every point needing a ray.
    fn seen(&self, points: &[Point]) -> Result<Vec<bool>, ViewpointError> {
        let verdicts: Vec<Verdict> = points.iter().map(|at| self.verdict(at)).collect();
        let rays: Vec<(Point, f64)> = verdicts
            .iter()
            .filter_map(|verdict| match verdict {
                Verdict::Known(_) => None,
                Verdict::Ray(dir, reach) => Some((*dir, *reach)),
            })
            .collect();
        let mut reached = if rays.is_empty() {
            Vec::new()
        } else {
            self.trace(&rays)?
        }
        .into_iter();
        Ok(verdicts
            .iter()
            .map(|verdict| match verdict {
                Verdict::Known(answer) => *answer,
                Verdict::Ray(..) => reached.next().unwrap_or(false),
            })
            .collect())
    }

    fn trace(&self, rays: &[(Point, f64)]) -> Result<Vec<bool>, ViewpointError> {
        if self.everything {
            let casts: Vec<OcclusionRay> = rays
                .iter()
                .map(|&(dir, reach)| OcclusionRay {
                    origin: self.eye,
                    dir,
                    tmin: 0.0,
                    tmax: reach,
                })
                .collect();
            let mut blocked = vec![0_u8; casts.len()];
            self.queries
                .occlude_rays(&casts, &mut blocked, 0)
                .map_err(ray_failure)?;
            return Ok(blocked.iter().map(|b| *b == 0).collect());
        }
        let casts: Vec<FirstHitRay> = rays
            .iter()
            .map(|&(dir, reach)| FirstHitRay {
                origin: self.eye,
                dir,
                length: reach,
            })
            .collect();
        let mut hits = vec![RayHit::default(); casts.len()];
        self.queries
            .first_hits(&casts, Some(self.mask), &mut hits, 0)
            .map_err(ray_failure)?;
        Ok(hits.iter().map(|h| !h.hit).collect())
    }
}

/// `t0 + (t1 - t0) i / n + (t2 - t0) j / n`, evaluated in the C++ order.
fn lerp(triangle: &Triangle, row: f64, column: f64, span: f64) -> Point {
    let [base, along, across] = triangle;
    let axis = |k: usize| -> f64 {
        let at = |p: &Point| p.get(k).copied().unwrap_or_default();
        at(base) + (at(along) - at(base)) * row / span + (at(across) - at(base)) * column / span
    };
    [axis(0), axis(1), axis(2)]
}

/// A cell's three edges, in C++ order.
fn cell_edges(cell: [usize; 3]) -> [(usize, usize); 3] {
    [(cell[0], cell[1]), (cell[1], cell[2]), (cell[2], cell[0])]
}

/// One triangle's barycentric grid: corner (i, j) stored at i (n + 1) + j.
struct Grid {
    n: usize,
    corners: Vec<Point>,
    visible: Vec<bool>,
}

impl Grid {
    fn new(t: &Triangle, n: usize) -> Self {
        let side = n + 1;
        let mut corners = vec![[0.0; 3]; side * side];
        let span = count_to_f64(n);
        for (row, column) in Self::valid(n) {
            if let Some(at) = corners.get_mut(row * side + column) {
                *at = lerp(t, count_to_f64(row), count_to_f64(column), span);
            }
        }
        Self {
            n,
            corners,
            visible: vec![false; side * side],
        }
    }

    /// The grid's corners inside the triangle, in C++ order.
    fn valid(n: usize) -> impl Iterator<Item = (usize, usize)> {
        (0..=n).flat_map(move |row| (0..=n - row).map(move |column| (row, column)))
    }

    fn indices(&self) -> impl Iterator<Item = usize> + '_ {
        let side = self.n + 1;
        Self::valid(self.n).map(move |(row, column)| row * side + column)
    }

    fn corner(&self, k: usize) -> Point {
        self.corners.get(k).copied().unwrap_or_default()
    }

    fn sees(&self, k: usize) -> bool {
        self.visible.get(k).copied().unwrap_or(false)
    }

    fn whole(&self) -> bool {
        self.indices().all(|k| self.sees(k))
    }

    /// The cells as corner-index triples, in C++ order.
    fn cells(&self) -> Vec<[usize; 3]> {
        let (n, side) = (self.n, self.n + 1);
        let mut cells = Vec::with_capacity(n * n);
        for row in 0..n {
            for column in 0..n - row {
                let here = row * side + column;
                cells.push([here, here + side, here + 1]);
                if row + column + 1 < n {
                    cells.push([here + side, here + side + 1, here + 1]);
                }
            }
        }
        cells
    }
}

/// A mixed edge being bisected: `from` seen, `to` not.
struct Edge {
    from: Point,
    to: Point,
}

fn edge_key(a: usize, b: usize) -> u64 {
    let low = u64::try_from(a.min(b)).unwrap_or(u64::MAX);
    let high = u64::try_from(a.max(b)).unwrap_or(u64::MAX);
    (low << 32) | high
}

/// Every mixed edge of a cut grid, once, keyed to its place in `edges`.
fn mixed_edges(grid: &Grid, edges: &mut Vec<Edge>) -> HashMap<u64, usize> {
    let mut keys = HashMap::new();
    for cell in grid.cells() {
        for (a, b) in cell_edges(cell) {
            if grid.sees(a) == grid.sees(b) {
                continue;
            }
            keys.entry(edge_key(a, b)).or_insert_with(|| {
                let (seen, unseen) = if grid.sees(a) { (a, b) } else { (b, a) };
                edges.push(Edge {
                    from: grid.corner(seen),
                    to: grid.corner(unseen),
                });
                edges.len() - 1
            });
        }
    }
    keys
}

/// Where each edge's seen and unseen ends change over, to [`CROSSING_METRES`].
fn bisect(sight: &Sight<'_>, edges: &mut [Edge]) -> Result<Vec<Point>, ViewpointError> {
    for _ in 0..CROSSING_ROUNDS {
        let active: Vec<&mut Edge> = edges
            .iter_mut()
            .filter(|e| distance(&e.from, &e.to) > CROSSING_METRES)
            .collect();
        if active.is_empty() {
            break;
        }
        let middles: Vec<Point> = active.iter().map(|e| midpoint(&e.from, &e.to)).collect();
        let seen = sight.seen(&middles)?;
        for ((edge, middle), answer) in active.into_iter().zip(middles).zip(seen) {
            if answer {
                edge.from = middle;
            } else {
                edge.to = middle;
            }
        }
    }
    Ok(edges.iter().map(|e| midpoint(&e.from, &e.to)).collect())
}

/// The seen part of each cell: its seen corners and a crossing on each mixed
/// edge (three or four corners), fanned from the first.
fn cut(grid: &Grid, keys: &HashMap<u64, usize>, crossings: &[Point], out: &mut SeenSurfaces) {
    let mut polygon: Vec<Point> = Vec::with_capacity(4);
    for cell in grid.cells() {
        polygon.clear();
        for (a, b) in cell_edges(cell) {
            if grid.sees(a) {
                polygon.push(grid.corner(a));
            }
            if grid.sees(a) != grid.sees(b) {
                let crossing = keys.get(&edge_key(a, b)).and_then(|e| crossings.get(*e));
                polygon.push(crossing.copied().unwrap_or_default());
            }
        }
        let Some((first, rest)) = polygon.split_first() else {
            continue;
        };
        for pair in rest.windows(2) {
            if let [second, third] = pair {
                let piece = [*first, *second, *third];
                out.triangles.extend_from_slice(&piece);
                out.area += triangle_area(&piece);
            }
        }
    }
}

/// One batch of triangles: their corners in one host call, their mixed edges
/// bisected together, then each triangle kept whole or cut, in order.
fn seen_batch(
    sight: &Sight<'_>,
    near: &[Triangle],
    cell: f64,
    out: &mut SeenSurfaces,
) -> Result<(), ViewpointError> {
    let mut grids: Vec<Grid> = near.iter().map(|t| Grid::new(t, splits(t, cell))).collect();
    let points: Vec<Point> = grids
        .iter()
        .flat_map(|g| g.indices().map(|k| g.corner(k)))
        .collect();
    let mut answers = sight.seen(&points)?.into_iter();
    for grid in &mut grids {
        let indices: Vec<usize> = grid.indices().collect();
        for k in indices {
            if let Some(visible) = grid.visible.get_mut(k) {
                *visible = answers.next().unwrap_or(false);
            }
        }
    }
    let mut edges = Vec::new();
    let keys: Vec<Option<HashMap<u64, usize>>> = grids
        .iter()
        .map(|g| (!g.whole()).then(|| mixed_edges(g, &mut edges)))
        .collect();
    let crossings = bisect(sight, &mut edges)?;
    for ((t, grid), keys) in near.iter().zip(&grids).zip(&keys) {
        if let Some(keys) = keys {
            cut(grid, keys, &crossings, out);
        } else {
            out.triangles.extend_from_slice(t);
            out.area += triangle_area(t);
        }
    }
    Ok(())
}

/// The end of the batch starting at `start`: at least one triangle, then as
/// many as keep the batch under [`BATCH_CORNERS`].
fn batch_end(near: &[Triangle], start: usize, cell: f64) -> usize {
    let mut corners = 0;
    let mut end = start;
    for t in near.iter().skip(start) {
        let side = splits(t, cell) + 1;
        if end > start && corners + side * side > BATCH_CORNERS {
            break;
        }
        corners += side * side;
        end += 1;
    }
    end
}

/// The surfaces a sphere viewpoint sees; empty off the sphere.
/// `cell_metres` > 0 fixes the starting cell side, otherwise it follows the
/// radius. `meshes` are the snapshot's, in order.
///
/// # Errors
/// [`ViewpointError`]: the study's own refusals, a missing index, or
/// cancellation (polled before each batch and by the host during one).
pub fn seen_surfaces(
    meshes: &[SamplingMesh<'_>],
    context: &[&str],
    options: &ViewpointOptions,
    cell_metres: f64,
    queries: &dyn RayQueries,
) -> Result<SeenSurfaces, ViewpointError> {
    validate(options)?;
    if options.mode != ViewpointMode::Sphere {
        return Ok(SeenSurfaces::default());
    }
    let guids: Vec<&str> = meshes.iter().map(|m| m.guid).collect();
    let mask = context_mask(&guids, context)?;
    let traced = !context.is_empty();
    if traced {
        queries.prepare().map_err(|_| ViewpointError::NoIndex)?;
    }
    let near = near_triangles(meshes, options);
    let cell = cell_side(&near, options.radius, cell_metres);
    let sight = Sight {
        eye: options.point,
        radius: options.radius,
        sin_low: (options.min_elevation_degrees * RADIANS).sin(),
        sin_high: (options.max_elevation_degrees * RADIANS).sin(),
        mask: &mask,
        everything: mask.iter().all(|on| *on != 0),
        traced,
        queries,
    };
    let mut out = SeenSurfaces {
        cell_metres: cell,
        ..SeenSurfaces::default()
    };
    let mut start = 0;
    while start < near.len() {
        if queries.cancelled() {
            return Err(ViewpointError::Cancelled);
        }
        let end = batch_end(&near, start, cell);
        seen_batch(&sight, near.get(start..end).unwrap_or(&[]), cell, &mut out)?;
        start = end;
    }
    Ok(out)
}

#[cfg(test)]
mod tests;
