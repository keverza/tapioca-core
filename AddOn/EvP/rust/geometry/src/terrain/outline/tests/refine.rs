//! #149 T3: the faces' vertical error bounded by refinement, not offset rings.

use super::*;
use crate::terrain::outline::refine::{Memo, SAMPLES, mix};

/// The ground tilted 5 % along x, so the toe wanders across the base triangles;
/// the pavement only (no offset rings) as the caller's line, with the T2 toe and
/// creases, refined to `refine`.
fn refined(rounding: f64, refine: Option<Refine>) -> Result<(Outlined, Vec<f64>), OutlineError> {
    refined_toe(
        rounding,
        refine,
        &[0.0],
        Some(crate::terrain::outline::toe::Toe::DEFAULT),
    )
}

/// The tilted scene with the pavement grown by each of `rings` as lines, the toe and creases
/// as `toe` asks, refined to `refine`.
fn refined_toe(
    rounding: f64,
    refine: Option<Refine>,
    rings: &[f64],
    toe: Option<crate::terrain::outline::toe::Toe>,
) -> Result<(Outlined, Vec<f64>), OutlineError> {
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.05f64.mul_add(p[0], 9.0);
    }
    let outline: Vec<Vec<f64>> = rings.iter().flat_map(|&d| ring(d)).collect();
    let lines: Vec<&[f64]> = outline.iter().map(Vec::as_slice).collect();
    // The toe and creases as `toe` asks (#149 T2); `None` measures refinement alone.
    let built = build_with(
        &OutlineRequest {
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
                limit: 10.0,
                rounding,
                effect: Effect::Both,
                tolerance: 1e-6,
            },
        },
        toe,
    )?;
    Ok((built, points))
}

/// The exact faces over the tilted ground, for measuring (leaked: a test-only 'static borrow).
fn exact(points: &[f64], rounding: f64) -> Result<Faces<'static>, OutlineError> {
    let base: &'static Field = Box::leak(Box::new(
        Field::new(points, &base().1).map_err(|_| OutlineError::Field)?,
    ));
    let surface: &'static Field = Box::leak(Box::new(
        Field::new(&PAVED, &PAVED_TRIANGLES).map_err(|_| OutlineError::Surface)?,
    ));
    let uniform = SurfaceGrade {
        cut: 0.5,
        fill: 0.5,
        rounding,
    };
    let edges: &'static [Edge] =
        Box::leak(outline(&PAVED, &PAVED_TRIANGLES, &[], uniform).into_boxed_slice());
    Ok(Faces::new(
        base,
        surface,
        edges,
        Grading {
            cut: HALF,
            fill: HALF,
            limit: 10.0,
            rounding,
            effect: Effect::Both,
            tolerance: 1e-6,
        },
    ))
}

/// Largest gap between the mesh and the exact faces over ten samples per
/// inside triangle (more than the refinement takes), and the fifth-percentile
/// smallest angle, degrees, of the inside triangles off the pavement.
fn measure(built: &Outlined, points: &[f64], rounding: f64) -> Result<(f64, f64), OutlineError> {
    let faces = exact(points, rounding)?;
    let corners = plan::vertices(&built.points);
    let mut gap = 0.0_f64;
    let mut angles = Vec::new();
    for (&[a, b, c], _) in built
        .mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&built.mesh.inside)
        .filter(|(_, inside)| **inside)
    {
        let [Some(&pa), Some(&pb), Some(&pc)] = [a, b, c].map(|v| corners.get(v as usize)) else {
            continue;
        };
        for [wa, wb] in [
            [0.1_f64, 0.1],
            [0.8, 0.1],
            [0.1, 0.8],
            [0.45, 0.1],
            [0.1, 0.45],
            [0.45, 0.45],
            [0.6, 0.2],
            [0.2, 0.6],
            [0.2, 0.2],
            [1.0 / 3.0, 1.0 / 3.0],
        ] {
            let wc = 1.0 - wa - wb;
            let [x, y, z] = mix([wa, wb, wc], [pa, pb, pc]);
            if let Some((height, _)) = faces.height(x, y) {
                gap = gap.max((height - z).abs());
            }
        }
        let paved = [pa, pb, pc]
            .iter()
            .all(|p| (-1e-9..=40.0 + 1e-9).contains(&p[0]) && p[1].abs() <= 2.0 + 1e-9);
        if !paved {
            let angle = |p: [f64; 3], q: [f64; 3], r: [f64; 3]| {
                let (u, v) = ([q[0] - p[0], q[1] - p[1]], [r[0] - p[0], r[1] - p[1]]);
                u[0].mul_add(v[1], -u[1] * v[0])
                    .abs()
                    .atan2(u[0].mul_add(v[0], u[1] * v[1]))
                    .to_degrees()
            };
            let smallest = angle(pa, pb, pc)
                .min(angle(pb, pc, pa))
                .min(angle(pc, pa, pb));
            angles.push(smallest);
        }
    }
    angles.sort_by(f64::total_cmp);
    Ok((gap, angles.get(angles.len() / 20).copied().unwrap_or(0.0)))
}

#[test]
fn error_sampling_reuses_move_samples_and_cached_triangle_errors() {
    let calls = std::cell::Cell::new(0_usize);
    let design = |x: f64, y: f64| {
        calls.set(calls.get() + 1);
        Some((0.25f64.mul_add(x, 0.5 * y) + 0.2, false))
    };
    let corners = [[0.0, 0.0, 0.0], [2.0, 0.0, 0.0], [0.0, 2.0, 0.0]];
    let mut memo = Memo::default();

    // Mesher::moves evaluates these four samples before asking for the triangle's error.
    for weights in SAMPLES {
        let [x, y, _] = mix(weights, corners);
        assert!(memo.height(&design, x, y).is_some());
    }
    assert_eq!(calls.get(), 4);

    let first = memo.error(&design, corners);
    assert!(first > 0.0);
    assert_eq!(calls.get(), 7, "only the three side midpoints are new");
    assert_eq!(memo.error(&design, corners), first);
    assert_eq!(calls.get(), 7, "a repeated error request evaluates nothing");
}

#[test]
fn an_exhausted_vertical_error_budget_refuses_instead_of_publishing_the_unrefined_mesh()
-> Result<(), OutlineError> {
    let (mut points, triangles) = base();
    for p in points.as_chunks_mut::<3>().0 {
        p[2] = 0.05f64.mul_add(p[0], 9.0);
    }
    let grading = Grading {
        cut: HALF,
        fill: HALF,
        limit: 10.0,
        rounding: 0.0,
        effect: Effect::Both,
        tolerance: 1e-6,
    };
    let operator = Operator::new(&points, &triangles, &PAVED, &PAVED_TRIANGLES, &[], grading)?;
    let unrefined = build_with(
        &OutlineRequest {
            points: &points,
            triangles: &triangles,
            region: &REGION,
            surface_points: &PAVED,
            surface_triangles: &PAVED_TRIANGLES,
            breaklines: &[],
            spots: &[],
            surface_grades: &[],
            refine: None,
            grading,
        },
        Some(crate::terrain::outline::toe::Toe::DEFAULT),
    )?;
    let ground = |x, y| operator.base.height_at(x, y).map(|sample| sample.height);
    let result = operator.meshed(
        &ground,
        &|_, _| true,
        (&unrefined.mesh, [0, unrefined.toe]),
        Refine {
            budget: 0,
            ..Refine::DEFAULT
        },
        [(&points, &triangles), (&PAVED, &PAVED_TRIANGLES)],
    );
    assert_eq!(result.err(), Some(OutlineError::Refinement));
    Ok(())
}

#[test]
fn boundary_step_strips_retain_local_mismatch_witnesses() -> Result<(), OutlineError> {
    let (points, triangles) = base();
    // One metre below the pavement is not enough for its fill face: refinement may classify the
    // resulting ring-adjacent residual as step work only with a local stitch mismatch.
    let region = [
        -10.0, -3.0, 0.0, 50.0, -3.0, 0.0, 50.0, 15.0, 0.0, -10.0, 15.0, 0.0,
    ];
    let request = OutlineRequest {
        points: &points,
        triangles: &triangles,
        region: &region,
        surface_points: &PAVED,
        surface_triangles: &PAVED_TRIANGLES,
        breaklines: &[],
        spots: &[],
        surface_grades: &[],
        refine: Some(Refine::DEFAULT),
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
    assert_eq!(build(&request)?.steps, built.steps, "deterministic");
    let strips: Vec<_> = built
        .steps
        .iter()
        .filter_map(|step| match *step {
            StepWitness::StepStrip {
                position,
                design,
                residual,
            } => Some((position, design, residual)),
            _ => None,
        })
        .collect();
    assert!(
        !strips.is_empty(),
        "the capture-boundary step was classified"
    );
    assert_eq!(strips.len(), STEP_WITNESS_LIMIT);
    assert!(
        strips.iter().all(|(position, _, _)| {
            position[1].to_bits() == (-3.0_f64).to_bits() && position[0] <= 43.4
        }),
        "the unfiltered cap is filled before the allowed right-end strips"
    );
    assert!(built.clipped > 0);
    for ([x, y, retained], design, residual) in strips {
        assert!(residual > Refine::DEFAULT.tolerance);
        assert!((design - retained).abs() > Refine::DEFAULT.tolerance);
        let on_ring = built.mesh.ring.iter().enumerate().any(|(index, &from)| {
            let Some(to) = built.mesh.ring.get((index + 1) % built.mesh.ring.len()) else {
                return false;
            };
            let vertices = plan::vertices(&built.mesh.points);
            let (Some(a), Some(b)) = (vertices.get(from as usize), vertices.get(*to as usize))
            else {
                return false;
            };
            let cross = (x - a[0]).mul_add(b[1] - a[1], -(y - a[1]) * (b[0] - a[0]));
            cross.abs() <= 1e-9 * (b[0] - a[0]).hypot(b[1] - a[1]).max(1.0)
        });
        assert!(on_ring, "witness at {x},{y} is not on the stitch");
    }

    // Filtering after retention would publish nothing here: the first 32 bottom-ring events are
    // all at x <= 43.4. The mesher instead applies this diagnostic-only predicate before its cap.
    let coarse = build(&OutlineRequest {
        refine: None,
        ..request
    })?;
    let operator = Operator::new(
        &points,
        &triangles,
        &PAVED,
        &PAVED_TRIANGLES,
        &[],
        request.grading,
    )?;
    let ground = |x, y| operator.base.height_at(x, y).map(|sample| sample.height);
    let (_, _, late) = operator.meshed(
        &ground,
        &|x, _| x > 43.4,
        (&coarse.mesh, [0, coarse.toe]),
        Refine::DEFAULT,
        [(&points, &triangles), (&PAVED, &PAVED_TRIANGLES)],
    )?;
    assert!(!late.is_empty(), "later allowed events survive the cap");
    assert!(late.iter().all(|step| {
        matches!(step, StepWitness::StepStrip { position, .. } if position[0] > 43.4)
    }));
    Ok(())
}

#[test]
fn refinement_bounds_the_faces_vertical_error_without_offset_rings() -> Result<(), OutlineError> {
    for rounding in [0.0, 1.0] {
        let (coarse, points) = refined(rounding, None)?;
        let started = std::time::Instant::now();
        let (built, _) = refined(rounding, Some(Refine::DEFAULT))?;
        let took = started.elapsed();
        let (before, _) = measure(&coarse, &points, rounding)?;
        let (gap, p5) = measure(&built, &points, rounding)?;
        // What the caller's offset rings gave, with the T2 lines: half-metre rings past the toe.
        let rings: Vec<f64> = (0..=12).map(|k| 0.5 * f64::from(k)).collect();
        let (ringed, _) = refined_toe(
            rounding,
            None,
            &rings,
            Some(crate::terrain::outline::toe::Toe::DEFAULT),
        )?;
        let (ring_gap, ring_p5) = measure(&ringed, &points, rounding)?;
        // The mesher alone, without the T2 lines: the toe only sampled.
        let (bare, _) = refined_toe(rounding, Some(Refine::DEFAULT), &[0.0], None)?;
        let (bare_gap, bare_p5) = measure(&bare, &points, rounding)?;
        let report = built.refined;
        eprintln!(
            "rounding {rounding}: unrefined {before:.3} m; rings + T2 {ring_gap:.4} m, p5 {ring_p5:.1} deg, \
             {} vertices; meshed + T2 {gap:.5} m, p5 {p5:.1} deg, {} vertices, {took:?}, {report:?}; \
             meshed alone {bare_gap:.4} m, p5 {bare_p5:.1} deg, {} vertices",
            ringed.points.len() / 3,
            built.points.len() / 3,
            bare.points.len() / 3
        );
        assert!(
            before > 0.1,
            "the unrefined mesh misses the faces: {before}"
        );
        assert!(!report.exhausted && report.residual <= 0.01, "{report:?}");
        // The T3 gate: within a centimetre of the faces at ten samples per triangle (the mesher
        // takes seven), the fifth-percentile angle at least 15 degrees, and no more vertices
        // than twice the half-metre rings that miss by several centimetres.
        assert!(gap <= 0.0101, "{gap}");
        assert!(p5 >= 15.0, "{p5}");
        assert!(ring_gap > 2.0 * gap && ring_p5 < p5, "{ring_gap} {ring_p5}");
        assert!(
            built.points.len() <= 2 * ringed.points.len(),
            "vertex budget"
        );
        // Without the toe line the mesher samples the toe's kink: a little over, same angles.
        assert!(bare_gap <= 0.015 && bare_p5 >= 15.0, "{bare_gap} {bare_p5}");
        // Locality: unmoved triangles are the ground, and a triangle the toe crosses between
        // its samples is no wider than the band (with the toe line, none is crossed).
        for mesh in [&built, &bare] {
            let (wide, ground) = toe_band(mesh, &points, rounding)?;
            assert_eq!(wide, 0, "triangles across the toe wider than the band");
            assert!(
                ground < 1e-12,
                "an unmoved triangle leaves the ground by {ground}"
            );
        }
    }
    Ok(())
}

/// Over the inside triangles: how many the toe crosses between their inside
/// samples (the exact faces keep the ground at some and move it at others) with a
/// side longer than the band, and how far a triangle whose vertices are all
/// unmoved strays from the ground at its samples.
fn toe_band(built: &Outlined, points: &[f64], rounding: f64) -> Result<(usize, f64), OutlineError> {
    let ground = Field::new(points, &base().1).map_err(|_| OutlineError::Field)?;
    let faces = exact(points, rounding)?;
    let corners = plan::vertices(&built.points);
    let (mut wide, mut stray) = (0, 0.0_f64);
    for (&[a, b, c], _) in built
        .mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&built.mesh.inside)
        .filter(|(_, inside)| **inside)
    {
        let [Some(&pa), Some(&pb), Some(&pc)] = [a, b, c].map(|v| corners.get(v as usize)) else {
            continue;
        };
        let moved = [a, b, c].map(|v| built.owners.get(v as usize).copied().unwrap_or(false));
        let side = |p: [f64; 3], q: [f64; 3]| (q[0] - p[0]).hypot(q[1] - p[1]);
        let inside: Vec<bool> = [
            [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
            [2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0],
            [1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0],
            [1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0],
        ]
        .iter()
        .filter_map(|&w| {
            let [x, y, _] = mix(w, [pa, pb, pc]);
            let (height, _) = faces.height(x, y)?;
            Some(height.to_bits() != ground.height_at(x, y)?.height.to_bits())
        })
        .collect();
        let longest = side(pa, pb).max(side(pb, pc)).max(side(pc, pa));
        if inside.contains(&true) && inside.contains(&false) {
            wide += usize::from(longest > Refine::DEFAULT.toe + 1e-9);
        }
        if !moved.iter().any(|m| *m) {
            for weights in [
                [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
                [0.6, 0.2, 0.2],
                [0.2, 0.6, 0.2],
            ] {
                let [x, y, z] = mix(weights, [pa, pb, pc]);
                if let Some(sample) = ground.height_at(x, y) {
                    stray = stray.max((sample.height - z).abs());
                }
            }
        }
    }
    Ok((wide, stray))
}

#[test]
fn the_mesher_keeps_the_ground_affine_the_area_and_every_line() -> Result<(), OutlineError> {
    // Kinked ground: every base edge is a real crease of the ground, so every one the region
    // keeps must stay a path of triangle sides for the base to be affine under each triangle.
    let (mut points, triangles) = base();
    for (k, p) in points.as_chunks_mut::<3>().0.iter_mut().enumerate() {
        let bump = if k % 3 == 0 {
            0.4
        } else if k % 3 == 1 {
            -0.3
        } else {
            0.0
        };
        p[2] = 0.05f64.mul_add(p[0], 9.0) + bump;
    }
    let built = build_with(
        &OutlineRequest {
            points: &points,
            triangles: &triangles,
            region: &REGION,
            surface_points: &PAVED,
            surface_triangles: &PAVED_TRIANGLES,
            breaklines: &[],
            spots: &[],
            surface_grades: &[],
            refine: Some(Refine::DEFAULT),
            grading: Grading {
                cut: HALF,
                fill: HALF,
                limit: 10.0,
                rounding: 1.0,
                effect: Effect::Both,
                tolerance: 1e-6,
            },
        },
        Some(crate::terrain::outline::toe::Toe::DEFAULT),
    )?;
    let mesh = &built.mesh;
    assert!(built.refined.added > 0, "{:?}", built.refined);
    assert!(
        mesh.new_vertices
            .contains(&crate::terrain::region::RegionVertex::Refined),
        "the mesher's vertices are recorded"
    );
    let ground = Field::new(&points, &triangles).map_err(|_| OutlineError::Field)?;
    let corners = plan::vertices(&mesh.points);
    let mut area = 0.0;
    let mut worst = 0.0_f64;
    for (&[a, b, c], _) in mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&mesh.inside)
        .filter(|(_, inside)| **inside)
    {
        let [Some(&pa), Some(&pb), Some(&pc)] = [a, b, c].map(|v| corners.get(v as usize)) else {
            return Err(OutlineError::Coverage);
        };
        let doubled = (pb[0] - pa[0]).mul_add(pc[1] - pa[1], -(pb[1] - pa[1]) * (pc[0] - pa[0]));
        assert!(doubled > 0.0, "a counter-clockwise triangle");
        area += doubled / 2.0;
        // The base heights at the corners span the ground across the whole triangle.
        for w in [
            [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
            [0.7, 0.2, 0.1],
            [0.1, 0.2, 0.7],
        ] {
            let [x, y, z] = mix(w, [pa, pb, pc]);
            if let Some(sample) = ground.height_at(x, y) {
                if (sample.height - z).abs() > 1e-6 {
                    eprintln!(
                        "BAD tri {a} {b} {c} {pa:?} {pb:?} {pc:?} faces {:?} {:?} {:?} new {} {:?}",
                        ground.height_at(pa[0], pa[1]).map(|s| s.face),
                        ground.height_at(pb[0], pb[1]).map(|s| s.face),
                        ground.height_at(pc[0], pc[1]).map(|s| s.face),
                        mesh.input_vertices as usize + mesh.split_vertices.len(),
                        [a, b, c].map(|v| (v as usize)
                            .checked_sub(mesh.input_vertices as usize + mesh.split_vertices.len())
                            .and_then(|i| mesh.new_vertices.get(i)))
                    );
                }
                worst = worst.max((sample.height - z).abs());
            }
        }
    }
    // The region's 60 x 30 m inside, whatever its triangles.
    assert!((area - 1800.0).abs() < 1e-6, "{area}");
    assert!(
        worst < 1e-9,
        "the ground leaves a triangle's plane by {worst}"
    );
    // Every line segment is a chain of edges from its start to its end.
    for (line, vertices) in mesh.breakline_vertices.iter().enumerate() {
        for (segment, pair) in vertices.windows(2).enumerate() {
            let &[start, end] = pair else {
                continue;
            };
            let mut at = start;
            for edge in mesh
                .line_edges
                .iter()
                .filter(|edge| edge.line as usize == line && edge.segment as usize == segment)
            {
                assert_eq!(
                    edge.vertices[0], at,
                    "line {line} segment {segment} is a path"
                );
                at = edge.vertices[1];
            }
            assert_eq!(at, end, "line {line} segment {segment} reaches its end");
        }
    }
    Ok(())
}
