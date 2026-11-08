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

/// The rectangle grown by `d` as two open halves (a breakline is an open line),
/// a vertex every half metre along it as the editor's rings have.
fn ring(d: f64) -> [Vec<f64>; 2] {
    let (x0, y0, x1, y1) = (-d, -2.0 - d, 40.0 + d, 2.0 + d);
    let half = |corners: [[f64; 2]; 3]| {
        let mut line = Vec::new();
        for pair in corners.windows(2) {
            let &[[ax, ay], [bx, by]] = pair else {
                continue;
            };
            let pieces = ((bx - ax).hypot(by - ay) / 0.5).ceil().max(1.0);
            let mut k = 0.0;
            while k < pieces {
                let t = k / pieces;
                line.extend([(bx - ax).mul_add(t, ax), (by - ay).mul_add(t, ay), 0.0]);
                k += 1.0;
            }
        }
        let [x, y] = corners[2];
        line.extend([x, y, 0.0]);
        line
    };
    [
        half([[x0, y0], [x1, y0], [x1, y1]]),
        half([[x1, y1], [x0, y1], [x0, y0]]),
    ]
}

fn run(rounding: f64, limit: f64) -> Result<Outlined, OutlineError> {
    run_with(rounding, limit, None)
}

/// [`run`], refined to `refine` (#149 T3: the editor meshes the faces with [`Refine::DEFAULT`]).
fn run_with(rounding: f64, limit: f64, refine: Option<Refine>) -> Result<Outlined, OutlineError> {
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
        refine,
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

/// The proposed surface's height at `(x, y)`.
fn at(built: &Outlined, x: f64, y: f64) -> f64 {
    Field::new(&built.points, &built.mesh.triangles)
        .ok()
        .and_then(|surface| surface.height_at(x, y))
        .map_or(f64::NAN, |sample| sample.height)
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
        refine: None,
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
    inside_corner_rounded(px, py, 0.0)
}

fn inside_corner_rounded(px: f64, py: f64, rounding: f64) -> Option<f64> {
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
        rounding,
    };
    let edges = outline(&surface, &surface_triangles, &[], uniform);
    let faces = Faces::new(
        &base,
        &paved,
        &edges,
        Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    );
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
                refine: None,
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
        refine: None,
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
            refine: None,
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
                    refine: None,
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
                // The toe lines (#149 T2) come first, then the supports.
                let toe = built.toe as usize;
                assert_eq!(
                    built.mesh.breakline_vertices.len(),
                    toe + built.surface_supports.len()
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
                        .filter(|row| row.line as usize == toe + line)
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
        refine: None,
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
            refine: None,
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
            refine: None,
            ..request
        }),
        Err(OutlineError::Parameters)
    );
    Ok(())
}

mod blend;
mod crease;
mod reach;
mod refine;
mod toe;

/// #149 T1: the same paved area through the unified build, with region entries
/// `entries` composed first.
fn unified(
    rounding: f64,
    limit: f64,
    entries: &[crate::terrain::Entry<'_>],
) -> Result<crate::terrain::BuiltTerrain, crate::terrain::BuildError> {
    let (points, triangles) = base();
    let rings: Vec<Vec<f64>> = [0.0, 0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0]
        .into_iter()
        .flat_map(ring)
        .collect();
    let lines: Vec<&[f64]> = rings.iter().map(Vec::as_slice).collect();
    let outlines = [crate::terrain::Outline {
        order: u32::try_from(entries.len()).unwrap_or(u32::MAX),
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        surface_grades: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
        lines: &lines,
        spots: &[],
    }];
    crate::terrain::build_terrain(
        &crate::terrain::Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&REGION),
        },
        &crate::terrain::Plan {
            revision: 1,
            hash: 2,
            entries,
            paths: &[],
            masks: &[],
            controls: &[],
            control_tolerance: 1e-3,
            outlines: &outlines,
        },
    )
}

#[test]
fn the_unified_build_grades_an_outline_exactly_as_this_lane() -> Result<(), String> {
    for (rounding, limit) in [(0.0, 10.0), (1.0, 10.0), (1.0, 3.0)] {
        // Both refine: the unified build's outline stage always meshes (#149 T3).
        let lane =
            run_with(rounding, limit, Some(Refine::DEFAULT)).map_err(|e| format!("lane {e:?}"))?;
        let built = unified(rounding, limit, &[]).map_err(|e| format!("unified {e:?}"))?;
        assert_eq!(
            built.mesh.triangles, lane.mesh.triangles,
            "{rounding} {limit}"
        );
        assert_eq!(built.points.len(), lane.points.len());
        for (a, b) in plan::vertices(&built.points)
            .iter()
            .zip(plan::vertices(&lane.points))
        {
            assert_eq!(a[0].to_bits(), b[0].to_bits());
            assert_eq!(a[1].to_bits(), b[1].to_bits());
            assert!((a[2] - b[2]).abs() <= 1e-9, "{a:?} vs {b:?}");
        }
        assert_eq!(
            built.outline_unreached, lane.unreached,
            "{rounding} {limit}"
        );
        assert_eq!(built.outline_clipped, lane.clipped);
        assert!((built.volumes.site.fill - lane.volumes.site.fill).abs() < 1e-6);
        assert!((built.volumes.site.cut - lane.volumes.site.cut).abs() < 1e-6);
        // Every face line and every paved support is accounted for.
        assert_eq!(
            built.outline_lines.len(),
            built.mesh.breakline_vertices.len() - built.lines.len()
        );
        assert!(built.outline_lines.iter().any(|o| o.support.is_some()));
    }
    Ok(())
}

#[test]
fn an_outline_grades_over_the_ground_a_region_entry_composed_first() -> Result<(), String> {
    // A level 9 m pad north of the pavement, under its fill face (the ground is 10 m).
    let pad = [
        10.0, 3.0, 0.0, 30.0, 3.0, 0.0, 30.0, 10.0, 0.0, 10.0, 10.0, 0.0,
    ];
    let parts = [crate::terrain::Part {
        outer: &pad,
        holes: &[],
    }];
    let entries = [crate::terrain::Entry {
        order: 0,
        object: crate::identity::ObjectId(7),
        root_operation: crate::identity::OperationId(8),
        sources: &[],
        parts: &parts,
        surface: crate::terrain::Surface::Constant { level: 9.0 },
        terrain: crate::terrain::Terrain {
            effect: crate::terrain::TerrainEffect::Grade,
            transition: crate::terrain::Transition::Falloff { blur: 0.5 },
        },
    }];
    let built = unified(0.0, 10.0, &entries).map_err(|e| format!("unified {e:?}"))?;
    let lane = run(0.0, 10.0).map_err(|e| format!("lane {e:?}"))?;
    let height = |points: &[f64], triangles: &[u32], x: f64, y: f64| {
        Field::new(points, triangles)
            .ok()
            .and_then(|f| f.height_at(x, y))
            .map_or(f64::NAN, |s| s.height)
    };
    let h = |x, y| height(&built.points, &built.mesh.triangles, x, y);
    // The pavement and its face where the pad is not: as the lane.
    assert!((h(20.0, 2.0) - 12.0).abs() < 1e-9);
    assert!((h(20.0, -3.0) - 11.5).abs() < 1e-9);
    assert!((h(45.0, 8.0) - height(&lane.points, &lane.mesh.triangles, 45.0, 8.0)).abs() < 1e-9);
    // Over the pad: the face falls 1 in 2 from 12 m until it meets the pad at 9 m (6 m out,
    // y = 8), where the lane met the old ground at 10 m (4 m out); beyond, the pad (here at a
    // base vertex: the toe is not a mesh line yet, #149 T2).
    assert!((h(20.0, 7.0) - 9.5).abs() < 1e-9, "{}", h(20.0, 7.0));
    assert!((h(19.5, 9.5) - 9.0).abs() < 1e-9, "{}", h(19.5, 9.5));
    assert!((height(&lane.points, &lane.mesh.triangles, 20.0, 7.0) - 10.0).abs() < 1e-9);
    // The face vertex is the outline's, the pad's own vertex the entry's.
    let owner = |x: f64, y: f64| {
        plan::vertices(&built.points)
            .iter()
            .position(|&[px, py, _]| (px - x).abs() < 1e-9 && (py - y).abs() < 1e-9)
            .and_then(|v| built.owners.get(v).copied().flatten())
    };
    assert_eq!(owner(20.0, 7.0), Some(1));
    assert_eq!(owner(10.0, 10.0), Some(0));
    Ok(())
}

/// #175: the field preview's contours lie on the mesh the unified build makes from the same
/// plan, to within its refinement tolerance and the grid's interpolation, and its volumes are
/// the build's to within the grid's resolution.
#[test]
fn the_field_preview_matches_the_unified_mesh() -> Result<(), String> {
    let built = unified(1.0, 10.0, &[]).map_err(|e| format!("unified {e:?}"))?;
    let mesh = Field::new(&built.points, &built.mesh.triangles).map_err(|e| format!("{e:?}"))?;
    let (points, triangles) = base();
    let outlines = [crate::terrain::Outline {
        order: 0,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        surface_grades: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding: 1.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
        lines: &[],
        spots: &[],
    }];
    let started = std::time::Instant::now();
    let previewed = crate::terrain::field_preview(
        &crate::terrain::Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&REGION),
        },
        &crate::terrain::Plan {
            revision: 1,
            hash: 2,
            entries: &[],
            paths: &[],
            masks: &[],
            controls: &[],
            control_tolerance: 1e-3,
            outlines: &outlines,
        },
        0.1,
        0.5,
        0.0,
    )
    .map_err(|e| format!("preview {e:?}"))?;
    let elapsed = started.elapsed();
    let traced = &previewed.contours;
    // 10 at the toe (the ground is level at 10, so that level bounds what rises above it), 10.5,
    // 11 and 11.5 around the pavement (level at 12, nothing above it).
    let levels: std::collections::BTreeSet<u64> = traced
        .lines
        .iter()
        .map(|line| line.level.to_bits())
        .collect();
    assert_eq!(
        levels.len(),
        4,
        "{:?}",
        traced.lines.iter().map(|l| l.level).collect::<Vec<_>>()
    );
    assert_eq!(traced.lines.len(), 4);
    let mut worst = 0.0_f64;
    for line in &traced.lines {
        assert!(line.closed, "level {}", line.level);
        for p in &line.points {
            let on = mesh.height_at(p[0], p[1]).ok_or("off the mesh")?.height;
            worst = worst.max((on - p[2]).abs());
        }
    }
    assert!(worst < 0.03, "worst {worst}");
    let site = built.volumes.site;
    assert!(
        (previewed.fill - site.fill).abs() <= 0.02 * site.fill.max(1.0),
        "fill {} vs {}",
        previewed.fill,
        site.fill
    );
    assert!(
        previewed.cut <= 0.02 * site.fill.max(1.0),
        "cut {}",
        previewed.cut
    );
    println!(
        "field preview: {} lines, {} surface triangles, fill {:.2} vs {:.2}, worst {worst:.4} m, {elapsed:?}",
        traced.lines.len(),
        previewed.triangles.len() / 3,
        previewed.fill,
        site.fill
    );
    Ok(())
}

/// #175: the preview reports what the build reports when the faces run out at their limit, and
/// widens the reach as the editor's job widens the build's.
/// #170: a face only 20 cm above level ground, less than its 33 cm vertical toe rounding, still
/// never exceeds the rounded top's 4/3 of its 33 % grade. Sample the exact field around the low
/// pavement's end, rather than judging it through triangles the mesher happened to leave.
#[test]
fn a_face_lower_than_its_rounding_keeps_the_grade() -> Result<(), OutlineError> {
    let (mut points, triangles) = base();
    for [_, _, z] in points.as_chunks_mut::<3>().0 {
        *z = -0.2;
    }
    let paved = [
        0.0, -2.0, 0.0, 40.0, -2.0, 0.0, 40.0, 2.0, 0.0, 0.0, 2.0, 0.0,
    ];
    let operator = Operator::new(
        &points,
        &triangles,
        &paved,
        &PAVED_TRIANGLES,
        &[],
        Grading {
            cut: HALF,
            fill: Slope::Percent(33.0),
            limit: 10.0,
            rounding: 1.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    )?;
    let faces = operator.faces();
    let step = 0.001;
    for i in 0..100 {
        for j in 0..200 {
            let (x, y) = (-0.5 + 0.01 * f64::from(i), 0.01 * f64::from(j));
            let dx = (faces.over(-0.2, x + step, y).0 - faces.over(-0.2, x - step, y).0).abs()
                / (2.0 * step);
            let dy = (faces.over(-0.2, x, y + step).0 - faces.over(-0.2, x, y - step).0).abs()
                / (2.0 * step);
            assert!(
                dx.hypot(dy) <= 0.33 * 4.0 / 3.0 + 1e-6,
                "({x},{y}): gradient {}",
                dx.hypot(dy)
            );
        }
    }
    Ok(())
}

/// The low-face toe cap is continuous when the highest face changes at a tie. The two collinear
/// edges have different heights but the same eased face height here; neither may bring its own
/// cap to the envelope, or their tied design would jump (#170).
#[test]
fn a_low_face_toe_span_does_not_depend_on_the_winning_edge() {
    let face = |index, height, edge| super::blend::Face {
        index,
        height,
        edge,
        toward: [1.0, 0.0],
        segment: [[0.0, 0.0, edge], [1.0, 0.0, edge]],
        inside: false,
        foot: [0.0, 0.0],
        distance: 1.0,
        grade: 0.5,
        rounding: 1.0,
    };
    let keep = &|_: &super::blend::Face| true;
    let below = [face(0, 0.05, 0.1), face(1, 0.05, 0.2)];
    let above = [face(2, 0.05, 0.1), face(1, 0.05, 0.2)];
    let a = envelope_toe(0.0, &below, keep, false, 1.0);
    let b = envelope_toe(0.0, &above, keep, false, 1.0);
    assert!((a.unwrap_or(f64::NAN) - toe(0.0, 0.05, false, 0.15)).abs() < 1e-15);
    assert!((a.unwrap_or(f64::NAN) - b.unwrap_or(f64::NAN)).abs() < 1e-12);
}

#[test]
fn the_field_preview_counts_and_widens_a_short_reach() -> Result<(), String> {
    let built = unified(1.0, 3.0, &[]).map_err(|e| format!("unified {e:?}"))?;
    assert!(
        built.outline_unreached > 0,
        "the fixture's faces are short at 3 m"
    );
    let (points, triangles) = base();
    let outlines = [crate::terrain::Outline {
        order: 0,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        surface_grades: &[],
        grading: Grading {
            cut: HALF,
            fill: HALF,
            limit: 3.0,
            rounding: 1.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
        lines: &[],
        spots: &[],
    }];
    let previewed = |max_reach: f64| {
        crate::terrain::field_preview(
            &crate::terrain::Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&REGION),
            },
            &crate::terrain::Plan {
                revision: 1,
                hash: 2,
                entries: &[],
                paths: &[],
                masks: &[],
                controls: &[],
                control_tolerance: 1e-3,
                outlines: &outlines,
            },
            0.25,
            0.0,
            max_reach,
        )
        .map_err(|e| format!("preview {e:?}"))
    };
    let short = previewed(0.0)?;
    assert!(short.unreached > 0);
    assert_eq!(short.reach, 3.0);
    let widened = previewed(100.0)?;
    assert_eq!(widened.unreached, 0);
    assert_eq!(
        widened.reach, 20.0,
        "four times, at least 20 m, as the job widens"
    );
    assert_eq!(widened.clipped, 0);
    Ok(())
}

/// #175: the preview's merged outline grades as the dense one does. A 40 x 4 m pavement split
/// every half metre, rising 2 % along its length and dipping 0.5 mm off a straight grade at its
/// middle, merges to a handful of edges. Along its sides the faces over them are the dense
/// faces' to within the merge tolerance; at its corners the crease between a side and an end
/// lifts a little differently from many pieces than from one edge, by a couple of centimetres.
#[test]
fn the_merged_outline_grades_as_the_dense_one() -> Result<(), OutlineError> {
    let (points, triangles) = base();
    let height = |x: f64| 0.02f64.mul_add(x, 12.0) + 0.0005 * (x - 20.0).abs().min(10.0) / 10.0;
    let mut paved = Vec::new();
    let count = 81_u32;
    for side in [-2.0, 2.0] {
        for i in 0..count {
            let x = 0.5 * f64::from(i);
            paved.extend([x, side, height(x)]);
        }
    }
    let mut paved_triangles = Vec::new();
    for i in 0..count - 1 {
        let (a, b, c, d) = (i, i + 1, count + i + 1, count + i);
        paved_triangles.extend([a, b, c, a, c, d]);
    }
    let operator = Operator::new(
        &points,
        &triangles,
        &paved,
        &paved_triangles,
        &[],
        Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding: 1.0,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    )?;
    let merged = operator.merged_edges(PREVIEW_MERGE);
    assert!(operator.edges.len() >= 160, "{}", operator.edges.len());
    assert!(merged.len() <= 12, "merged to {} edges", merged.len());
    let (dense, sparse) = (operator.faces(), operator.faces_over(&merged));
    let (mut sides, mut corners) = (0.0_f64, 0.0_f64);
    for row in 0..80 {
        for column in 0..180 {
            let (x, y) = (
                0.37f64.mul_add(f64::from(column), -12.0),
                0.37f64.mul_add(f64::from(row), -14.0),
            );
            let gap = (dense.over(10.0, x, y).0 - sparse.over(10.0, x, y).0).abs();
            if (3.0..=37.0).contains(&x) {
                sides = sides.max(gap);
            } else {
                corners = corners.max(gap);
            }
        }
    }
    assert!(sides <= 2.0 * PREVIEW_MERGE, "sides {sides}");
    assert!(corners <= 0.03, "corners {corners}");
    Ok(())
}
