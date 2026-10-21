use std::collections::BTreeMap;

use super::*;

fn plane(x: f64, y: f64) -> f64 {
    0.125 * x - 0.25 * y + 3.0
}

/// `cells` x `cells` grid over [0, 10]^2 on the plane, two CCW triangles per
/// cell split along the rising diagonal.
fn grid(cells: u32) -> (Vec<f64>, Vec<u32>) {
    let step = 10.0 / f64::from(cells);
    let mut points = Vec::new();
    for row in 0..=cells {
        for column in 0..=cells {
            let (x, y) = (f64::from(column) * step, f64::from(row) * step);
            points.extend([x, y, plane(x, y)]);
        }
    }
    let at = |row: u32, column: u32| row * (cells + 1) + column;
    let mut triangles = Vec::new();
    for row in 0..cells {
        for column in 0..cells {
            let (a, b) = (at(row, column), at(row, column + 1));
            let (c, d) = (at(row + 1, column + 1), at(row + 1, column));
            triangles.extend([a, b, c, a, c, d]);
        }
    }
    (points, triangles)
}

fn flat(ring: &[[f64; 2]]) -> Vec<f64> {
    ring.iter().flat_map(|&[x, y]| [x, y, 0.0]).collect()
}

fn request<'a>(points: &'a [f64], triangles: &'a [u32], line: &'a [f64]) -> SplitRequest<'a> {
    SplitRequest {
        points,
        triangles,
        line,
        arc_angles: &[],
    }
}

fn point(points: &[f64], vertex: u32) -> [f64; 3] {
    let start = usize::try_from(vertex).map_or(usize::MAX, |index| index * 3);
    match points.get(start..start + 3) {
        Some(&[x, y, z]) => [x, y, z],
        _ => [f64::NAN; 3],
    }
}

fn area(points: &[f64], [a, b, c]: [u32; 3]) -> f64 {
    let [a, b, c] = [point(points, a), point(points, b), point(points, c)];
    0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
}

fn ring_area(ring: &[[f64; 2]]) -> f64 {
    let pairs = ring.iter().zip(ring.iter().cycle().skip(1));
    0.5 * pairs.map(|(a, b)| a[0] * b[1] - a[1] * b[0]).sum::<f64>()
}

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// Invariants every successful split must satisfy.
fn check(input: &SplitRequest<'_>, split: &SplitMesh, ring: &[[f64; 2]]) {
    let inputs = input.triangles.as_chunks::<3>().0;
    let outputs = split.triangles.as_chunks::<3>().0;
    assert_eq!(split.points.get(..input.points.len()), Some(input.points));
    assert_eq!(split.parents.len(), outputs.len());
    assert_eq!(split.inside.len(), outputs.len());
    assert!(split.parents.is_sorted(), "children follow their parents");
    let mut children: BTreeMap<u32, (usize, f64)> = BTreeMap::new();
    for (face, &parent) in outputs.iter().zip(&split.parents) {
        let face_area = area(&split.points, *face);
        assert!(face_area > 0.0, "{face:?} is counter-clockwise");
        let entry = children.entry(parent).or_default();
        entry.0 += 1;
        entry.1 += face_area;
    }
    for (parent, original) in (0_u32..).zip(inputs) {
        let (count, sum) = children.get(&parent).copied().unwrap_or_default();
        assert!(
            close(sum, area(input.points, *original), 1e-9),
            "parent {parent}"
        );
        if count == 1 {
            let index = split.parents.iter().position(|&p| p == parent);
            let kept = index.and_then(|i| outputs.get(i));
            assert_eq!(kept, Some(original), "untouched {parent} is verbatim");
        }
    }
    let vertex_count = u32::try_from(split.points.len() / 3).unwrap_or(0);
    for vertex in 0..vertex_count {
        let [x, y, z] = point(&split.points, vertex);
        assert!(
            close(z, plane(x, y), 1e-9),
            "vertex {vertex} on the surface"
        );
    }
    let inside: f64 = outputs
        .iter()
        .zip(&split.inside)
        .filter(|(_, inside)| **inside)
        .map(|(face, _)| area(&split.points, *face))
        .sum();
    assert!(close(inside, ring_area(ring).abs(), 1e-9), "inside area");
    // Conforming and manifold: every edge has one or two triangles, and the
    // mesh boundary length is unchanged.
    let mut uses: BTreeMap<[u32; 2], usize> = BTreeMap::new();
    for face in outputs {
        for edge in super::super::classify::face_edges(*face) {
            *uses.entry(key(edge)).or_default() += 1;
        }
    }
    assert!(uses.values().all(|&count| count <= 2));
    let boundary: f64 = uses
        .iter()
        .filter(|(_, count)| **count == 1)
        .map(|(edge, _)| {
            let [a, b] = edge.map(|vertex| point(&split.points, vertex));
            (b[0] - a[0]).hypot(b[1] - a[1])
        })
        .sum();
    assert!(close(boundary, 40.0, 1e-9), "boundary {boundary}");
    // The line edges walk the ring through its vertices.
    let segments = u32::try_from(ring.len()).unwrap_or(0);
    let mut rows = split.line_edges.iter().peekable();
    for segment in 0..segments {
        let from = split
            .line_vertices
            .get(usize::try_from(segment).unwrap_or(0))
            .copied();
        let next = usize::try_from((segment + 1) % segments).unwrap_or(0);
        let mut current = from;
        while let Some(row) = rows.next_if(|row| row.segment == segment) {
            assert_eq!(Some(row.vertices[0]), current);
            current = Some(row.vertices[1]);
        }
        assert_eq!(current, split.line_vertices.get(next).copied());
    }
    assert_eq!(rows.next(), None);
}

#[test]
fn generic_quadrilateral_splits_only_the_triangles_it_touches() -> Result<(), Error> {
    let (points, triangles) = grid(5);
    let ring = [[1.3, 1.7], [7.2, 1.1], [8.4, 7.9], [2.2, 8.6]];
    let line = flat(&ring);
    let input = request(&points, &triangles, &line);
    let split = split_mesh(&input)?;
    check(&input, &split, &ring);
    assert_eq!(split_mesh(&input)?, split, "repeatable");
    for vertex in &split.new_vertices {
        match vertex.origin {
            Origin::LineVertex(_) => assert!(matches!(vertex.host, Host::Face(_))),
            Origin::Crossing(_) => assert!(matches!(vertex.host, Host::Edge(_))),
        }
    }
    let line_vertices = split
        .new_vertices
        .iter()
        .filter(|vertex| matches!(vertex.origin, Origin::LineVertex(_)))
        .count();
    assert_eq!(line_vertices, 4);
    // 50 input triangles: some rebuilt, the rest emitted verbatim.
    let mut children: BTreeMap<u32, usize> = BTreeMap::new();
    for &parent in &split.parents {
        *children.entry(parent).or_default() += 1;
    }
    assert_eq!(children.len(), 50);
    let rebuilt = children.values().filter(|&&count| count > 1).count();
    assert!(rebuilt > 0 && rebuilt < 30, "{rebuilt} rebuilt");
    Ok(())
}

#[test]
fn line_along_mesh_edges_leaves_the_mesh_identical() -> Result<(), Error> {
    let (points, triangles) = grid(5);
    let ring = [[2.0, 2.0], [6.0, 2.0], [6.0, 6.0], [2.0, 6.0]];
    let line = flat(&ring);
    let input = request(&points, &triangles, &line);
    let split = split_mesh(&input)?;
    check(&input, &split, &ring);
    assert_eq!(split.points, points);
    assert_eq!(split.triangles, triangles);
    assert_eq!(split.new_vertices, []);
    assert_eq!(split.line_vertices, [7, 9, 21, 19]);
    assert_eq!(split.line_edges.len(), 8);
    Ok(())
}

#[test]
fn line_vertices_on_edges_and_through_mesh_vertices() -> Result<(), Error> {
    let (points, triangles) = grid(5);
    // (3, 2) is on a horizontal grid edge; the diagonal (4,4)-(8,8) runs along
    // cell diagonals through mesh vertices.
    let ring = [[3.0, 2.0], [8.0, 2.0], [8.0, 8.0], [4.0, 4.0]];
    let line = flat(&ring);
    let input = request(&points, &triangles, &line);
    let split = split_mesh(&input)?;
    check(&input, &split, &ring);
    assert_eq!(
        split.new_vertices.first(),
        Some(&NewVertex {
            origin: Origin::LineVertex(0),
            host: Host::Edge([7, 8]),
        })
    );
    let diagonal: Vec<[u32; 2]> = split
        .line_edges
        .iter()
        .filter(|row| row.segment == 2)
        .map(|row| row.vertices)
        .collect();
    assert_eq!(diagonal, [[28, 21], [21, 14]]);
    Ok(())
}

#[test]
fn arcs_refuse_before_any_other_line_check() {
    let (points, triangles) = grid(2);
    let line = flat(&[[1.0, 1.0], [9.0, 1.0], [5.0, 9.0]]);
    let arcs = [0.0, 0.5, 0.0];
    let arc = SplitRequest {
        arc_angles: &arcs,
        ..request(&points, &triangles, &line)
    };
    assert_eq!(split_mesh(&arc), Err(Error::PropertyLineArcUnsupported));
    let straight = [0.0; 3];
    let ok = SplitRequest {
        arc_angles: &straight,
        ..request(&points, &triangles, &line)
    };
    assert!(split_mesh(&ok).is_ok());
    let short = [0.0; 2];
    let mismatched = SplitRequest {
        arc_angles: &short,
        ..request(&points, &triangles, &line)
    };
    assert_eq!(split_mesh(&mismatched), Err(Error::PropertyLineOutside));
}

#[test]
fn malformed_or_uncovered_lines_are_outside() {
    let (points, triangles) = grid(4);
    let cases: [&[[f64; 2]]; 7] = [
        &[[1.0, 1.0], [9.0, 1.0]],
        &[[1.0, 1.0], [9.0, 1.0], [9.0, 1.0], [5.0, 9.0]],
        &[[1.0, 1.0], [9.0, 9.0], [9.0, 1.0], [1.0, 9.0]],
        &[[1.0, 1.0], [5.0, 1.0], [9.0, 1.0]],
        &[[1.0, 1.0], [12.0, 1.0], [5.0, 9.0]],
        &[[1.0, 1.0], [9.0, 1.0], [5.0, 9.0], [5.0, 0.5]],
        &[[-1.0, -1.0], [9.0, 1.0], [5.0, 9.0]],
    ];
    for ring in cases {
        let line = flat(ring);
        assert_eq!(
            split_mesh(&request(&points, &triangles, &line)),
            Err(Error::PropertyLineOutside),
            "{ring:?}"
        );
    }
    let nan = [1.0, 1.0, 0.0, 9.0, f64::NAN, 0.0, 5.0, 9.0, 0.0];
    assert_eq!(
        split_mesh(&request(&points, &triangles, &nan)),
        Err(Error::PropertyLineOutside)
    );
}

#[test]
fn line_across_a_mesh_hole_is_outside() -> Result<(), Error> {
    let square = [
        0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0, 0.0, 0.0, 10.0, 0.0, //
        4.0, 4.0, 0.0, 6.0, 4.0, 0.0, 6.0, 6.0, 0.0, 4.0, 6.0, 0.0,
    ];
    let constraints = [
        Constraint {
            role: Role::Boundary,
            vertices: &[0, 1, 2, 3],
        },
        Constraint {
            role: Role::Hole,
            vertices: &[4, 5, 6, 7],
        },
    ];
    let tin = triangulate(&Request {
        points: &square,
        constraints: &constraints,
        z_tolerance: 0.0,
    })?;
    let around = flat(&[[3.0, 3.0], [7.0, 3.0], [7.0, 7.0], [3.0, 7.0]]);
    assert_eq!(
        split_mesh(&request(&tin.points, &tin.triangles, &around)),
        Err(Error::PropertyLineOutside)
    );
    let through = flat(&[[1.0, 1.0], [9.0, 1.0], [5.0, 5.5]]);
    assert_eq!(
        split_mesh(&request(&tin.points, &tin.triangles, &through)),
        Err(Error::PropertyLineOutside)
    );
    let beside = flat(&[[1.0, 1.0], [9.0, 1.0], [9.0, 3.0]]);
    assert!(split_mesh(&request(&tin.points, &tin.triangles, &beside)).is_ok());
    Ok(())
}

#[test]
fn malformed_meshes_refuse() {
    let (points, triangles) = grid(2);
    let line = flat(&[[1.0, 1.0], [9.0, 1.0], [5.0, 9.0]]);
    let mut clockwise = triangles.clone();
    clockwise.swap(1, 2);
    assert_eq!(
        split_mesh(&request(&points, &clockwise, &line)),
        Err(Error::TriangulationDegenerate)
    );
    let mut missing = triangles.clone();
    if let Some(last) = missing.last_mut() {
        *last = 99;
    }
    assert_eq!(
        split_mesh(&request(&points, &missing, &line)),
        Err(Error::Index)
    );
    assert_eq!(
        split_mesh(&request(&points, triangles.get(..5).unwrap_or(&[]), &line)),
        Err(Error::Shape)
    );
    let mut infinite = points;
    if let Some(first) = infinite.first_mut() {
        *first = f64::INFINITY;
    }
    assert_eq!(
        split_mesh(&request(&infinite, &triangles, &line)),
        Err(Error::NonFinite)
    );
}

struct Lcg(u64);

impl Lcg {
    fn unit(&mut self) -> f64 {
        self.0 = self
            .0
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        f64::from_bits(0x3FF0_0000_0000_0000 | (self.0 >> 12)) - 1.0
    }
}

#[test]
fn seeded_star_lines_over_seeded_tins() -> Result<(), Error> {
    for seed in 0..48_u64 {
        let mut random = Lcg(0x5711_7000 + seed);
        let mut points: Vec<f64> = [0.0, 0.0, 10.0, 0.0, 10.0, 10.0, 0.0, 10.0]
            .as_chunks::<2>()
            .0
            .iter()
            .flat_map(|&[x, y]| [x, y, plane(x, y)])
            .collect();
        for _ in 0..40 {
            let (x, y) = (10.0 * random.unit(), 10.0 * random.unit());
            points.extend([x, y, plane(x, y)]);
        }
        let tin = triangulate(&Request {
            points: &points,
            constraints: &[Constraint {
                role: Role::Boundary,
                vertices: &[0, 1, 2, 3],
            }],
            z_tolerance: 0.0,
        })?;
        let count = 5 + u32::try_from(seed % 9).unwrap_or(0);
        let ring: Vec<[f64; 2]> = (0..count)
            .map(|index| {
                let angle = std::f64::consts::TAU * (f64::from(index) + 0.8 * random.unit())
                    / f64::from(count);
                let radius = 1.5 + 3.0 * random.unit();
                [5.0 + radius * angle.cos(), 5.0 + radius * angle.sin()]
            })
            .collect();
        let line = flat(&ring);
        let input = request(&tin.points, &tin.triangles, &line);
        let split = split_mesh(&input)?;
        check(&input, &split, &ring);
        assert_eq!(split_mesh(&input)?, split, "seed {seed} repeats");
    }
    Ok(())
}
