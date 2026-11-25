//! Mirrors the cases of `tests/cpp/test_surfacepatch.cpp`.

use super::*;

/// A w x h rectangle in the z = 0 plane split into `strips` x 2 triangles.
fn rectangle(width: f64, height: f64, strips: u32) -> (Vec<f64>, Vec<u32>) {
    let mut vertices = Vec::new();
    for i in 0..=strips {
        let x = width * f64::from(i) / f64::from(strips);
        vertices.extend_from_slice(&[x, 0.0, 0.0, x, height, 0.0]);
    }
    let mut triangles = Vec::new();
    for i in 0..strips {
        let (low, next, next_top, top) = (2 * i, 2 * i + 2, 2 * i + 3, 2 * i + 1);
        triangles.extend_from_slice(&[low, next, next_top, low, next_top, top]);
    }
    (vertices, triangles)
}

fn patches(vertices: &[f64], triangles: &[u32], groups: Option<&[u32]>) -> Vec<SurfacePatch> {
    build_surface_patches(
        vertices,
        triangles,
        groups,
        &SurfacePatchOptions::default(),
        None,
    )
}

fn total(grid: &PatchGrid) -> f64 {
    grid.cells.iter().map(|c| c.area).sum()
}

#[test]
fn two_coplanar_triangles_are_one_surface() {
    let (v, t) = rectangle(4.0, 3.0, 1);
    let p = patches(&v, &t, None);
    assert_eq!(p.len(), 1);
    let first = p.first().cloned().unwrap_or_else(unreachable_patch);
    assert_eq!(first.triangles.len(), 2);
    assert!((first.area - 12.0).abs() < 1e-9);
    assert!((first.u_extent - 4.0).abs() < 1e-9 && (first.v_extent - 3.0).abs() < 1e-9);
}

fn unreachable_patch() -> SurfacePatch {
    SurfacePatch {
        group: u32::MAX,
        triangles: Vec::new(),
        normal: [0.0; 3],
        origin: [0.0; 3],
        u_axis: [0.0; 3],
        v_axis: [0.0; 3],
        u_extent: 0.0,
        v_extent: 0.0,
        area: f64::NAN,
    }
}

fn only(vertices: &[f64], triangles: &[u32]) -> SurfacePatch {
    let p = patches(vertices, triangles, None);
    assert_eq!(p.len(), 1);
    p.first().cloned().unwrap_or_else(unreachable_patch)
}

#[test]
fn twenty_coplanar_triangles_are_still_one_surface() {
    let (v, t) = rectangle(10.0, 2.0, 10);
    let p = only(&v, &t);
    assert_eq!(p.triangles.len(), 20);
    assert!((p.area - 20.0).abs() < 1e-9);
}

#[test]
fn one_lattice_covers_the_rectangle_with_no_diagonal_seam() {
    let (v, t) = rectangle(4.0, 3.0, 1);
    let grid = build_patch_grid(&only(&v, &t), &v, &t, 1.0);
    assert_eq!((grid.columns, grid.rows, grid.cells.len()), (4, 3, 12));
    assert!(!grid.from_centroid);
    assert!(
        grid.cells
            .iter()
            .all(|c| c.interior && (c.area - 1.0).abs() < 1e-9)
    );
}

#[test]
fn the_lattice_is_independent_of_the_triangulation() {
    let (v2, t2) = rectangle(10.0, 2.0, 1);
    let (v20, t20) = rectangle(10.0, 2.0, 10);
    let a = build_patch_grid(&only(&v2, &t2), &v2, &t2, 1.0);
    let b = build_patch_grid(&only(&v20, &t20), &v20, &t20, 1.0);
    assert_eq!(a.cells.len(), 20);
    assert_eq!(a.cells.len(), b.cells.len());
    for (x, y) in a.cells.iter().zip(&b.cells) {
        assert_eq!((x.column, x.row), (y.column, y.row));
        assert!((x.area - y.area).abs() < 1e-9);
        assert!(
            x.centre
                .iter()
                .zip(&y.centre)
                .all(|(p, q)| (p - q).abs() < 1e-9)
        );
    }
}

#[test]
fn perpendicular_and_disconnected_pieces_are_separate_surfaces() {
    let vertices = [
        0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 2.0, 0.0, // floor
        0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 0.0, 2.0, // wall sharing an edge
        5.0, 0.0, 0.0, 7.0, 0.0, 0.0, 7.0, 2.0, 0.0, // coplanar, disconnected
    ];
    let triangles = [0, 1, 2, 0, 1, 5, 6, 7, 8];
    // Edge (0,1) is shared by index between floor and wall.
    assert_eq!(patches(&vertices, &triangles, None).len(), 3);
}

#[test]
fn two_elements_never_share_a_patch_even_when_flush() {
    let (v, t) = rectangle(4.0, 2.0, 1);
    let p = patches(&v, &t, Some(&[0, 1]));
    assert_eq!(p.len(), 2);
    assert_ne!(p.first().map(|x| x.group), p.get(1).map(|x| x.group));
}

#[test]
fn the_two_sides_of_a_slab_are_two_surfaces() {
    let vertices = [0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 2.0, 0.0];
    let triangles = [0, 1, 2, 0, 2, 1];
    assert_eq!(patches(&vertices, &triangles, None).len(), 2);
}

#[test]
fn the_cell_areas_sum_to_the_patch_area_at_every_spacing() {
    let (v, t) = rectangle(7.3, 2.9, 5);
    let p = only(&v, &t);
    for spacing in [0.25, 0.7, 1.0, 1.3, 4.0, 20.0] {
        assert!((total(&build_patch_grid(&p, &v, &t, spacing)) - p.area).abs() < 1e-9);
    }
}

#[test]
fn a_triangle_clips_to_exactly_half_the_square() {
    let vertices = [0.0, 0.0, 0.0, 4.0, 0.0, 0.0, 0.0, 4.0, 0.0];
    let triangles = [0, 1, 2];
    let p = only(&vertices, &triangles);
    assert!((p.area - 8.0).abs() < 1e-9);
    let grid = build_patch_grid(&p, &vertices, &triangles, 1.0);
    assert!((total(&grid) - 8.0).abs() < 1e-9);
    let interior = grid.cells.iter().filter(|c| c.interior).count();
    assert!(interior > 0 && interior < grid.cells.len());
}

#[test]
fn a_surface_below_the_grid_still_gets_a_sample() {
    let vertices = [0.0, 0.0, 0.0, 0.1, 0.0, 0.0, 0.1, 0.1, 0.0, 0.0, 0.1, 0.0];
    let triangles = [0, 1, 2, 0, 2, 3];
    let p = only(&vertices, &triangles);
    let grid = build_patch_grid(&p, &vertices, &triangles, 1.0);
    // A 0.1 m square sits inside one 1 m cell, so that cell is covered.
    assert_eq!(grid.cells.len(), 1);
    assert!(
        grid.cells
            .first()
            .is_some_and(|c| (c.area - 0.01).abs() < 1e-12 && !c.interior)
    );
}

#[test]
fn degenerate_triangles_are_dropped_rather_than_rotating_a_plane() {
    let (mut v, mut t) = rectangle(4.0, 2.0, 1);
    v.extend_from_slice(&[1.0, 0.0, 0.0]);
    t.extend_from_slice(&[0, 4, 2]); // collinear sliver on the shared edge
    let p = only(&v, &t);
    assert_eq!(p.triangles.len(), 2);
}

#[test]
fn a_face_partition_splits_coplanar_patches() {
    let (v, t) = rectangle(4.0, 2.0, 1);
    let split = build_surface_patches(&v, &t, None, &SurfacePatchOptions::default(), Some(&[1, 0]));
    assert_eq!(split.len(), 2);
}

#[test]
fn patches_are_deterministic() {
    let (v, t) = rectangle(9.0, 3.0, 7);
    assert_eq!(patches(&v, &t, None), patches(&v, &t, None));
}

#[test]
fn bad_inputs_give_nothing() {
    let (v, t) = rectangle(4.0, 2.0, 1);
    assert_eq!(patches(&[], &t, None), Vec::new());
    assert_eq!(patches(&v, &[], None), Vec::new());
    let p = only(&v, &t);
    for spacing in [0.0, -1.0, f64::NAN] {
        assert_eq!(build_patch_grid(&p, &v, &t, spacing).cells, Vec::new());
    }
}

/// The all-pairs lattice as it was before #117: every triangle clipped against
/// every cell. The reference the swept lattice must reproduce bit for bit.
fn all_pairs_grid(
    patch: &SurfacePatch,
    vertices: &[f64],
    triangles: &[u32],
    spacing: f64,
) -> PatchGrid {
    let mut grid = PatchGrid::default();
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
    for row in 0..grid.rows {
        for column in 0..grid.columns {
            let min = [f64::from(column) * spacing, f64::from(row) * spacing];
            let max = [min[0] + spacing, min[1] + spacing];
            let (mut area, mut wu, mut wv) = (0.0, 0.0, 0.0);
            for t in &flat {
                let (piece, pu, pv) = clip_triangle_to_cell(t, min, max);
                if piece <= 0.0 {
                    continue;
                }
                area += piece;
                wu += pu * piece;
                wv += pv * piece;
            }
            if area.partial_cmp(&1e-12) != Some(std::cmp::Ordering::Greater) {
                continue;
            }
            grid.cells.push(PatchCell {
                column,
                row,
                centre: world(wu / area, wv / area),
                area,
                interior: area >= spacing * spacing * (1.0 - 1e-9),
            });
        }
    }
    grid
}

/// A tilted w x h plane as an n x n lattice of triangles, interior corners
/// jittered within the plane so the triangles are irregular.
fn jittered_plane(width: f64, height: f64, n: u32) -> (Vec<f64>, Vec<u32>) {
    let mut vertices = Vec::new();
    for j in 0..=n {
        for i in 0..=n {
            let inner = i > 0 && i < n && j > 0 && j < n;
            let wobble = if inner {
                f64::from((i * 7 + j * 13) % 11) / 11.0 - 0.5
            } else {
                0.0
            };
            let u = (f64::from(i) + 0.4 * wobble) * width / f64::from(n);
            let v = (f64::from(j) - 0.3 * wobble) * height / f64::from(n);
            vertices.extend_from_slice(&[u, v * 0.8, v * 0.6]);
        }
    }
    let mut triangles = Vec::new();
    for j in 0..n {
        for i in 0..n {
            let a = j * (n + 1) + i;
            if (i + j) % 2 == 0 {
                triangles.extend_from_slice(&[a, a + 1, a + n + 2, a, a + n + 2, a + n + 1]);
            } else {
                triangles.extend_from_slice(&[a, a + 1, a + n + 1, a + 1, a + n + 2, a + n + 1]);
            }
        }
    }
    (vertices, triangles)
}

#[test]
fn the_swept_lattice_is_the_all_pairs_lattice_bit_for_bit() {
    let cases = [
        jittered_plane(7.3, 5.1, 9),
        jittered_plane(3.0, 11.0, 4),
        rectangle(9.0, 2.5, 6),
    ];
    for (vertices, triangles) in &cases {
        for patch in patches(vertices, triangles, None) {
            for spacing in [0.07, 0.33, 0.5, 1.0, 2.9, 40.0] {
                let swept = build_patch_grid(&patch, vertices, triangles, spacing);
                let reference = all_pairs_grid(&patch, vertices, triangles, spacing);
                assert_eq!(
                    swept.cells.len(),
                    reference.cells.len(),
                    "spacing {spacing}"
                );
                for (a, b) in swept.cells.iter().zip(&reference.cells) {
                    assert_eq!((a.column, a.row, a.interior), (b.column, b.row, b.interior));
                    assert_eq!(a.area.to_bits(), b.area.to_bits(), "spacing {spacing}");
                    for (p, q) in a.centre.iter().zip(&b.centre) {
                        assert_eq!(p.to_bits(), q.to_bits(), "spacing {spacing}");
                    }
                }
            }
        }
    }
}
