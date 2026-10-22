use super::*;

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

fn surface(x: f64, y: f64) -> f64 {
    0.1 * x + 0.2 * y + 0.25 * x.min(y)
}

/// `side x side` vertices at 1-m spacing on a nonplanar surface (creases on the
/// diagonals), two counter-clockwise triangles per cell.
fn grid(side: u32) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column), f64::from(row));
            points.extend([x, y, surface(x, y)]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..side - 1 {
        for column in 0..side - 1 {
            let a = row * side + column;
            triangles.extend([a, a + 1, a + side + 1, a, a + side + 1, a + side]);
        }
    }
    (points, triangles)
}

fn ring(corners: &[[f64; 2]]) -> Vec<f64> {
    corners.iter().flat_map(|&[x, y]| [x, y, 0.0]).collect()
}

fn signed_area(points: &[f64], [a, b, c]: [u32; 3]) -> f64 {
    let vertices = plan::vertices(points);
    let at = |v: u32| vertices.get(v as usize).copied().unwrap_or([0.0; 3]);
    let ([ax, ay, _], [bx, by, _], [cx, cy, _]) = (at(a), at(b), at(c));
    0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))
}

/// Barycentric containment of `(x, y)` in input triangle `face`.
fn within(points: &[f64], triangles: &[u32], face: u32, x: f64, y: f64) -> bool {
    let corners = triangles.as_chunks::<3>().0;
    let Some(&triple) = corners.get(face as usize) else {
        return false;
    };
    let vertices = plan::vertices(points);
    let [[ax, ay, _], [bx, by, _], [cx, cy, _]] =
        triple.map(|vertex| vertices.get(vertex as usize).copied().unwrap_or([0.0; 3]));
    let area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    let wa = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area;
    let wb = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area;
    [wa, wb, 1.0 - wa - wb].iter().all(|&w| w >= -1e-12)
}

#[test]
fn the_inside_is_rebuilt_with_breaklines_and_the_outside_is_untouched() -> Result<(), RegionError> {
    let (points, triangles) = grid(9);
    let region = ring(&[[1.5, 1.5], [6.5, 1.5], [6.5, 6.5], [1.5, 6.5]]);
    let line = [2.2, 3.3, 0.0, 4.0, 3.9, 0.0, 5.7, 4.1, 0.0];
    let breaklines: [&[f64]; 1] = [&line];
    let request = RegionRequest {
        points: &points,
        triangles: &triangles,
        region: &region,
        breaklines: &breaklines,
    };
    let mesh = retriangulate_region(&request)?;
    assert_eq!(retriangulate_region(&request)?, mesh, "deterministic");
    assert_eq!(mesh.input_vertices, 81);
    assert_eq!(mesh.points.get(..points.len()), Some(&points[..]));

    // Still the base surface everywhere: new vertices are interpolated.
    for &[x, y, z] in plan::vertices(&mesh.points) {
        assert!(close(z, surface(x, y), 1e-12), "{x},{y}: {z}");
    }
    let corners = mesh.triangles.as_chunks::<3>().0;
    let (mut inside_area, mut kept) = (0.0, 0);
    for (((triple, &parent), &is_kept), &is_inside) in corners
        .iter()
        .zip(&mesh.parents)
        .zip(&mesh.kept)
        .zip(&mesh.inside)
    {
        let area = signed_area(&mesh.points, *triple);
        assert!(area > 0.0, "counter-clockwise, non-degenerate");
        // Each output triangle lies in one input triangle: base edges survived.
        for &vertex in triple {
            let [x, y, _] = plan::vertices(&mesh.points)
                .get(vertex as usize)
                .copied()
                .unwrap_or([f64::NAN; 3]);
            assert!(within(&points, &triangles, parent, x, y));
        }
        if is_inside {
            inside_area += area;
        }
        if is_kept {
            kept += 1;
            assert_eq!(
                triangles.as_chunks::<3>().0.get(parent as usize),
                Some(triple)
            );
        }
    }
    assert!(close(inside_area, 25.0, 1e-9));
    assert!(kept > 0 && mesh.kept.iter().zip(&mesh.inside).all(|(k, i)| !(*k && *i)));

    // Stitch: every ring edge has exactly one inside and one outside triangle.
    let mut uses: BTreeMap<[u32; 2], [u32; 2]> = BTreeMap::new();
    for (&[a, b, c], &is_inside) in corners.iter().zip(&mesh.inside) {
        for (p, q) in [(a, b), (b, c), (c, a)] {
            let entry = uses.entry(sorted(p, q)).or_default();
            if let Some(slot) = entry.get_mut(usize::from(is_inside)) {
                *slot += 1;
            }
        }
    }
    let count = mesh.ring.len();
    for (index, &vertex) in mesh.ring.iter().enumerate() {
        let next = mesh
            .ring
            .get((index + 1) % count)
            .copied()
            .unwrap_or(vertex);
        assert_eq!(uses.get(&sorted(vertex, next)), Some(&[1, 1]));
    }
    // The breakline is a connected path of output edges between its vertices.
    let ends = mesh.breakline_vertices.first().cloned().unwrap_or_default();
    assert_eq!(ends.len(), 3);
    let edges: Vec<[u32; 2]> = mesh.line_edges.iter().map(|edge| edge.vertices).collect();
    assert_eq!(edges.first().map(|e| e[0]), ends.first().copied());
    assert_eq!(edges.last().map(|e| e[1]), ends.last().copied());
    assert!(
        edges
            .windows(2)
            .all(|pair| matches!(pair, [a, b] if a[1] == b[0]))
    );
    assert!(mesh.new_vertices.iter().any(|v| matches!(
        v,
        RegionVertex::Crossing(
            [Support::BaseEdge(_), Support::Breakline { line: 0, .. }]
                | [Support::Breakline { line: 0, .. }, Support::BaseEdge(_)]
        )
    )));
    Ok(())
}

#[test]
fn a_breakline_vertex_on_a_mesh_vertex_reuses_it() -> Result<(), RegionError> {
    let (points, triangles) = grid(9);
    let region = ring(&[[1.5, 1.5], [6.5, 1.5], [6.5, 6.5], [1.5, 6.5]]);
    let line = [3.0, 3.0, 0.0, 5.0, 3.0, 0.0];
    let breaklines: [&[f64]; 1] = [&line];
    let mesh = retriangulate_region(&RegionRequest {
        points: &points,
        triangles: &triangles,
        region: &region,
        breaklines: &breaklines,
    })?;
    assert_eq!(mesh.breakline_vertices, vec![vec![3 * 9 + 3, 3 * 9 + 5]]);
    // Along an existing grid line: no new vertex at all.
    assert_eq!(mesh.new_vertices, Vec::<RegionVertex>::new());
    Ok(())
}

#[test]
fn breaklines_leaving_the_region_and_base_holes_refuse() {
    let (points, triangles) = grid(9);
    let notched = ring(&[
        [0.5, 0.5],
        [6.5, 0.5],
        [6.5, 3.5],
        [3.5, 3.5],
        [3.5, 6.5],
        [0.5, 6.5],
    ]);
    let across = [6.0, 1.5, 0.0, 1.5, 6.0, 0.0];
    let outside = [2.0, 2.0, 0.0, 8.0, 8.0, 0.0];
    let short = [2.0, 2.0, 0.0];
    let run = |region: &[f64], line: &[f64], triangles: &[u32]| {
        let breaklines: [&[f64]; 1] = [line];
        retriangulate_region(&RegionRequest {
            points: &points,
            triangles,
            region,
            breaklines: &breaklines,
        })
        .err()
    };
    assert_eq!(
        run(&notched, &across, &triangles),
        Some(RegionError::BoundaryCrossed)
    );
    assert_eq!(
        run(&notched, &outside, &triangles),
        Some(RegionError::Breakline)
    );
    assert_eq!(
        run(&notched, &short, &triangles),
        Some(RegionError::Breakline)
    );
    // Remove the two triangles of cell (3, 3): a hole inside the region.
    let cell = (3 * 8 + 3) * 6;
    let holed: Vec<u32> = triangles
        .iter()
        .enumerate()
        .filter(|(i, _)| !(cell..cell + 6).contains(i))
        .map(|(_, &v)| v)
        .collect();
    let square = ring(&[[1.5, 1.5], [6.5, 1.5], [6.5, 6.5], [1.5, 6.5]]);
    let inner = [2.5, 2.5, 0.0, 2.6, 2.7, 0.0];
    // The split itself already refuses a ring around uncovered plan; the
    // topology check is the second line of defence.
    assert!(matches!(
        run(&square, &inner, &holed),
        Some(RegionError::Topology | RegionError::Terrain(Error::PropertyLineOutside))
    ));
    assert!(
        RegionError::Topology
            .message()
            .starts_with("RegionTopology:")
    );
}
