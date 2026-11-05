use super::*;

const HALF: Slope = Slope::RiseRun {
    rise: 1.0,
    run: 2.0,
};

/// A 5-m grid over `[-20.5, 59.5] x [-30.5, 29.5]` at 10 m.
fn base() -> (Vec<f64>, Vec<u32>) {
    let (columns, rows) = (17_u32, 13_u32);
    let mut points = Vec::new();
    for row in 0..rows {
        for column in 0..columns {
            points.extend([
                -20.5 + 5.0 * f64::from(column),
                -30.5 + 5.0 * f64::from(row),
                10.0,
            ]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..rows - 1 {
        for column in 0..columns - 1 {
            let a = row * columns + column;
            triangles.extend([a, a + 1, a + columns + 1, a, a + columns + 1, a + columns]);
        }
    }
    (points, triangles)
}

const REGION: [f64; 12] = [
    -10.0, -15.0, 0.0, 50.0, -15.0, 0.0, 50.0, 15.0, 0.0, -10.0, 15.0, 0.0,
];
/// A level 40 x 4 m pavement at 12 m: 2 m above the ground.
const PAVED: [f64; 12] = [
    0.0, -2.0, 12.0, 40.0, -2.0, 12.0, 40.0, 2.0, 12.0, 0.0, 2.0, 12.0,
];
const PAVED_TRIANGLES: [u32; 6] = [0, 1, 2, 0, 2, 3];

/// The rectangle grown by `d` as two open halves (a breakline is an open line).
fn ring(d: f64) -> [Vec<f64>; 2] {
    let (x0, y0, x1, y1) = (-d, -2.0 - d, 40.0 + d, 2.0 + d);
    [
        vec![x0, y0, 0.0, x1, y0, 0.0, x1, y1, 0.0],
        vec![x1, y1, 0.0, x0, y1, 0.0, x0, y0, 0.0],
    ]
}

fn run(rounding: f64, limit: f64) -> Result<Outlined, OutlineError> {
    let (points, triangles) = base();
    let rings: Vec<Vec<f64>> = [0.0, 0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0]
        .into_iter()
        .flat_map(ring)
        .collect();
    let lines: Vec<&[f64]> = rings.iter().map(Vec::as_slice).collect();
    build(&OutlineRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        breaklines: &lines,
        spots: &[],
        surface_grades: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    })
}

/// The proposed height at the output vertex nearest `(x, y)`.
fn at(built: &Outlined, x: f64, y: f64) -> f64 {
    plan::vertices(&built.points)
        .iter()
        .min_by(|a, b| {
            (a[0] - x)
                .hypot(a[1] - y)
                .total_cmp(&(b[0] - x).hypot(b[1] - y))
        })
        .map_or(f64::NAN, |p| p[2])
}

#[test]
fn the_pavement_is_kept_and_its_faces_fall_to_the_ground() -> Result<(), OutlineError> {
    let built = run(0.0, 10.0)?;
    // On the pavement, its own height; its fill face falls 1 in 2 to meet the
    // ground 4 m out, and the ground stays beyond.
    assert!((at(&built, 20.0, 2.0) - 12.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 3.0) - 11.5).abs() < 1e-9);
    assert!((at(&built, 20.0, 4.0) - 11.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 6.0) - 10.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 7.0) - 10.0).abs() < 1e-9);
    // Around the ends too: the area's own outline, not a band that stops.
    assert!((at(&built, 42.0, 0.0) - 11.0).abs() < 1e-9);
    assert!((at(&built, -2.0, 0.0) - 11.0).abs() < 1e-9);
    assert_eq!(built.unreached, 0);
    assert!(built.volumes.site.fill > 0.0);
    assert!(built.volumes.site.cut.abs() < 1e-9);
    assert!(built.volumes.context.fill.abs() < 1e-12);
    Ok(())
}

#[test]
fn rounding_eases_the_top_and_keeps_the_straight_face() -> Result<(), OutlineError> {
    let built = run(1.0, 10.0)?;
    // A quarter metre out the rounded top has barely left the pavement's level
    // (the sharp face would be 0.125 m down); the straight face is unchanged.
    let quarter = at(&built, 20.0, 2.25);
    assert!(quarter < 12.0 && 12.0 - quarter < 0.06, "{quarter}");
    assert!((at(&built, 20.0, 4.0) - 11.0).abs() < 1e-9);
    Ok(())
}

#[test]
fn a_face_cut_short_by_its_limit_is_counted() -> Result<(), OutlineError> {
    let built = run(0.0, 1.0)?;
    assert!(built.unreached > 0);
    assert_eq!(built.clipped, 0);
    Ok(())
}

#[test]
fn a_face_cut_short_by_the_region_is_counted() -> Result<(), OutlineError> {
    // The fill face needs 4 m beside the pavement; this region leaves 1 m.
    let (points, triangles) = base();
    let region = [
        -10.0, -3.0, 0.0, 50.0, -3.0, 0.0, 50.0, 15.0, 0.0, -10.0, 15.0, 0.0,
    ];
    let built = build(&OutlineRequest {
        points: &points,
        triangles: &triangles,
        region: &region,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        breaklines: &[],
        spots: &[],
        surface_grades: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding: 0.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    })?;
    assert!(built.clipped > 0);
    assert_eq!(built.unreached, 0);
    assert_eq!(run(0.0, 10.0)?.clipped, 0);
    Ok(())
}

/// An L of two 4-m-wide legs on level ground, the corner leg's far end
/// higher: the faces' heights over the inside corner.
fn inside_corner(px: f64, py: f64) -> Option<f64> {
    // Leg A along x at y in [-2, 2] from x=0 to 20 (12 m), leg B along y at
    // x in [16, 20] from y=2 to 20 falling from 12 to 11.
    let surface = [
        0.0, -2.0, 12.0, 20.0, -2.0, 12.0, 20.0, 2.0, 12.0, 0.0, 2.0, 12.0, 16.0, 2.0, 12.0, 16.0,
        20.0, 11.0, 20.0, 20.0, 11.0,
    ];
    let surface_triangles = [0_u32, 1, 2, 0, 2, 4, 0, 4, 3, 4, 2, 6, 4, 6, 5];
    let (points, triangles) = base();
    let base = Field::new(&points, &triangles).ok()?;
    let paved = Field::new(&surface, &surface_triangles).ok()?;
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding: 0.0,
    };
    let edges = outline(&surface, &surface_triangles, &[], uniform);
    let faces = Faces {
        base: &base,
        surface: &paved,
        outline: &edges,
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding: 0.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    };
    faces.height(px, py).map(|(height, _)| height)
}

#[test]
fn faces_meeting_at_an_inside_corner_stay_continuous_and_within_grade() -> Result<(), OutlineError>
{
    // Walk the inside corner's bisector region on a fine grid: every step's
    // height change stays within the 1 in 2 grade, where the nearest-point
    // faces jumped between the two legs' edge heights.
    let step = 0.05;
    for i in 0..120 {
        for j in 0..120 {
            let (x, y) = (
                f64::from(i).mul_add(step, 10.0),
                f64::from(j).mul_add(step, 2.5),
            );
            let here = inside_corner(x, y).ok_or(OutlineError::Coverage)?;
            for (nx, ny) in [(x + step, y), (x, y + step)] {
                let next = inside_corner(nx, ny).ok_or(OutlineError::Coverage)?;
                assert!(
                    (next - here).abs() <= 0.5f64.mul_add(step, 1e-9),
                    "({x},{y}) -> ({nx},{ny}): {here} -> {next}"
                );
            }
        }
    }
    Ok(())
}

#[test]
fn the_steepest_point_of_an_edge_is_its_cone_extremum() {
    // A 20 m edge rising 1 in 10 under a 1 in 2 cone, queried 3 m off it.
    let (from, to) = ([0.0, 0.0, 0.0], [20.0, 0.0, 2.0]);
    for (px, cut) in [(-5.0, false), (4.0, false), (12.0, true), (25.0, true)] {
        let (distance, edge) = steepest([from, to], [px, 3.0], 0.5, cut);
        let best = |t: f64| {
            let (d, h) = ((t - px).hypot(3.0), 0.1 * t);
            if cut { h + 0.5 * d } else { h - 0.5 * d }
        };
        let found = if cut {
            edge + 0.5 * distance
        } else {
            edge - 0.5 * distance
        };
        let mut t = 0.0;
        while t <= 20.0 {
            let other = best(t);
            assert!(if cut {
                found <= other + 1e-12
            } else {
                found >= other - 1e-12
            });
            t += 0.01;
        }
    }
}

/// Two affine halves meeting at y=0, with a longitudinal 5% grade.
fn crowned(offset: f64, crossfall: f64) -> Vec<f64> {
    [
        (0.0, -2.0),
        (40.0, -2.0),
        (0.0, 0.0),
        (40.0, 0.0),
        (0.0, 2.0),
        (40.0, 2.0),
    ]
    .into_iter()
    .flat_map(|(x, y): (f64, f64)| {
        [
            x + offset,
            y + offset,
            10.0 + 0.05 * x - crossfall * y.abs(),
        ]
    })
    .collect()
}

const CROWN_TRIANGLES: [u32; 12] = [0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4];

#[test]
fn crown_and_valley_preserve_affine_pieces_between_samples() -> Result<(), OutlineError> {
    // Fixed-seed translation property, with binary-exact offsets so the analytic
    // oracle does not acquire its own coordinate quantization error.
    let mut offsets = vec![0.0, 1_000_000.0, -1_000_000.0];
    let mut seed = 115_u32;
    for _ in 0..8 {
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        offsets.push(f64::from(seed % 32_000_000) / 16.0 - 1_000_000.0);
    }
    for offset in offsets {
        for crossfall in [0.02, -0.02] {
            let (mut points, triangles) = base();
            for p in points.as_chunks_mut::<3>().0 {
                p[2] = 10.0 + 0.05 * p[0] - 0.2 * p[1];
                p[0] += offset;
                p[1] += offset;
            }
            let surface = crowned(offset, crossfall);
            let region: Vec<f64> = plan::vertices(&REGION)
                .iter()
                .flat_map(|p| [p[0] + offset, p[1] + offset, 0.0])
                .collect();
            let rings = ring(0.0).map(|line| {
                plan::vertices(&line)
                    .iter()
                    .flat_map(|p| [p[0] + offset, p[1] + offset, 0.0])
                    .collect::<Vec<_>>()
            });
            let lines: Vec<&[f64]> = rings.iter().map(Vec::as_slice).collect();
            let built = build(&OutlineRequest {
                points: &points,
                triangles: &triangles,
                region: &region,
                surface_points: &surface,
                surface_triangles: &CROWN_TRIANGLES,
                breaklines: &lines,
                spots: &surface,
                surface_grades: &[],
                grading: Grading {
                    cut: HALF,
                    fill: HALF,
                    limit: 10.0,
                    rounding: 0.0,
                    effect: Effect::Both,
                    tolerance: 1e-6,
                },
            })?;
            let field = Field::new(&built.points, &built.mesh.triangles)
                .map_err(|_| OutlineError::Field)?;
            for x in [1.0, 12.3, 20.0, 35.7, 39.0] {
                for y in [-1.7_f64, -1.0, -0.1, 0.0, 0.1, 1.0, 1.7] {
                    let expected = 10.0 + 0.05 * x - crossfall * y.abs();
                    let actual = field
                        .height_at(x + offset, y + offset)
                        .ok_or(OutlineError::Coverage)?
                        .height;
                    assert!(
                        (actual - expected).abs() < 1e-8,
                        "offset={offset} crossfall={crossfall} ({x},{y}): {actual} != {expected}"
                    );
                }
            }
        }
    }
    Ok(())
}

#[test]
fn support_preparation_does_not_sanitize_a_malformed_region() -> Result<(), OutlineError> {
    let (points, triangles) = base();
    let surface = crowned(0.0, 0.02);
    let request = OutlineRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        surface_points: &surface,
        surface_triangles: &CROWN_TRIANGLES,
        breaklines: &[],
        spots: &[],
        surface_grades: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding: 0.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    };
    let mut ragged = REGION.to_vec();
    ragged.push(0.0);
    let mut nonfinite = REGION;
    nonfinite[2] = f64::NAN; // ignored z retains the finite-ingress contract
    for region in [ragged.as_slice(), nonfinite.as_slice(), &REGION[..6], &[]] {
        assert_eq!(
            build(&OutlineRequest { region, ..request }),
            Err(OutlineError::Region(RegionError::Terrain(
                crate::terrain::Error::PropertyLineOutside
            )))
        );
    }
    let reversed: Vec<f64> = plan::vertices(&REGION)
        .iter()
        .rev()
        .flatten()
        .copied()
        .collect();
    let built = build(&OutlineRequest {
        region: &reversed,
        ..request
    })?;
    check_stitch(
        &built,
        &Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?,
    )?;
    Ok(())
}

#[test]
fn authored_line_grazing_an_existing_concave_stitch_vertex_still_refuses() {
    let (points, triangles) = base();
    let surface = crowned(0.0, 0.02);
    let region = [
        -5.0, -10.0, 0.0, 45.0, -10.0, 0.0, 45.0, 10.0, 0.0, 22.0, 10.0, 0.0, 22.0, 0.0, 0.0, 18.0,
        0.0, 0.0, 18.0, 10.0, 0.0, -5.0, 10.0, 0.0,
    ];
    // Both endpoints are strictly inside; the segment only touches (18,0),
    // an existing stitch vertex also reached by private crown supports.
    let line = [15.0, 3.0, 0.0, 21.0, -3.0, 0.0];
    assert_eq!(
        build(&OutlineRequest {
            points: &points,
            triangles: &triangles,
            region: &region,
            surface_points: &surface,
            surface_triangles: &CROWN_TRIANGLES,
            breaklines: &[&line],
            spots: &[],
            surface_grades: &[],
            grading: Grading {
                cut: HALF,
                fill: HALF,
                limit: 10.0,
                rounding: 0.0,
                effect: Effect::Both,
                tolerance: 1e-6
            },
        }),
        Err(OutlineError::Region(RegionError::BoundaryCrossed))
    );
}

/// Check a real (non-vacuous) exterior, base-parent interpolation and shared
/// stitch incidence. Fixed boundary heights deliberately override the pavement.
fn check_stitch(built: &Outlined, base: &Field) -> Result<(), OutlineError> {
    let vertices = plan::vertices(&built.mesh.points);
    let mut uses = BTreeMap::<[u32; 2], (u32, u32)>::new();
    let mut outside = 0;
    for (&triple, &inside) in built
        .mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&built.mesh.inside)
    {
        for (a, b) in [
            (triple[0], triple[1]),
            (triple[1], triple[2]),
            (triple[2], triple[0]),
        ] {
            let entry = uses.entry([a.min(b), a.max(b)]).or_default();
            entry.0 += u32::from(inside);
            entry.1 += u32::from(!inside);
        }
        if !inside {
            outside += 1;
            for v in triple {
                let original = vertices.get(v as usize).ok_or(OutlineError::Coverage)?;
                let proposed = plan::vertices(&built.points)
                    .get(v as usize)
                    .ok_or(OutlineError::Coverage)?;
                assert_eq!(original.map(f64::to_bits), proposed.map(f64::to_bits));
            }
        }
    }
    assert!(outside > 0);
    for (index, &a) in built.mesh.ring.iter().enumerate() {
        let b = built
            .mesh
            .ring
            .get((index + 1) % built.mesh.ring.len())
            .ok_or(OutlineError::Coverage)?;
        assert_eq!(uses.get(&[a.min(*b), a.max(*b)]), Some(&(1, 1)));
    }
    for &[x, y, z] in vertices {
        let height = base.height_at(x, y).ok_or(OutlineError::Coverage)?.height;
        assert!((z - height).abs() < 1e-8);
    }
    for (&triple, &parent) in built
        .mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&built.mesh.parents)
    {
        let mut centre = [0.0; 2];
        for v in triple {
            let p = vertices.get(v as usize).ok_or(OutlineError::Coverage)?;
            centre[0] += p[0] / 3.0;
            centre[1] += p[1] / 3.0;
        }
        assert_eq!(
            base.height_at(centre[0], centre[1])
                .ok_or(OutlineError::Coverage)?
                .face,
            parent
        );
    }
    Ok(())
}

#[test]
fn clipped_touching_and_coincident_supports_share_a_conforming_stitch() -> Result<(), OutlineError>
{
    // Crossing ends; touching ends; coincident sides; concave notch through the
    // crown crease. No caller spots/lines hide the source-support responsibility.
    let regions: &[&[[f64; 2]]] = &[
        &[[5.0, -10.0], [35.0, -10.0], [35.0, 10.0], [5.0, 10.0]],
        &[[0.0, -10.0], [40.0, -10.0], [40.0, 10.0], [0.0, 10.0]],
        &[[-5.0, -2.0], [45.0, -2.0], [45.0, 2.0], [-5.0, 2.0]],
        &[
            [-5.0, -10.0],
            [45.0, -10.0],
            [45.0, 10.0],
            [22.0, 10.0],
            [22.0, 0.0],
            [18.0, 0.0],
            [18.0, 10.0],
            [-5.0, 10.0],
        ],
    ];
    for offset in [0.0, 1_000_000.0, -1_000_000.0] {
        let (mut points, triangles) = base();
        for p in points.as_chunks_mut::<3>().0 {
            p[2] = 10.0 + 0.05 * p[0] - 0.2 * p[1];
            p[0] += offset;
            p[1] += offset;
        }
        let base = Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?;
        for shape in regions {
            let region: Vec<_> = shape
                .iter()
                .flat_map(|p| [p[0] + offset, p[1] + offset, 0.0])
                .collect();
            for crossfall in [0.02, -0.02] {
                let surface = crowned(offset, crossfall);
                let request = OutlineRequest {
                    points: &points,
                    triangles: &triangles,
                    region: &region,
                    surface_points: &surface,
                    surface_triangles: &CROWN_TRIANGLES,
                    breaklines: &[],
                    spots: &[],
                    surface_grades: &[],
                    grading: Grading {
                        cut: HALF,
                        fill: HALF,
                        limit: 10.0,
                        rounding: 0.0,
                        effect: Effect::Both,
                        tolerance: 1e-6,
                    },
                };
                let built = build(&request)?;
                assert_eq!(built, build(&request)?);
                check_stitch(&built, &base)?;
                assert_eq!(
                    built.mesh.breakline_vertices.len(),
                    built.surface_supports.len()
                );
                assert!(
                    built.surface_supports.contains(&[2, 3]),
                    "crown support must not disappear when clipped"
                );
                for (line, source) in built.surface_supports.iter().enumerate() {
                    let start = plan::vertices(&surface)
                        .get(source[0] as usize)
                        .ok_or(OutlineError::Surface)?;
                    let end = plan::vertices(&surface)
                        .get(source[1] as usize)
                        .ok_or(OutlineError::Surface)?;
                    let chain = [*start, *end];
                    let rows: Vec<_> = built
                        .mesh
                        .line_edges
                        .iter()
                        .filter(|row| row.line as usize == line)
                        .collect();
                    assert_ne!(rows.len(), 0);
                    for row in rows {
                        for v in row.vertices {
                            let p = plan::vertices(&built.mesh.points)
                                .get(v as usize)
                                .ok_or(OutlineError::Coverage)?;
                            assert!(
                                plan::nearest(&chain, false, p[0], p[1])
                                    .is_some_and(|n| n.distance < 1e-8)
                            );
                        }
                    }
                }
                // Public controls are still strict, even if a private support
                // already introduced exactly this boundary vertex.
                let first = plan::vertices(&region)
                    .first()
                    .ok_or(OutlineError::Coverage)?;
                let invalid = [first[0], first[1], 0.0, offset + 10.0, offset - 1.0, 0.0];
                assert_eq!(
                    build(&OutlineRequest {
                        breaklines: &[&invalid],
                        ..request
                    }),
                    Err(OutlineError::Region(RegionError::Breakline))
                );
            }
        }
    }
    Ok(())
}

#[test]
fn each_outline_edge_grades_its_face_by_its_own_vertices() -> Result<(), OutlineError> {
    // The pavement's south side leaves at 1 in 2, its north side at 1 in 4
    // (two paths, or two cross-section ranges): each face keeps its own grade.
    let (points, triangles) = base();
    let steep = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding: 0.0,
    };
    let gentle = SurfaceGrade {
        fill: 0.25,
        ..steep
    };
    let grades = [steep, steep, gentle, gentle];
    let rings: Vec<Vec<f64>> = [0.0, 0.5, 1.0, 2.0, 4.0, 8.0]
        .into_iter()
        .flat_map(ring)
        .collect();
    let lines: Vec<&[f64]> = rings.iter().map(Vec::as_slice).collect();
    let request = OutlineRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        breaklines: &lines,
        spots: &[],
        surface_grades: &grades,
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding: 0.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    };
    let built = build(&request)?;
    assert!((at(&built, 20.0, -2.5) - 11.75).abs() < 1e-9);
    assert!((at(&built, 20.0, -4.0) - 11.0).abs() < 1e-9);
    assert!((at(&built, 20.0, 4.0) - 11.5).abs() < 1e-9);
    assert!((at(&built, 20.0, 10.0) - 10.0).abs() < 1e-9);
    // A wrong count, or an invalid grade, refuses rather than guessing.
    assert_eq!(
        build(&OutlineRequest {
            surface_grades: &grades[..3],
            ..request
        }),
        Err(OutlineError::Parameters)
    );
    let negative = [
        steep,
        steep,
        gentle,
        SurfaceGrade {
            fill: -0.1,
            ..gentle
        },
    ];
    assert_eq!(
        build(&OutlineRequest {
            surface_grades: &negative,
            ..request
        }),
        Err(OutlineError::Parameters)
    );
    Ok(())
}
