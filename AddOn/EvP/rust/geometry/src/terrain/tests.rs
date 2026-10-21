use std::collections::BTreeSet;

use super::*;

const SQUARE: [f64; 12] = [
    0.0, 0.0, 0.0, //
    10.0, 0.0, 1.0, //
    10.0, 10.0, 2.0, //
    0.0, 10.0, 1.0,
];
const OUTER: [u32; 4] = [0, 1, 2, 3];

fn request<'a>(points: &'a [f64], constraints: &'a [Constraint<'a>]) -> Request<'a> {
    Request {
        points,
        constraints,
        z_tolerance: 1e-9,
    }
}

const fn boundary(vertices: &[u32]) -> Constraint<'_> {
    Constraint {
        role: Role::Boundary,
        vertices,
    }
}

const fn hole(vertices: &[u32]) -> Constraint<'_> {
    Constraint {
        role: Role::Hole,
        vertices,
    }
}

const fn breakline(vertices: &[u32]) -> Constraint<'_> {
    Constraint {
        role: Role::Breakline,
        vertices,
    }
}

fn point(tin: &Tin, vertex: u32) -> [f64; 3] {
    let start = usize::try_from(vertex).map_or(usize::MAX, |index| index * 3);
    match tin.points.get(start..start + 3) {
        Some(&[x, y, z]) => [x, y, z],
        _ => [f64::NAN; 3],
    }
}

fn faces(tin: &Tin) -> Vec<[u32; 3]> {
    tin.triangles.as_chunks::<3>().0.to_vec()
}

fn signed_area(tin: &Tin, [a, b, c]: [u32; 3]) -> f64 {
    let [a, b, c] = [point(tin, a), point(tin, b), point(tin, c)];
    0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
}

fn total_area(tin: &Tin) -> f64 {
    faces(tin)
        .into_iter()
        .map(|face| signed_area(tin, face))
        .sum()
}

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// Used vertices minus edges plus faces of the kept triangles.
fn euler(tin: &Tin) -> Result<i64, Error> {
    let used: BTreeSet<u32> = tin.triangles.iter().copied().collect();
    let edges = extract_edges(&tin.triangles)?;
    let count = |n: usize| i64::try_from(n).unwrap_or(i64::MAX);
    Ok(count(used.len()) - count(edges.len()) + count(faces(tin).len()))
}

fn rows_of(tin: &Tin, constraint: u32) -> Vec<[u32; 2]> {
    tin.constraint_edges
        .iter()
        .filter(|row| row.segment.constraint == constraint)
        .map(|row| row.vertices)
        .collect()
}

#[test]
fn square_with_a_spot_is_a_fan_with_its_ring_enforced() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    points.extend([5.0, 5.0, 3.0]);
    let constraints = [boundary(&OUTER)];
    let tin = triangulate(&request(&points, &constraints))?;
    assert_eq!(tin.points, points);
    assert_eq!(tin.input_vertices, [0, 1, 2, 3, 4]);
    assert_eq!(faces(&tin), [[0, 1, 4], [0, 4, 3], [1, 2, 4], [2, 3, 4]]);
    assert_eq!(tin.face_boundary, [Some(0); 4]);
    assert_eq!(rows_of(&tin, 0), [[0, 1], [1, 2], [2, 3], [3, 0]]);
    assert!(tin.constraint_edges.iter().all(|row| row.kept));
    assert_eq!(tin.crossings, []);
    assert!(close(total_area(&tin), 100.0, 1e-12));
    assert_eq!(euler(&tin)?, 1);
    Ok(())
}

#[test]
fn hole_faces_are_excluded_and_its_ring_has_one_kept_side() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    points.extend([4.0, 4.0, 0.0, 6.0, 4.0, 0.0, 6.0, 6.0, 0.0, 4.0, 6.0, 0.0]);
    let inner = [4, 5, 6, 7];
    let constraints = [boundary(&OUTER), hole(&inner)];
    let tin = triangulate(&request(&points, &constraints))?;
    assert!(close(total_area(&tin), 96.0, 1e-12));
    assert_eq!(euler(&tin)?, 0);
    let hole_rows = rows_of(&tin, 1);
    assert_eq!(hole_rows, [[4, 5], [5, 6], [6, 7], [7, 4]]);
    for face in faces(&tin) {
        let centre = face.map(|vertex| point(&tin, vertex));
        let x = (centre[0][0] + centre[1][0] + centre[2][0]) / 3.0;
        let y = (centre[0][1] + centre[1][1] + centre[2][1]) / 3.0;
        assert!(!(4.0..=6.0).contains(&x) || !(4.0..=6.0).contains(&y));
    }
    Ok(())
}

#[test]
fn island_inside_a_hole_is_kept_and_names_its_own_boundary() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    points.extend([2.0, 2.0, 0.0, 8.0, 2.0, 0.0, 8.0, 8.0, 0.0, 2.0, 8.0, 0.0]);
    points.extend([4.0, 4.0, 0.0, 6.0, 4.0, 0.0, 6.0, 6.0, 0.0, 4.0, 6.0, 0.0]);
    let (gap, island) = ([4, 5, 6, 7], [8, 9, 10, 11]);
    let constraints = [boundary(&OUTER), hole(&gap), boundary(&island)];
    let tin = triangulate(&request(&points, &constraints))?;
    assert!(close(total_area(&tin), 100.0 - 36.0 + 4.0, 1e-12));
    let island_faces = tin.face_boundary.iter().filter(|b| **b == Some(2)).count();
    assert_eq!(island_faces, 2);
    assert!(tin.face_boundary.iter().all(|b| matches!(b, Some(0 | 2))));
    Ok(())
}

#[test]
fn crossing_breaklines_are_presplit_with_interpolated_heights() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    // Two breaklines on the plane z = 0.1x + 0.1y crossing at (5, 5).
    points.extend([2.0, 2.0, 0.4, 8.0, 8.0, 1.6, 2.0, 8.0, 1.0, 8.0, 2.0, 1.0]);
    let (first, second) = ([4, 5], [6, 7]);
    let constraints = [boundary(&OUTER), breakline(&first), breakline(&second)];
    let tin = triangulate(&request(&points, &constraints))?;
    let reference = |constraint| SegmentRef {
        constraint,
        segment: 0,
    };
    assert_eq!(
        tin.crossings,
        [Crossing {
            vertex: 8,
            segments: [reference(1), reference(2)],
        }]
    );
    let [x, y, z] = point(&tin, 8);
    assert!(close(x, 5.0, 1e-12) && close(y, 5.0, 1e-12) && close(z, 1.0, 1e-12));
    assert_eq!(rows_of(&tin, 1), [[4, 8], [8, 5]]);
    assert_eq!(rows_of(&tin, 2), [[6, 8], [8, 7]]);
    assert!(close(total_area(&tin), 100.0, 1e-12));
    Ok(())
}

#[test]
fn crossing_breaklines_with_different_heights_refuse() {
    let mut points = SQUARE.to_vec();
    points.extend([2.0, 2.0, 0.0, 8.0, 8.0, 0.0, 2.0, 8.0, 1.0, 8.0, 2.0, 1.0]);
    let (first, second) = ([4, 5], [6, 7]);
    let constraints = [boundary(&OUTER), breakline(&first), breakline(&second)];
    assert_eq!(
        triangulate(&request(&points, &constraints)),
        Err(Error::TriangulationConstraint { constraint: 2 })
    );
    let tolerant = Request {
        z_tolerance: 1.0,
        ..request(&points, &constraints)
    };
    assert!(triangulate(&tolerant).is_ok());
}

#[test]
fn breakline_crossing_the_boundary_splits_both_and_its_outside_part_is_not_kept()
-> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    points.extend([5.0, 5.0, 1.5, 15.0, 5.0, 1.5]);
    let line = [4, 5];
    let constraints = [boundary(&OUTER), breakline(&line)];
    let tin = triangulate(&request(&points, &constraints))?;
    assert_eq!(
        tin.crossings,
        [Crossing {
            vertex: 6,
            segments: [
                SegmentRef {
                    constraint: 0,
                    segment: 1,
                },
                SegmentRef {
                    constraint: 1,
                    segment: 0,
                },
            ],
        }]
    );
    assert_eq!(point(&tin, 6), [10.0, 5.0, 1.5]);
    let kept: Vec<bool> = tin
        .constraint_edges
        .iter()
        .filter(|row| row.segment.constraint == 1)
        .map(|row| row.kept)
        .collect();
    assert_eq!(kept, [true, false]);
    assert!(close(total_area(&tin), 100.0, 1e-12));
    Ok(())
}

#[test]
fn touching_and_overlapping_constraints_share_vertices_and_edges() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    // A breakline from the boundary edge midpoint (T-junction) inwards, and a
    // second breakline overlapping the first one's inner half.
    points.extend([5.0, 0.0, 0.5, 5.0, 5.0, 1.0, 5.0, 2.5, 0.75, 5.0, 8.0, 1.3]);
    let (first, second) = ([4, 5], [6, 7]);
    let constraints = [boundary(&OUTER), breakline(&first), breakline(&second)];
    let tin = triangulate(&request(&points, &constraints))?;
    assert_eq!(rows_of(&tin, 0).first(), Some(&[0, 4]));
    assert!(rows_of(&tin, 0).contains(&[4, 1]));
    assert_eq!(rows_of(&tin, 1), [[4, 6], [6, 5]]);
    assert_eq!(rows_of(&tin, 2), [[6, 5], [5, 7]]);
    assert_eq!(tin.crossings, []);
    Ok(())
}

#[test]
fn hull_domain_without_boundary_has_no_face_boundary() -> Result<(), Error> {
    let constraints = [];
    let tin = triangulate(&request(&SQUARE, &constraints))?;
    assert_eq!(tin.face_boundary, [None, None]);
    assert!(close(total_area(&tin), 100.0, 1e-12));
    Ok(())
}

#[test]
fn ring_conflicts_refuse_classification() {
    let mut points = SQUARE.to_vec();
    points.extend([5.0, 5.0, 0.0, 15.0, 5.0, 0.0, 15.0, 6.0, 0.0]);
    let crossing_hole = [4, 5, 6];
    let constraints = [boundary(&OUTER), hole(&crossing_hole)];
    assert_eq!(
        triangulate(&request(&points, &constraints)),
        Err(Error::HoleClassificationFailed)
    );

    let mut points = SQUARE.to_vec();
    points.extend([20.0, 20.0, 0.0, 22.0, 20.0, 0.0, 21.0, 22.0, 0.0]);
    let outside_hole = [4, 5, 6];
    let constraints = [boundary(&OUTER), hole(&outside_hole)];
    assert_eq!(
        triangulate(&request(&points, &constraints)),
        Err(Error::HoleClassificationFailed)
    );

    let mut points = SQUARE.to_vec();
    points.extend([4.0, 4.0, 0.0, 6.0, 4.0, 0.0, 5.0, 6.0, 0.0]);
    let nested = [4, 5, 6];
    let constraints = [boundary(&OUTER), boundary(&nested)];
    assert_eq!(
        triangulate(&request(&points, &constraints)),
        Err(Error::HoleClassificationFailed)
    );

    let bow_tie = [0, 2, 1, 3];
    let constraints = [boundary(&bow_tie)];
    assert_eq!(
        triangulate(&request(&SQUARE, &constraints)),
        Err(Error::HoleClassificationFailed)
    );

    // A hole sharing an edge with the boundary has no kept side on that edge.
    let mut points = SQUARE.to_vec();
    points.extend([5.0, 5.0, 0.0]);
    let notch = [0, 1, 4];
    let constraints = [boundary(&OUTER), hole(&notch)];
    assert_eq!(
        triangulate(&request(&points, &constraints)),
        Err(Error::HoleClassificationFailed)
    );
}

#[test]
fn duplicate_positions_merge_only_within_the_height_tolerance() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    points.extend([-0.0, 0.0, 0.0, 5.0, 5.0, 1.0]);
    let constraints = [boundary(&OUTER), breakline(&[4, 5])];
    let tin = triangulate(&request(&points, &constraints))?;
    assert_eq!(tin.input_vertices, [0, 1, 2, 3, 0, 4]);
    assert_eq!(tin.points.len(), 15);
    assert_eq!(rows_of(&tin, 1), [[0, 4]]);

    let mut points = SQUARE.to_vec();
    points.extend([0.0, 0.0, 0.5]);
    assert_eq!(
        triangulate(&request(&points, &[])),
        Err(Error::TriangulationDegenerate)
    );
    Ok(())
}

#[test]
fn degenerate_and_malformed_requests_refuse() {
    let collinear = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 2.0, 0.0, 0.0, 3.0, 0.0, 0.0];
    let check = |input: Request<'_>, error: Error| {
        assert_eq!(triangulate(&input), Err(error), "{input:?}");
    };
    check(request(&collinear, &[]), Error::TriangulationDegenerate);
    check(request(&SQUARE[..6], &[]), Error::TriangulationDegenerate);
    check(request(&SQUARE[..11], &[]), Error::Shape);
    let nan = [0.0, 0.0, f64::NAN, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0];
    check(request(&nan, &[]), Error::NonFinite);
    check(request(&SQUARE, &[breakline(&[0])]), Error::Shape);
    check(request(&SQUARE, &[boundary(&[0, 1])]), Error::Shape);
    check(request(&SQUARE, &[breakline(&[0, 4])]), Error::Index);
    check(
        request(&SQUARE, &[breakline(&[0, 0])]),
        Error::TriangulationDegenerate,
    );
    let negative = Request {
        z_tolerance: -1.0,
        ..request(&SQUARE, &[])
    };
    check(negative, Error::Tolerance);
    let huge = [0.0, 0.0, 0.0, 1e300, 0.0, 0.0, 0.0, 1e300, 0.0];
    assert_eq!(triangulate(&request(&huge, &[])), Err(Error::Coordinate));
}

#[test]
fn georeferenced_coordinates_keep_full_precision() -> Result<(), Error> {
    let (east, north) = (2_600_000.0, 1_200_000.0);
    let points: Vec<f64> = SQUARE
        .as_chunks::<3>()
        .0
        .iter()
        .flat_map(|&[x, y, z]| [x + east, y + north, z + 400.0])
        .chain([east + 5.0, north + 5.0, 401.5])
        .collect();
    let constraints = [boundary(&OUTER)];
    let tin = triangulate(&request(&points, &constraints))?;
    assert_eq!(tin.points, points);
    assert!(close(total_area(&tin), 100.0, 1e-6));
    Ok(())
}

#[test]
fn verification_rejects_a_tampered_result() -> Result<(), Error> {
    let mut points = SQUARE.to_vec();
    points.extend([2.0, 5.0, 0.7, 8.0, 5.0, 1.3]);
    let line = [4, 5];
    let constraints = [boundary(&OUTER), breakline(&line)];
    let input = request(&points, &constraints);
    let tin = triangulate(&input)?;
    assert_eq!(verify_constraints(&input, &tin), Ok(()));

    // Drop the triangles on the breakline's first edge.
    let mut flipped = tin.clone();
    let row = rows_of(&tin, 1).first().copied().unwrap_or_default();
    flipped.triangles = faces(&tin)
        .into_iter()
        .filter(|face| !(face.contains(&row[0]) && face.contains(&row[1])))
        .flatten()
        .collect();
    assert_eq!(
        verify_constraints(&input, &flipped),
        Err(Error::BreaklineUnenforced { constraint: 1 })
    );

    let mut broken = tin.clone();
    broken
        .constraint_edges
        .retain(|edge| edge.segment.constraint != 1);
    assert_eq!(
        verify_constraints(&input, &broken),
        Err(Error::BreaklineUnenforced { constraint: 1 })
    );

    let mut extra = tin;
    extra.constraint_edges.push(ConstraintEdge {
        segment: SegmentRef {
            constraint: 7,
            segment: 0,
        },
        vertices: [0, 1],
        kept: true,
    });
    assert_eq!(verify_constraints(&input, &extra), Err(Error::Shape));
    assert_eq!(extract_edges(&[0, 1]), Err(Error::Shape));
    assert_eq!(
        extract_edges(&[0, 1, 2, 2, 1, 3])?,
        [[0, 1], [0, 2], [1, 2], [1, 3], [2, 3]]
    );
    Ok(())
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

fn plane(x: f64, y: f64) -> f64 {
    0.125 * x - 0.25 * y + 3.0
}

/// Square on a plane with seeded spots and many crossing breaklines.
fn seeded(seed: u64) -> (Vec<f64>, Vec<Vec<u32>>) {
    let mut random = Lcg(seed);
    let mut points: Vec<f64> = SQUARE
        .as_chunks::<3>()
        .0
        .iter()
        .flat_map(|&[x, y, _]| [x, y, plane(x, y)])
        .collect();
    let add = |random: &mut Lcg, points: &mut Vec<f64>| {
        let (x, y) = (0.5 + 9.0 * random.unit(), 0.5 + 9.0 * random.unit());
        points.extend([x, y, plane(x, y)]);
        u32::try_from(points.len() / 3 - 1).unwrap_or(u32::MAX)
    };
    for _ in 0..12 {
        add(&mut random, &mut points);
    }
    let lines = (0..10)
        .map(|line| {
            let count = 2 + line % 3;
            (0..count).map(|_| add(&mut random, &mut points)).collect()
        })
        .collect();
    (points, lines)
}

#[test]
fn seeded_breakline_networks_triangulate_verify_and_repeat() -> Result<(), Error> {
    for seed in 0..64_u64 {
        let (points, lines) = seeded(0x7E44_A100 + seed);
        let mut constraints = vec![boundary(&OUTER)];
        constraints.extend(lines.iter().map(|line| breakline(line)));
        let input = Request {
            z_tolerance: 1e-9,
            ..request(&points, &constraints)
        };
        let tin = triangulate(&input)?;
        assert_eq!(triangulate(&input)?, tin, "seed {seed} repeats");
        assert!(close(total_area(&tin), 100.0, 1e-9), "seed {seed} area");
        assert_eq!(euler(&tin)?, 1, "seed {seed} is a disk");
        for face in faces(&tin) {
            assert!(signed_area(&tin, face) > 0.0, "seed {seed} face {face:?}");
            assert!(face[0] < face[1] && face[0] < face[2]);
        }
        assert!(tin.triangles.as_chunks::<3>().0.is_sorted());
        for vertex in 0..u32::try_from(tin.points.len() / 3).unwrap_or(0) {
            let [x, y, z] = point(&tin, vertex);
            assert!(close(z, plane(x, y), 1e-9), "seed {seed} vertex {vertex}");
        }
        for crossing in &tin.crossings {
            assert!(crossing.segments[0] < crossing.segments[1]);
        }
        assert!(tin.constraint_edges.iter().all(|row| row.kept));
    }
    Ok(())
}

/// FNV-1a over every output list of the seeded networks.
fn digest(tins: &[Tin]) -> u64 {
    let mut hash = 0xCBF2_9CE4_8422_2325_u64;
    let mut feed = |value: u64| {
        for byte in value.to_le_bytes() {
            hash = (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01B3);
        }
    };
    for tin in tins {
        tin.points.iter().for_each(|value| feed(value.to_bits()));
        tin.triangles.iter().for_each(|&value| feed(value.into()));
        tin.face_boundary
            .iter()
            .for_each(|value| feed(value.map_or(u64::MAX, u64::from)));
        tin.input_vertices
            .iter()
            .for_each(|&value| feed(value.into()));
        for crossing in &tin.crossings {
            feed(crossing.vertex.into());
            for segment in crossing.segments {
                feed(segment.constraint.into());
                feed(segment.segment.into());
            }
        }
        for row in &tin.constraint_edges {
            feed(row.segment.constraint.into());
            feed(row.segment.segment.into());
            feed(row.vertices[0].into());
            feed(row.vertices[1].into());
            feed(row.kept.into());
        }
    }
    hash
}

/// Pinned against the vendored `spade` 2.15.1. A change here across runs means
/// hash-ordered or scheduling-dependent output; across a `spade` update it is
/// expected and the pin moves with the vendoring commit.
#[test]
fn seeded_networks_match_the_pinned_digest() -> Result<(), Error> {
    let mut tins = Vec::new();
    for seed in 0..16_u64 {
        let (points, lines) = seeded(0x00D1_6E57 + seed);
        let mut constraints = vec![boundary(&OUTER)];
        constraints.extend(lines.iter().map(|line| breakline(line)));
        tins.push(triangulate(&request(&points, &constraints))?);
    }
    assert_eq!(digest(&tins), 14_121_222_549_121_236_847);
    Ok(())
}
