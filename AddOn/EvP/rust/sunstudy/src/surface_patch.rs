//! The analysis domain above the renderer's triangles.
//!
//! Port of `SunStudy/SurfacePatch.cpp`. A patch is the coplanar, edge-connected
//! run of source triangles one BIM surface was tessellated into; gridding it
//! once in its own plane gives a regular lattice whatever the triangulation.
//! Adjacency is by SOURCE vertex index (never welded render vertices); patches
//! never cross a group (element); coplanarity is tested against the seed, not
//! the neighbour, so a tessellated curve is not walked into one flat patch.

use crate::raster::{Vec3, cpp_max, cpp_min};
use crate::sampler::triangle_normal;
use crate::series::count_to_f64;
use std::collections::BTreeMap;

/// One coplanar, edge-connected run of source triangles within one group.
#[derive(Clone, Debug, PartialEq)]
pub struct SurfacePatch {
    /// The caller's group (element) id; patches never cross it.
    pub group: u32,
    /// Source triangle indices, ascending, never empty.
    pub triangles: Vec<u32>,
    /// The seed triangle's unit normal.
    pub normal: Vec3,
    /// Minimum corner of the patch in its own frame.
    pub origin: Vec3,
    /// Unit u axis (along the first triangle's first edge, in-plane).
    pub u_axis: Vec3,
    /// Unit v axis (`normal x u_axis`).
    pub v_axis: Vec3,
    /// Extent along u from `origin`, metres.
    pub u_extent: f64,
    /// Extent along v from `origin`, metres.
    pub v_extent: f64,
    /// Summed area of the source triangles, m^2.
    pub area: f64,
}

/// Coplanarity tolerances.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SurfacePatchOptions {
    /// Largest angle between normals of one surface, degrees.
    pub coplanar_degrees: f64,
    /// Largest centroid offset from the seed plane, metres.
    pub plane_tolerance_m: f64,
}

impl Default for SurfacePatchOptions {
    fn default() -> Self {
        Self {
            coplanar_degrees: 1.0,
            plane_tolerance_m: 1e-3,
        }
    }
}

fn sub(a: &Vec3, b: &Vec3) -> Vec3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn dot(a: &Vec3, b: &Vec3) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn cross(a: &Vec3, b: &Vec3) -> Vec3 {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn normalise(v: &mut Vec3) -> bool {
    let length = dot(v, v).sqrt();
    if length.partial_cmp(&1e-12) != Some(std::cmp::Ordering::Greater) {
        return false;
    }
    for c in v.iter_mut() {
        *c /= length;
    }
    true
}

pub(crate) fn vertex_at(vertices: &[f64], index: u32) -> Option<Vec3> {
    let start = usize::try_from(index).ok()?.checked_mul(3)?;
    match vertices.get(start..start.checked_add(3)?)? {
        [x, y, z] => Some([*x, *y, *z]),
        _ => None,
    }
}

pub(crate) fn corners(triangles: &[u32], face: u32) -> Option<[u32; 3]> {
    let start = usize::try_from(face).ok()?.checked_mul(3)?;
    match triangles.get(start..start.checked_add(3)?)? {
        [a, b, c] => Some([*a, *b, *c]),
        _ => None,
    }
}

#[derive(Clone, Copy)]
struct Facet {
    normal: Vec3,
    centroid: Vec3,
    area: f64,
    group: u32,
}

fn same_surface(a: &Facet, b: &Facet, cos_tolerance: f64, plane_tolerance: f64) -> bool {
    // Not abs'd: the two faces of a thin slab are antiparallel, two surfaces.
    a.group == b.group
        && dot(&a.normal, &b.normal).partial_cmp(&cos_tolerance) != Some(std::cmp::Ordering::Less)
        && dot(&sub(&b.centroid, &a.centroid), &a.normal).abs() <= plane_tolerance
}

fn facet(vertices: &[f64], triangles: &[u32], face: u32, group: u32) -> Option<Facet> {
    let [ia, ib, ic] = corners(triangles, face)?;
    let (a, b, c) = (
        vertex_at(vertices, ia)?,
        vertex_at(vertices, ib)?,
        vertex_at(vertices, ic)?,
    );
    let (normal, area) = triangle_normal(&a, &b, &c);
    if area.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return None; // a sliver's normal is noise
    }
    Some(Facet {
        normal,
        centroid: [
            (a[0] + b[0] + c[0]) / 3.0,
            (a[1] + b[1] + c[1]) / 3.0,
            (a[2] + b[2] + c[2]) / 3.0,
        ],
        area,
        group,
    })
}

fn edge_key(a: u32, b: u32) -> (u32, u32) {
    if a < b { (a, b) } else { (b, a) }
}

fn face_u32(face: usize) -> u32 {
    u32::try_from(face).unwrap_or(u32::MAX)
}

/// The patch's frame from its lowest-numbered triangle's first edge, and its
/// minimum-corner origin and extents.
fn frame(patch: &mut SurfacePatch, vertices: &[f64], triangles: &[u32]) {
    let normal = patch.normal;
    let first = patch.triangles.first().copied().unwrap_or(0);
    let Some([ia, ib, _]) = corners(triangles, first) else {
        return;
    };
    let (Some(first_corner), Some(second_corner)) =
        (vertex_at(vertices, ia), vertex_at(vertices, ib))
    else {
        return;
    };
    let mut u_axis = sub(&second_corner, &first_corner);
    let along = dot(&u_axis, &normal);
    u_axis = [
        u_axis[0] - normal[0] * along,
        u_axis[1] - normal[1] * along,
        u_axis[2] - normal[2] * along,
    ];
    if !normalise(&mut u_axis) {
        // Unreachable for a non-degenerate triangle; a fallback beats NaN.
        let fallback = if normal[2].abs() < 0.9 {
            [0.0, 0.0, 1.0]
        } else {
            [1.0, 0.0, 0.0]
        };
        let along_fallback = dot(&fallback, &normal);
        let mut projected = sub(
            &fallback,
            &[
                normal[0] * along_fallback,
                normal[1] * along_fallback,
                normal[2] * along_fallback,
            ],
        );
        normalise(&mut projected);
        u_axis = projected;
    }
    let mut v_axis = cross(&normal, &u_axis);
    normalise(&mut v_axis);

    let mut bounds: Option<[f64; 4]> = None;
    for &face in &patch.triangles {
        for index in corners(triangles, face).into_iter().flatten() {
            let Some(p) = vertex_at(vertices, index) else {
                continue;
            };
            let relative = sub(&p, &first_corner);
            let (pu, pv) = (dot(&relative, &u_axis), dot(&relative, &v_axis));
            bounds = Some(match bounds {
                None => [pu, pu, pv, pv],
                Some([min_u, max_u, min_v, max_v]) => [
                    cpp_min(min_u, pu),
                    cpp_max(max_u, pu),
                    cpp_min(min_v, pv),
                    cpp_max(max_v, pv),
                ],
            });
        }
    }
    let [min_u, max_u, min_v, max_v] = bounds.unwrap_or([0.0; 4]);
    patch.origin = [
        first_corner[0] + u_axis[0] * min_u + v_axis[0] * min_v,
        first_corner[1] + u_axis[1] * min_u + v_axis[1] * min_v,
        first_corner[2] + u_axis[2] * min_u + v_axis[2] * min_v,
    ];
    patch.u_extent = max_u - min_u;
    patch.v_extent = max_v - min_v;
    patch.u_axis = u_axis;
    patch.v_axis = v_axis;
}

/// Flood-fill state shared by every seed of one build.
struct Flood<'a> {
    triangles: &'a [u32],
    facets: &'a [Option<Facet>],
    edges: &'a BTreeMap<(u32, u32), Vec<u32>>,
    partition: Option<&'a [u8]>,
    cos_tolerance: f64,
    plane_tolerance: f64,
    taken: Vec<bool>,
}

impl Flood<'_> {
    fn partition_differs(&self, seed: usize, other: usize) -> bool {
        self.partition
            .is_some_and(|p| matches!((p.get(seed), p.get(other)), (Some(x), Some(y)) if x != y))
    }

    /// Whether `neighbour` joins the seed's patch; marks it taken if so.
    fn accept(&mut self, seed: usize, seed_facet: &Facet, neighbour: u32) -> bool {
        let n = usize::try_from(neighbour).unwrap_or(usize::MAX);
        if self.taken.get(n).copied().unwrap_or(true) || self.partition_differs(seed, n) {
            return false;
        }
        // Compared against the SEED, never the face we arrived from.
        let joins = self.facets.get(n).copied().flatten().is_some_and(|other| {
            same_surface(seed_facet, &other, self.cos_tolerance, self.plane_tolerance)
        });
        if let Some(slot) = self.taken.get_mut(n).filter(|_| joins) {
            *slot = true;
        }
        joins
    }

    /// The seed's patch triangles in discovery order (the caller sorts).
    fn fill(&mut self, seed: usize, seed_facet: &Facet) -> Vec<u32> {
        if let Some(slot) = self.taken.get_mut(seed) {
            *slot = true;
        }
        let mut triangles = Vec::new();
        let mut stack = vec![face_u32(seed)];
        while let Some(face) = stack.pop() {
            triangles.push(face);
            let Some([a, b, c]) = corners(self.triangles, face) else {
                continue;
            };
            // Edges in winding order, each edge's faces ascending, as in C++.
            let neighbours: Vec<u32> = [edge_key(a, b), edge_key(b, c), edge_key(c, a)]
                .iter()
                .flat_map(|key| self.edges.get(key).into_iter().flatten().copied())
                .collect();
            let accepted: Vec<u32> = neighbours
                .into_iter()
                .filter(|n| self.accept(seed, seed_facet, *n))
                .collect();
            stack.extend(accepted);
        }
        triangles
    }
}

/// Group a soup into patches. `groups` is one id per face or `None`;
/// `face_partition`, when given, splits patches wherever two faces' bytes
/// differ (a receiver/context boundary). Degenerate and out-of-range faces join
/// no patch. Seeds run in ascending face order and each patch's triangles are
/// sorted, so the result is deterministic.
#[must_use]
pub fn build_surface_patches(
    vertices: &[f64],
    triangles: &[u32],
    groups: Option<&[u32]>,
    options: &SurfacePatchOptions,
    face_partition: Option<&[u8]>,
) -> Vec<SurfacePatch> {
    let face_count = triangles.len() / 3;
    if vertices.len() < 3 || face_count == 0 {
        return Vec::new();
    }
    let cos_tolerance = (options.coplanar_degrees * std::f64::consts::PI / 180.0).cos();
    let facets: Vec<Option<Facet>> = (0..face_count)
        .map(|face| {
            let group = groups.and_then(|g| g.get(face).copied()).unwrap_or(0);
            facet(vertices, triangles, face_u32(face), group)
        })
        .collect();

    let mut edges: BTreeMap<(u32, u32), Vec<u32>> = BTreeMap::new();
    for (face, corners_of) in triangles.as_chunks::<3>().0.iter().enumerate() {
        if facets.get(face).is_some_and(Option::is_some) {
            let [a, b, c] = *corners_of;
            for key in [edge_key(a, b), edge_key(b, c), edge_key(c, a)] {
                edges.entry(key).or_default().push(face_u32(face));
            }
        }
    }

    let mut flood = Flood {
        triangles,
        facets: &facets,
        edges: &edges,
        partition: face_partition,
        cos_tolerance,
        plane_tolerance: options.plane_tolerance_m,
        taken: vec![false; face_count],
    };
    let mut patches = Vec::new();
    for seed in 0..face_count {
        let Some(Some(seed_facet)) = facets.get(seed).copied() else {
            continue;
        };
        if flood.taken.get(seed).copied().unwrap_or(true) {
            continue;
        }
        let mut patch = SurfacePatch {
            group: seed_facet.group,
            triangles: flood.fill(seed, &seed_facet),
            normal: seed_facet.normal,
            origin: [0.0; 3],
            u_axis: [1.0, 0.0, 0.0],
            v_axis: [0.0, 1.0, 0.0],
            u_extent: 0.0,
            v_extent: 0.0,
            area: 0.0,
        };
        patch.triangles.sort_unstable();
        // Summed in ascending triangle order, as the C++ loop does.
        for &face in &patch.triangles {
            if let Some(Some(f)) = facets.get(usize::try_from(face).unwrap_or(usize::MAX)) {
                patch.area += f.area;
            }
        }
        frame(&mut patch, vertices, triangles);
        patches.push(patch);
    }
    patches
}

/// One covered cell of a patch lattice.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PatchCell {
    /// Lattice column.
    pub column: u32,
    /// Lattice row.
    pub row: u32,
    /// World point measured: the area-weighted centroid of the covered part.
    pub centre: Vec3,
    /// Covered area, m^2 (spacing^2 only for interior cells).
    pub area: f64,
    /// True when the patch boundary does not cut the cell.
    pub interior: bool,
}

/// The regular lattice over one patch and its covered cells.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PatchGrid {
    /// Lattice columns.
    pub columns: u32,
    /// Lattice rows.
    pub rows: u32,
    /// Covered cells in row-major order.
    pub cells: Vec<PatchCell>,
    /// The patch was below the grid and emitted its centroid.
    pub from_centroid: bool,
}

type Poly2D = Vec<[f64; 2]>;

/// Sutherland-Hodgman against one axis-aligned half-plane.
fn clip_half_plane(input: &[[f64; 2]], axis: usize, limit: f64, keep_below: bool) -> Poly2D {
    let mut out = Vec::with_capacity(input.len() + 1);
    let inside = |p: &[f64; 2]| {
        let value = p.get(axis).copied().unwrap_or(f64::NAN);
        if keep_below {
            value <= limit
        } else {
            value >= limit
        }
    };
    for (i, a) in input.iter().enumerate() {
        let Some(b) = input.get((i + 1) % input.len()) else {
            continue;
        };
        let (ina, inb) = (inside(a), inside(b));
        if ina {
            out.push(*a);
        }
        if ina == inb {
            continue;
        }
        let (along_a, along_b) = if axis == 0 {
            (a[0], b[0])
        } else {
            (a[1], b[1])
        };
        let denominator = along_b - along_a;
        if denominator.abs() < 1e-15 {
            continue;
        }
        let t = (limit - along_a) / denominator;
        out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]);
    }
    out
}

/// Unsigned area and centroid of a simple polygon (0 when degenerate).
fn polygon_area_centroid(poly: &[[f64; 2]]) -> (f64, f64, f64) {
    if poly.len() < 3 {
        return (0.0, 0.0, 0.0);
    }
    let (mut twice, mut cx, mut cy) = (0.0, 0.0, 0.0);
    for (i, p) in poly.iter().enumerate() {
        let Some(q) = poly.get((i + 1) % poly.len()) else {
            continue;
        };
        let cross = p[0] * q[1] - q[0] * p[1];
        twice += cross;
        cx += (p[0] + q[0]) * cross;
        cy += (p[1] + q[1]) * cross;
    }
    if twice.abs() < 1e-18 {
        return (0.0, 0.0, 0.0);
    }
    (twice.abs() * 0.5, cx / (3.0 * twice), cy / (3.0 * twice))
}

fn clip_triangle_to_cell(
    triangle: &[[f64; 2]; 3],
    min: [f64; 2],
    max: [f64; 2],
) -> (f64, f64, f64) {
    let left = clip_half_plane(triangle, 0, min[0], false);
    let right = clip_half_plane(&left, 0, max[0], true);
    let bottom = clip_half_plane(&right, 1, min[1], false);
    let top = clip_half_plane(&bottom, 1, max[1], true);
    polygon_area_centroid(&top)
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "mirrors static_cast<uint32_t>(max(1.0, ceil(extent / spacing)))"
)]
fn lattice_size(extent: f64, spacing: f64) -> u32 {
    cpp_max(1.0, (extent / spacing).ceil()) as u32
}

/// Rasterise a patch: each cell is clipped against the patch's own triangles
/// and the pieces summed (they partition the surface, so no union polygon is
/// needed). A patch with no covered cell emits its centroid.
#[must_use]
pub fn build_patch_grid(
    patch: &SurfacePatch,
    vertices: &[f64],
    triangles: &[u32],
    spacing: f64,
) -> PatchGrid {
    let mut grid = PatchGrid::default();
    if vertices.is_empty()
        || triangles.is_empty()
        || patch.triangles.is_empty()
        || spacing.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater)
    {
        return grid;
    }
    let (origin, u_axis, v_axis) = (patch.origin, patch.u_axis, patch.v_axis);
    let flat: Vec<[[f64; 2]; 3]> = patch
        .triangles
        .iter()
        .filter_map(|&face| corners(triangles, face))
        .map(|idx| {
            idx.map(|i| {
                let p = vertex_at(vertices, i).unwrap_or([f64::NAN; 3]);
                let relative = sub(&p, &origin);
                [dot(&relative, &u_axis), dot(&relative, &v_axis)]
            })
        })
        .collect();
    grid.columns = lattice_size(patch.u_extent, spacing);
    grid.rows = lattice_size(patch.v_extent, spacing);
    let world = |pu: f64, pv: f64| {
        [
            origin[0] + u_axis[0] * pu + v_axis[0] * pv,
            origin[1] + u_axis[1] * pu + v_axis[1] * pv,
            origin[2] + u_axis[2] * pu + v_axis[2] * pv,
        ]
    };
    let full = spacing * spacing;
    let columns = grid.columns;
    let cells = (0..grid.rows).flat_map(|row| (0..columns).map(move |column| (row, column)));
    for (row, column) in cells {
        let min = [f64::from(column) * spacing, f64::from(row) * spacing];
        let max = [min[0] + spacing, min[1] + spacing];
        let (mut area, mut weighted_u, mut weighted_v) = (0.0, 0.0, 0.0);
        for t in &flat {
            let (piece, cx, cy) = clip_triangle_to_cell(t, min, max);
            if piece <= 0.0 {
                continue;
            }
            area += piece;
            weighted_u += cx * piece;
            weighted_v += cy * piece;
        }
        if area.partial_cmp(&1e-12) != Some(std::cmp::Ordering::Greater) {
            continue; // an uncovered cell emits nothing
        }
        grid.cells.push(PatchCell {
            column,
            row,
            centre: world(weighted_u / area, weighted_v / area),
            area,
            interior: area >= full * (1.0 - 1e-9),
        });
    }
    if grid.cells.is_empty() {
        let (mut cu, mut cv) = (0.0, 0.0);
        let mut count = 0_usize;
        for p in flat.iter().flatten() {
            cu += p[0];
            cv += p[1];
            count += 1;
        }
        if count > 0 {
            let n = count_to_f64(count);
            grid.cells.push(PatchCell {
                column: 0,
                row: 0,
                centre: world(cu / n, cv / n),
                area: patch.area,
                interior: false,
            });
            grid.from_centroid = true;
        }
    }
    grid
}

#[cfg(test)]
mod tests;
