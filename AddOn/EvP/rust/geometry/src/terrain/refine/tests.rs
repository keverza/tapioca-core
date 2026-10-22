use super::*;
use crate::terrain::{Constraint, Request, Role, Tin, triangulate, verify_constraints};

const RING: [u32; 4] = [0, 1, 2, 3];
const SQUARE: [f64; 12] = [
    0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0, 8.0, 0.0, 10.0, 0.0,
];

fn parameters() -> Refinement {
    Refinement {
        min_angle_degrees: 20.0,
        max_area_square_metres: Some(2.0),
        max_added_vertices: 10_000,
    }
}

fn request<'a>(points: &'a [f64], constraints: &'a [Constraint<'a>]) -> Request<'a> {
    Request {
        points,
        constraints,
        z_tolerance: 1e-9,
        refinement: None,
    }
}

fn point(tin: &Tin, vertex: u32) -> [f64; 3] {
    tin.points
        .as_chunks::<3>()
        .0
        .get(vertex as usize)
        .copied()
        .unwrap_or([f64::NAN; 3])
}

fn weights([a, b, c]: [[f64; 3]; 3], sample: [f64; 3]) -> [f64; 3] {
    let [dx, dy] = [b[0] - a[0], b[1] - a[1]];
    let [ex, ey] = [c[0] - a[0], c[1] - a[1]];
    let [px, py] = [sample[0] - a[0], sample[1] - a[1]];
    let determinant = dx * ey - dy * ex;
    let second = (px * ey - py * ex) / determinant;
    let third = (dx * py - dy * px) / determinant;
    [1.0 - second - third, second, third]
}

fn height(tin: &Tin, p: [f64; 3]) -> Option<f64> {
    tin.triangles.as_chunks::<3>().0.iter().find_map(|face| {
        let corners = face.map(|vertex| point(tin, vertex));
        let weights = weights(corners, p);
        weights.iter().all(|&w| w >= -1e-8).then(|| {
            corners
                .into_iter()
                .zip(weights)
                .map(|(corner, weight)| corner[2] * weight)
                .sum()
        })
    })
}

fn prove_surface(original: &Tin, refined: &Tin, tolerance: f64) {
    assert_eq!(refined.input_vertices, original.input_vertices);
    assert_eq!(refined.crossings, original.crossings);
    assert_eq!(
        refined.points.get(..original.points.len()),
        Some(original.points.as_slice())
    );
    assert_eq!(
        refined.points.len() / 3,
        original.points.len() / 3 + refined.steiner.len()
    );
    for face in refined.triangles.as_chunks::<3>().0 {
        let corners = face.map(|vertex| point(refined, vertex));
        for barycentric in [
            [1.0, 0.0, 0.0],
            [0.0, 1.0, 0.0],
            [0.0, 0.0, 1.0],
            [0.2, 0.3, 0.5],
            [0.7, 0.2, 0.1],
        ] {
            // Anchor plan arithmetic locally for survey coordinates.
            let mut sample = [0.0; 3];
            for ((value, (a, b)), c) in sample
                .iter_mut()
                .zip(corners[0].into_iter().zip(corners[1]))
                .zip(corners[2])
            {
                *value = a + barycentric[1] * (b - a) + barycentric[2] * (c - a);
            }
            assert!(
                height(original, sample).is_some_and(|z| (z - sample[2]).abs() < tolerance),
                "surface changed at {sample:?}"
            );
        }
    }
    let count = u32::try_from(original.points.len() / 3).unwrap_or(0);
    for (vertex, added) in (count..).zip(&refined.steiner) {
        assert_eq!(added.vertex, vertex);
        match &added.origin {
            SteinerOrigin::Segments(segments) => {
                assert_ne!(segments.as_slice(), []);
                assert!(
                    segments
                        .windows(2)
                        .all(|pair| matches!(pair, [a, b] if a < b))
                );
                for segment in segments {
                    assert!(
                        refined
                            .constraint_edges
                            .iter()
                            .any(|row| row.segment == *segment && row.vertices.contains(&vertex))
                    );
                }
            }
            SteinerOrigin::Edge(edge) => assert!(edge.iter().all(|&v| v < count)),
            SteinerOrigin::Face(face) => assert!(face.iter().all(|&v| v < count)),
        }
    }
}

#[test]
fn area_refinement_preserves_a_nonplanar_surface_not_just_vertex_heights() -> Result<(), Error> {
    let constraints = [Constraint {
        role: Role::Boundary,
        vertices: &RING,
    }];
    let mut input = request(&SQUARE, &constraints);
    let original = triangulate(&input)?;
    input.refinement = Some(parameters());
    let refined = triangulate(&input)?;
    assert_ne!(refined.steiner.as_slice(), []);
    prove_surface(&original, &refined, 1e-10);
    verify_constraints(&input, &refined)?;
    for face in refined.triangles.as_chunks::<3>().0 {
        let [a, b, c] = face.map(|vertex| point(&refined, vertex));
        let area = 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
        assert!(area > 0.0 && area <= 2.0 + 1e-12);
    }
    assert!(
        refined
            .steiner
            .iter()
            .any(|point| matches!(point.origin, SteinerOrigin::Segments(_)))
    );
    assert!(
        refined
            .steiner
            .iter()
            .any(|point| matches!(point.origin, SteinerOrigin::Edge(_)))
    );
    assert!(
        refined
            .steiner
            .iter()
            .any(|point| matches!(point.origin, SteinerOrigin::Face(_)))
    );
    assert_eq!(triangulate(&input)?, refined);
    Ok(())
}

#[test]
fn holes_islands_breaklines_and_overlapping_support_keep_winding_semantics() -> Result<(), Error> {
    let points = [
        0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0, 0.0, 0.0, 10.0, 0.0, 2.0, 2.0, 0.0, 8.0, 2.0,
        0.0, 8.0, 8.0, 0.0, 2.0, 8.0, 0.0, 4.0, 4.0, 0.0, 6.0, 4.0, 0.0, 6.0, 6.0, 0.0, 4.0, 6.0,
        0.0, 0.0, 1.0, 0.0, 10.0, 1.0, 0.0,
    ];
    let constraints = [
        Constraint {
            role: Role::Boundary,
            vertices: &RING,
        },
        Constraint {
            role: Role::Hole,
            vertices: &[4, 5, 6, 7],
        },
        Constraint {
            role: Role::Boundary,
            vertices: &[8, 9, 10, 11],
        },
        Constraint {
            role: Role::Breakline,
            vertices: &[12, 13],
        },
        Constraint {
            role: Role::Breakline,
            vertices: &[13, 12],
        },
    ];
    let mut input = request(&points, &constraints);
    let original = triangulate(&input)?;
    input.refinement = Some(Refinement {
        min_angle_degrees: 0.0,
        ..parameters()
    });
    let refined = triangulate(&input)?;
    prove_surface(&original, &refined, 1e-10);
    verify_constraints(&input, &refined)?;
    assert!(refined.face_boundary.contains(&Some(2)));
    assert!(refined.steiner.iter().any(|point| matches!(&point.origin,
        SteinerOrigin::Segments(segments) if segments.len() == 2)));
    Ok(())
}

#[test]
fn angle_only_refinement_is_optional_and_disabled_mode_keeps_its_mapping() -> Result<(), Error> {
    let points = [0.0, 0.0, 0.0, 20.0, 0.0, 0.0, 20.0, 5.0, 4.0, 0.0, 5.0, 0.0];
    let constraints = [Constraint {
        role: Role::Boundary,
        vertices: &RING,
    }];
    let mut input = request(&points, &constraints);
    let original = triangulate(&input)?;
    assert_eq!(original.steiner, []);
    input.refinement = Some(Refinement {
        max_area_square_metres: None,
        ..parameters()
    });
    let refined = triangulate(&input)?;
    assert_ne!(refined.steiner.as_slice(), []);
    prove_surface(&original, &refined, 1e-10);
    verify_constraints(&input, &refined)?;
    input.refinement = None;
    assert_eq!(triangulate(&input)?, original);
    Ok(())
}

#[test]
fn invalid_parameters_and_exhaustion_return_no_partial_tin() {
    let input = request(&SQUARE, &[]);
    for min_angle_degrees in [-1.0, 30.1, f64::NAN, f64::INFINITY] {
        assert_eq!(
            triangulate(&Request {
                refinement: Some(Refinement {
                    min_angle_degrees,
                    ..parameters()
                }),
                ..input
            }),
            Err(Error::RefinementParameters)
        );
    }
    for area in [0.0, -1.0, f64::NAN, f64::INFINITY] {
        assert_eq!(
            triangulate(&Request {
                refinement: Some(Refinement {
                    max_area_square_metres: Some(area),
                    ..parameters()
                }),
                ..input
            }),
            Err(Error::RefinementParameters)
        );
    }
    assert_eq!(
        triangulate(&Request {
            refinement: Some(Refinement {
                max_added_vertices: u32::MAX,
                ..parameters()
            }),
            ..input
        }),
        Err(Error::RefinementParameters)
    );
    for max_added_vertices in [0, 1, 2] {
        assert_eq!(
            triangulate(&Request {
                refinement: Some(Refinement {
                    max_added_vertices,
                    ..parameters()
                }),
                ..input
            }),
            Err(Error::RefinementLimit)
        );
    }
}

#[test]
fn seeded_survey_surfaces_preserve_heights_and_mapping() -> Result<(), Error> {
    for seed in 0..32_u32 {
        let offset = if seed % 2 == 0 { 0.0 } else { 500_000.0 };
        let mut points = Vec::new();
        for [x, y, z] in SQUARE.as_chunks::<3>().0 {
            points.extend([x + offset, y + offset, *z]);
        }
        let mut state = seed + 1;
        for _ in 0..8 {
            let mut next = || {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                f64::from(state) / f64::from(u32::MAX)
            };
            points.extend([
                offset + 1.0 + 8.0 * next(),
                offset + 1.0 + 8.0 * next(),
                10.0 * next(),
            ]);
        }
        let constraints = [Constraint {
            role: Role::Boundary,
            vertices: &RING,
        }];
        let mut input = request(&points, &constraints);
        let original = triangulate(&input)?;
        input.refinement = Some(Refinement {
            min_angle_degrees: 0.0,
            ..parameters()
        });
        let refined = triangulate(&input)?;
        prove_surface(&original, &refined, 1e-7);
        assert_eq!(triangulate(&input)?, refined, "seed {seed}");
        verify_constraints(&input, &refined)?;
    }
    Ok(())
}

#[test]
fn oblique_crossing_constraints_keep_paths_and_surface_at_survey_coordinates() -> Result<(), Error>
{
    for seed in 0..16_u32 {
        let angle = 0.13 + f64::from(seed) * 0.07;
        let (sin, cos) = angle.sin_cos();
        let mut points = Vec::new();
        for [x, y, z] in [
            [0.0, 0.0, 0.0],
            [10.0, 0.0, 0.0],
            [10.0, 10.0, 0.0],
            [0.0, 10.0, 0.0],
            [1.0, 1.5, 0.0],
            [9.0, 8.5, 0.0],
            [2.0, 8.0, 0.0],
            [8.0, 2.0, 0.0],
            [3.0, 4.0, 5.0],
        ] {
            points.extend([
                500_000.0 + x * cos - y * sin,
                6_000_000.0 + x * sin + y * cos,
                z,
            ]);
        }
        let constraints = [
            Constraint {
                role: Role::Boundary,
                vertices: &RING,
            },
            Constraint {
                role: Role::Breakline,
                vertices: &[4, 5],
            },
            Constraint {
                role: Role::Breakline,
                vertices: &[6, 7],
            },
        ];
        let mut input = request(&points, &constraints);
        let original = triangulate(&input);
        assert!(original.is_ok(), "original seed {seed}: {original:?}");
        let original = original?;
        input.refinement = Some(Refinement {
            min_angle_degrees: 0.0,
            ..parameters()
        });
        let refined = triangulate(&input);
        assert!(refined.is_ok(), "refined seed {seed}: {refined:?}");
        let refined = refined?;
        prove_surface(&original, &refined, 1e-7);
        verify_constraints(&input, &refined)?;
        assert_eq!(triangulate(&input)?, refined);
    }
    Ok(())
}

#[test]
fn refined_fixture_has_a_pinned_digest() -> Result<(), Error> {
    let constraints = [Constraint {
        role: Role::Boundary,
        vertices: &RING,
    }];
    let tin = triangulate(&Request {
        refinement: Some(parameters()),
        ..request(&SQUARE, &constraints)
    })?;
    // Debug records include every ordered output/support field; no hash maps.
    let mut hash = 0xCBF2_9CE4_8422_2325_u64;
    for byte in format!("{tin:?}").bytes() {
        hash = (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01B3);
    }
    assert_eq!(hash, 10_368_858_918_537_117_607);
    Ok(())
}
