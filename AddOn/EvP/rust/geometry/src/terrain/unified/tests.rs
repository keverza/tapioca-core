// Cohesion review: the shared grid/ring fixtures and the explicit mutation and
// refusal matrix stay together so every unified-build contract is tested against
// the same immutable baseline inputs; the >1000-line advisory is intentional.
use super::*;

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

fn surface(side: u32, spacing: f64, height: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column) * spacing, f64::from(row) * spacing);
            points.extend([x, y, height(x, y)]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..side.saturating_sub(1) {
        for column in 0..side.saturating_sub(1) {
            let a = row * side + column;
            triangles.extend([a, a + 1, a + side + 1, a, a + side + 1, a + side]);
        }
    }
    (points, triangles)
}

fn ring(corners: &[[f64; 2]]) -> Vec<f64> {
    corners.iter().flat_map(|&[x, y]| [x, y, 0.0]).collect()
}

fn vertex_at(points: &[f64], x: f64, y: f64) -> Option<u32> {
    plan::vertices(points)
        .iter()
        .position(|&[px, py, _]| px.to_bits() == x.to_bits() && py.to_bits() == y.to_bits())
        .and_then(|index| u32::try_from(index).ok())
}

fn height_at(points: &[f64], x: f64, y: f64) -> Option<f64> {
    vertex_at(points, x, y)
        .and_then(|vertex| points.get(vertex as usize * 3 + 2))
        .copied()
}

fn signed_area(points: &[f64], face: &[u32]) -> f64 {
    let vertices = plan::vertices(points);
    let at = |vertex: u32| vertices.get(vertex as usize).copied().unwrap_or([0.0; 3]);
    let mut corners = [[0.0; 3]; 3];
    for (slot, &vertex) in corners.iter_mut().zip(face.iter().take(3)) {
        *slot = at(vertex);
    }
    let [a, b, c] = corners;
    0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
}

fn assert_mesh_invariants(built: &BuiltTerrain) {
    for face in built.mesh.triangles.as_chunks::<3>().0 {
        assert!(signed_area(&built.points, face) > 0.0, "positive {face:?}");
    }
    assert_eq!(built.mesh.parents.len(), built.mesh.inside.len());
    assert_eq!(built.mesh.inside.len(), built.mesh.triangles.len() / 3);
    assert_eq!(built.base.len(), built.points.len() / 3);
    assert_eq!(built.owners.len(), built.base.len());
}

fn constant_entry<'a>(
    order: u32,
    parts: &'a [Part<'a>],
    level: f64,
    effect: TerrainEffect,
    transition: Transition,
) -> Entry<'a> {
    Entry {
        order,
        object: ObjectId(100 + u64::from(order)),
        root_operation: OperationId(200 + u64::from(order)),
        sources: &[],
        parts,
        surface: Surface::Constant { level },
        terrain: Terrain { effect, transition },
    }
}

fn sharp() -> Transition {
    Transition::Daylight {
        cut: Slope::RiseRun {
            rise: 1.0,
            run: 1.0,
        },
        fill: Slope::RiseRun {
            rise: 1.0,
            run: 1.0,
        },
        reach: 0.0,
    }
}

#[test]
fn empty_plan_without_property_is_baseline_bit_for_bit() -> Result<(), BuildError> {
    let (points, triangles) = surface(5, 1.0, |_, _| 0.0);
    let entries: [Entry<'_>; 0] = [];
    let plan = Plan {
        revision: 4,
        hash: 44,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: None,
        },
        &plan,
    )?;
    assert_eq!(built.points, points);
    assert_eq!(built.mesh.triangles, triangles);
    assert!(built.owners.iter().all(Option::is_none));
    assert!(close(built.volumes.site.fill, 0.0, 1e-12));
    assert!(close(built.volumes.site.cut, 0.0, 1e-12));
    assert!(close(built.volumes.site.area, 16.0, 1e-12));
    assert_eq!(built.volumes.site.triangles, 32);
    assert_eq!((built.revision, built.plan_hash), (4, 44));
    Ok(())
}

#[test]
fn grade_hole_and_disjoint_parts_keep_membership_and_volumes() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let hole = ring(&[[4.0, 4.0], [5.0, 4.0], [5.0, 5.0], [4.0, 5.0]]);
    let island = ring(&[[6.0, 5.5], [6.5, 5.5], [6.5, 6.0], [6.0, 6.0]]);
    let holes: [&[f64]; 1] = [&hole];
    let parts = [
        Part {
            outer: &outer,
            holes: &holes,
        },
        Part {
            outer: &island,
            holes: &[],
        },
    ];
    let entry = constant_entry(0, &parts, 3.0, TerrainEffect::Grade, sharp());
    let entries = [entry];
    let plan = Plan {
        revision: 1,
        hash: 11,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let request = Baseline {
        points: &points,
        triangles: &triangles,
        property: Some(&property),
    };
    let built = build_terrain(&request, &plan)?;
    assert_eq!(build_terrain(&request, &plan)?, built, "deterministic");
    assert_mesh_invariants(&built);
    assert_eq!(built.lines.len(), 12);
    assert!(
        built
            .mesh
            .line_edges
            .iter()
            .all(|edge| (edge.line as usize) < built.lines.len())
    );

    for &[x, y, z] in plan::vertices(&built.points) {
        let in_outer = (2.0..=7.0).contains(&x) && (2.0..=7.0).contains(&y);
        let in_hole = (4.0..=5.0).contains(&x) && (4.0..=5.0).contains(&y);
        let in_island = (6.0..=6.5).contains(&x) && (5.5..=6.0).contains(&y);
        if (in_outer && !in_hole) || in_island {
            assert!(close(z, 3.0, 1e-12), "graded at {x},{y}: {z}");
        }
        if x > 4.0 && x < 5.0 && y > 4.0 && y < 5.0 {
            assert!(close(z, 0.0, 1e-12), "hole at {x},{y}: {z}");
        }
        if (4.0..=5.0).contains(&x) && (4.0..=5.0).contains(&y) {
            assert!(built.lines.iter().any(|origin| origin.ring == 1));
        }
    }
    // Domain area is 25 - 1 + 0.25; sharp daylight reach contributes no exterior fill.
    // Hard 2.5D boundaries share vertices with the neighbouring surface, so the
    // exact triangular integral includes the boundary interpolation rather than
    // pretending a vertical wall exists. The one-domain volume below pins the
    // exact positive fill; hard-step duplication/refusal is a later P3 slice.
    assert!(built.volumes.site.fill > 0.0);
    assert!(close(built.volumes.site.cut, 0.0, 1e-12));
    assert!(close(built.volumes.site.area, 49.0, 1e-12));
    Ok(())
}

#[test]
fn one_inner_domain_integrates_positive_fill_and_property_exterior_stays_baseline()
-> Result<(), BuildError> {
    let (points, triangles) = surface(11, 1.0, |_, _| 0.0);
    let property = ring(&[[0.0, 0.0], [10.0, 0.0], [10.0, 10.0], [0.0, 10.0]]);
    let domain = ring(&[[1.0, 1.0], [8.0, 1.0], [1.0, 8.0]]);
    let parts = [Part {
        outer: &domain,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        3.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let plan = Plan {
        revision: 12,
        hash: 121,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    // Triangles strictly inside the constrained domain are all at target height;
    // shared hard-boundary vertices also interpolate into the neighbouring site
    // surface, so the exact fill is at least the closed domain integral.
    assert!(built.volumes.site.fill >= 0.5 * 7.0 * 7.0 * 3.0);
    assert!(close(built.volumes.site.cut, 0.0, 1e-12));
    assert!(close(built.volumes.context.fill, 0.0, 1e-12));
    Ok(())
}

#[test]
fn later_overlapping_entries_override_earlier_ones() -> Result<(), BuildError> {
    let (points, triangles) = surface(10, 1.0, |_, _| 0.0);
    let property = ring(&[[0.5, 0.5], [8.5, 0.5], [8.5, 8.5], [0.5, 8.5]]);
    let first = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let second = ring(&[[5.0, 5.0], [8.0, 5.0], [8.0, 8.0], [5.0, 8.0]]);
    let first_parts = [Part {
        outer: &first,
        holes: &[],
    }];
    let second_parts = [Part {
        outer: &second,
        holes: &[],
    }];

    let build_with = |first_first: bool| {
        let (entries, owner) = if first_first {
            (
                [
                    constant_entry(0, &first_parts, 3.0, TerrainEffect::Grade, sharp()),
                    constant_entry(1, &second_parts, 1.0, TerrainEffect::Grade, sharp()),
                ],
                1,
            )
        } else {
            (
                [
                    constant_entry(0, &second_parts, 1.0, TerrainEffect::Grade, sharp()),
                    constant_entry(1, &first_parts, 3.0, TerrainEffect::Grade, sharp()),
                ],
                1,
            )
        };
        let plan = Plan {
            revision: 2,
            hash: 22,
            entries: &entries,
            paths: &[],
            masks: &[],
            controls: &[],
            control_tolerance: 1e-3,
        };
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        )
        .map(|built| (built, owner))
    };

    let (normal, overlap_owner) = build_with(true)?;
    let (reversed, reversed_owner) = build_with(false)?;
    let overlap = vertex_at(&normal.points, 6.0, 6.0).unwrap_or(u32::MAX);
    let outside = vertex_at(&normal.points, 3.0, 3.0).unwrap_or(u32::MAX);
    assert!(close(
        normal
            .points
            .get(overlap as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        1.0,
        1e-12
    ));
    assert!(close(
        reversed
            .points
            .get(overlap as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        3.0,
        1e-12
    ));
    assert!(close(
        normal
            .points
            .get(outside as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        3.0,
        1e-12
    ));
    assert_eq!(
        normal.owners.get(overlap as usize),
        Some(&Some(overlap_owner))
    );
    assert_eq!(
        reversed.owners.get(overlap as usize),
        Some(&Some(reversed_owner))
    );
    Ok(())
}

#[test]
fn adjacent_sharp_domains_with_different_targets_refuse_as_a_hard_step() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[0.0, 0.0], [8.0, 0.0], [8.0, 8.0], [0.0, 8.0]]);
    let left = ring(&[[2.0, 2.0], [4.0, 2.0], [4.0, 6.0], [2.0, 6.0]]);
    let right = ring(&[[4.0, 2.0], [6.0, 2.0], [6.0, 6.0], [4.0, 6.0]]);
    let left_parts = [Part {
        outer: &left,
        holes: &[],
    }];
    let right_parts = [Part {
        outer: &right,
        holes: &[],
    }];
    let entries = [
        constant_entry(0, &left_parts, 1.0, TerrainEffect::Grade, sharp()),
        constant_entry(1, &right_parts, 3.0, TerrainEffect::Grade, sharp()),
    ];
    let plan = Plan {
        revision: 25,
        hash: 251,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::HardStep { .. })
    ));
}

#[test]
fn add_raises_only_and_subtract_lowers_only_on_a_sloped_base() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |x, _| x);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let build_effect = |effect| {
        let entries = [constant_entry(0, &parts, 4.0, effect, sharp())];
        let plan = Plan {
            revision: 3,
            hash: 33,
            entries: &entries,
            paths: &[],
            masks: &[],
            controls: &[],
            control_tolerance: 1e-3,
        };
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        )
    };
    let raised = build_effect(TerrainEffect::Add)?;
    let lowered = build_effect(TerrainEffect::Subtract)?;
    for &[x, y, base] in plan::vertices(&points) {
        let vertex = vertex_at(&raised.points, x, y).unwrap_or(u32::MAX);
        let raised_z = raised
            .points
            .get(vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN);
        let lowered_z = lowered
            .points
            .get(vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN);
        assert!(close(base, x, 1e-12));
        let inside = (2.0..=7.0).contains(&x) && (2.0..=7.0).contains(&y);
        if inside {
            assert!(close(raised_z, base.max(4.0), 1e-12), "Add at {x},{y}");
            assert!(
                close(lowered_z, base.min(4.0), 1e-12),
                "Subtract at {x},{y}"
            );
        } else {
            assert!(close(raised_z, base, 1e-12));
            assert!(close(lowered_z, base, 1e-12));
        }
    }
    Ok(())
}

#[test]
fn falloff_blends_by_plan_distance_and_stops_at_blur() -> Result<(), BuildError> {
    let (points, triangles) = surface(17, 0.5, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.5, 2.5], [6.5, 2.5], [6.5, 6.5], [2.5, 6.5]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        2.0,
        TerrainEffect::Grade,
        Transition::Falloff { blur: 1.0 },
    )];
    let plan = Plan {
        revision: 5,
        hash: 55,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    let half = height_at(&built.points, 2.0, 3.0).unwrap_or(f64::NAN);
    let at_blur = height_at(&built.points, 1.5, 3.0).unwrap_or(f64::NAN);
    let inside = height_at(&built.points, 3.0, 3.0).unwrap_or(f64::NAN);
    let expected = 2.0 * (1.0 - smoother_step(0.5));
    assert!(close(inside, 2.0, 1e-12));
    assert!(close(half, expected, 1e-12));
    assert!(close(at_blur, 0.0, 1e-12));
    Ok(())
}

#[test]
fn daylight_runs_cut_and_fill_faces_to_the_reach() -> Result<(), BuildError> {
    let (points, triangles) = surface(17, 0.5, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[3.0, 3.0], [6.0, 3.0], [6.0, 6.0], [3.0, 6.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        2.0,
        TerrainEffect::Grade,
        Transition::Daylight {
            cut: Slope::RiseRun {
                rise: 1.0,
                run: 1.0,
            },
            fill: Slope::RiseRun {
                rise: 1.0,
                run: 1.0,
            },
            reach: 2.0,
        },
    )];
    let plan = Plan {
        revision: 6,
        hash: 66,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    assert!(close(
        height_at(&built.points, 2.0, 3.0).unwrap_or(f64::NAN),
        1.0,
        1e-12
    ));
    assert!(close(
        height_at(&built.points, 1.5, 3.0).unwrap_or(f64::NAN),
        0.5,
        1e-12
    ));
    assert!(close(
        height_at(&built.points, 1.0, 3.0).unwrap_or(f64::NAN),
        0.0,
        1e-12
    ));
    assert_mesh_invariants(&built);
    Ok(())
}

fn point_control(id: u64, x: f64, y: f64, z: f64) -> Control<'static> {
    Control {
        source: SourceId(id),
        vertex: id,
        point: true,
        point_position: [x, y, z],
        line: &[],
    }
}

fn line_control(id: u64, line: &'static [f64]) -> Control<'static> {
    Control {
        source: SourceId(id),
        vertex: 0,
        point: false,
        point_position: [0.0; 3],
        line,
    }
}

#[test]
fn fixed_point_matching_the_grade_is_inserted_and_kept_exact() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        3.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let controls = [point_control(900, 4.0, 4.0, 3.0)];
    let plan = Plan {
        revision: 13,
        hash: 131,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &controls,
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    let vertex = vertex_at(&built.points, 4.0, 4.0).unwrap_or(u32::MAX);
    assert!(close(
        built
            .points
            .get(vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        3.0,
        1e-12
    ));
    assert_eq!(built.owners.get(vertex as usize), Some(&None));
    let locked = built
        .locked
        .first()
        .ok_or(BuildError::Region(RegionError::Topology))?;
    assert_eq!(locked.control, 0);
    Ok(())
}

#[test]
fn fixed_point_conflicting_with_the_grade_refuses_atomically() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        3.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let controls = [point_control(901, 4.0, 4.0, 1.0)];
    let plan = Plan {
        revision: 14,
        hash: 141,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &controls,
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::LockedConflict { .. })
    ));
}

#[test]
fn fixed_line_interpolates_every_split_vertex_and_owns_no_region_entry() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let line: &[f64] = &[
        2.0, 3.0, 2.0, //
        3.0, 3.0, 2.0, //
        4.0, 3.0, 2.0, //
        5.0, 3.0, 2.0, //
        6.0, 3.0, 2.0, //
        7.0, 3.0, 2.0,
    ];
    let entries = [constant_entry(
        0,
        &parts,
        2.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let controls = [line_control(902, line)];
    let plan = Plan {
        revision: 15,
        hash: 151,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &controls,
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    assert_eq!(
        built.control_lines.first(),
        Some(&ControlLineOrigin {
            control: 0,
            segments: 5,
        })
    );
    assert_eq!(built.locked.len(), 6);
    for locked in &built.locked {
        assert_eq!(locked.control, 0);
        assert!(close(locked.height, 2.0, 1e-12));
        assert_eq!(built.owners.get(locked.vertex as usize), Some(&None));
        let z = built
            .points
            .get(locked.vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN);
        assert!(close(z, 2.0, 1e-12));
    }
    Ok(())
}

#[test]
fn coincident_fixed_points_with_different_heights_refuse() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let entries: [Entry<'_>; 0] = [];
    let controls = [
        point_control(903, 4.0, 4.0, 1.0),
        point_control(904, 4.0, 4.0, 2.0),
    ];
    let plan = Plan {
        revision: 16,
        hash: 161,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &controls,
        control_tolerance: 0.0,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::LockedConflict { .. })
    ));
}

#[test]
fn zero_length_fixed_line_refuses_before_meshing() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let entries: [Entry<'_>; 0] = [];
    let line: &[f64] = &[3.0, 3.0, 1.0, 3.0, 3.0, 1.0];
    let controls = [line_control(905, line)];
    let plan = Plan {
        revision: 17,
        hash: 171,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &controls,
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::ControlZeroLength { .. })
    ));
}

fn mask<'a>(id: u64, kind: MaskKind, parts: &'a [Part<'a>]) -> Mask<'a> {
    Mask {
        operation: OperationId(id),
        kind,
        sources: &[],
        parts,
    }
}

fn design_station(x: f64, y: f64, z: f64, half: f64) -> design::Station {
    design::Station {
        position: [x, y, z],
        half_left: half,
        half_right: half,
        crossfall: None,
        shoulder_left: 0.0,
        shoulder_right: 0.0,
        cut: Slope::Percent(100.0),
        fill: Slope::Percent(100.0),
        cut_limit: 10.0,
        fill_limit: 10.0,
        rounding: 0.0,
    }
}

fn path(order: u32, stations: &[design::Station]) -> Path<'_> {
    Path {
        order,
        source: SourceId(920 + u64::from(order)),
        operation: OperationId(930 + u64::from(order)),
        object: ObjectId(940 + u64::from(order)),
        band: design::Band {
            stations,
            effect: Effect::Both,
        },
        tolerance: 1e-3,
    }
}

#[test]
fn path_band_follows_regions_on_the_same_constrained_mesh() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[0.0, 0.0], [8.0, 0.0], [8.0, 8.0], [0.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let stations = [
        design_station(2.0, 4.0, 2.0, 1.0),
        design_station(7.0, 4.0, 2.0, 1.0),
    ];
    let entries = [constant_entry(
        0,
        &parts,
        1.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let paths = [path(1, &stations)];
    let plan = Plan {
        revision: 21,
        hash: 211,
        entries: &entries,
        paths: &paths,
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    assert!(built.path_lines.len() >= 6);
    assert!(built.path_lines.iter().any(|origin| origin.kind
        == crate::terrain::hybrid::LineKind::Edge(crate::terrain::edit::Hand::Left)));
    assert!(built.path_lines.iter().any(|origin| origin.kind
        == crate::terrain::hybrid::LineKind::Edge(crate::terrain::edit::Hand::Right)));
    assert!(
        built
            .path_lines
            .iter()
            .any(|origin| origin.kind == crate::terrain::hybrid::LineKind::End(false))
    );
    assert!(
        built
            .path_lines
            .iter()
            .any(|origin| origin.kind == crate::terrain::hybrid::LineKind::End(true))
    );
    assert!(built.path_lines.iter().any(|origin| matches!(
        origin.kind,
        crate::terrain::hybrid::LineKind::Daylight { .. }
    )));

    let band_vertex = vertex_at(&built.points, 4.0, 4.0).unwrap_or(u32::MAX);
    let region_vertex = vertex_at(&built.points, 3.0, 2.0).unwrap_or(u32::MAX);
    assert!(close(
        built
            .points
            .get(band_vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        2.0,
        1e-12
    ));
    assert!(close(
        built
            .points
            .get(region_vertex as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        1.0,
        1e-12
    ));
    assert_eq!(built.owners.get(band_vertex as usize), Some(&Some(1)));
    assert_eq!(built.owners.get(region_vertex as usize), Some(&Some(0)));
    Ok(())
}

#[test]
fn path_across_a_region_hole_builds_one_coherent_mesh() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[0.0, 0.0], [8.0, 0.0], [8.0, 8.0], [0.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [6.0, 2.0], [6.0, 6.0], [2.0, 6.0]]);
    let hole = ring(&[[4.0, 4.0], [5.0, 4.0], [5.0, 5.0], [4.0, 5.0]]);
    let holes: [&[f64]; 1] = [&hole];
    let parts = [Part {
        outer: &outer,
        holes: &holes,
    }];
    let stations = [
        design_station(2.0, 4.0, 2.0, 1.0),
        design_station(6.0, 4.0, 2.0, 1.0),
    ];
    let entries = [constant_entry(
        0,
        &parts,
        1.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let paths = [path(1, &stations)];
    let plan = Plan {
        revision: 24,
        hash: 241,
        entries: &entries,
        paths: &paths,
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    assert_mesh_invariants(&built);
    assert!(built.path_lines.iter().any(|origin| matches!(
        origin.kind,
        crate::terrain::hybrid::LineKind::Daylight { .. }
    )));
    let crossing = vertex_at(&built.points, 4.0, 4.0).unwrap_or(u32::MAX);
    assert!(close(
        built
            .points
            .get(crossing as usize * 3 + 2)
            .copied()
            .unwrap_or(f64::NAN),
        2.0,
        1e-12
    ));
    assert_eq!(built.owners.get(crossing as usize), Some(&Some(1)));
    Ok(())
}

#[test]
fn fixed_control_conflicting_with_a_later_path_refuses() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[0.0, 0.0], [8.0, 0.0], [8.0, 8.0], [0.0, 8.0]]);
    let stations = [
        design_station(2.0, 4.0, 2.0, 1.0),
        design_station(7.0, 4.0, 2.0, 1.0),
    ];
    let entries: [Entry<'_>; 0] = [];
    let paths = [path(0, &stations)];
    let controls = [point_control(921, 4.0, 4.0, 1.0)];
    let plan = Plan {
        revision: 22,
        hash: 221,
        entries: &entries,
        paths: &paths,
        masks: &[],
        controls: &controls,
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::LockedConflict { .. })
    ));
}

#[test]
fn coincident_path_stations_refuse_before_meshing() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let stations = [
        design_station(4.0, 4.0, 2.0, 1.0),
        design_station(4.0, 4.0, 2.0, 1.0),
    ];
    let entries: [Entry<'_>; 0] = [];
    let paths = [path(0, &stations)];
    let plan = Plan {
        revision: 23,
        hash: 231,
        entries: &entries,
        paths: &paths,
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::PathShape { .. })
    ));
}

#[test]
fn protected_zone_keeps_baseline_inside_and_on_its_ring() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let protected = ring(&[[4.0, 4.0], [5.0, 4.0], [5.0, 5.0], [4.0, 5.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let mask_parts = [Part {
        outer: &protected,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        3.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let masks = [mask(910, MaskKind::ProtectedZone, &mask_parts)];
    let plan = Plan {
        revision: 18,
        hash: 181,
        entries: &entries,
        paths: &[],
        masks: &masks,
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    assert_eq!(built.mask_lines.len(), 4);
    assert!(
        built
            .mask_lines
            .iter()
            .all(|origin| origin.mask == 0 && origin.ring == 0)
    );
    for &[x, y, z] in plan::vertices(&built.points) {
        let protected_member = (4.0..=5.0).contains(&x) && (4.0..=5.0).contains(&y);
        if protected_member {
            assert!(close(z, 0.0, 1e-12), "protected at {x},{y}: {z}");
        } else if (2.0..=7.0).contains(&x) && (2.0..=7.0).contains(&y) {
            assert!(close(z, 3.0, 1e-12), "graded at {x},{y}: {z}");
        }
    }
    let corner = vertex_at(&built.points, 4.0, 4.0).unwrap_or(u32::MAX);
    assert_eq!(built.owners.get(corner as usize), Some(&None));
    Ok(())
}

#[test]
fn grading_limit_restricts_the_entry_to_its_mask() -> Result<(), BuildError> {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let limited = ring(&[[4.0, 4.0], [5.0, 4.0], [5.0, 5.0], [4.0, 5.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let mask_parts = [Part {
        outer: &limited,
        holes: &[],
    }];
    let entries = [constant_entry(
        0,
        &parts,
        3.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let masks = [mask(911, MaskKind::GradingLimit, &mask_parts)];
    let plan = Plan {
        revision: 19,
        hash: 191,
        entries: &entries,
        paths: &[],
        masks: &masks,
        controls: &[],
        control_tolerance: 1e-3,
    };
    let built = build_terrain(
        &Baseline {
            points: &points,
            triangles: &triangles,
            property: Some(&property),
        },
        &plan,
    )?;
    for &[x, y, z] in plan::vertices(&built.points) {
        let in_region = (2.0..=7.0).contains(&x) && (2.0..=7.0).contains(&y);
        let in_limit = (4.0..=5.0).contains(&x) && (4.0..=5.0).contains(&y);
        if in_region && !in_limit {
            assert!(close(z, 0.0, 1e-12), "outside limit at {x},{y}: {z}");
        }
        if in_limit {
            assert!(close(z, 3.0, 1e-12), "inside limit at {x},{y}: {z}");
        }
    }
    Ok(())
}

#[test]
fn zero_length_mask_ring_refuses_before_meshing() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let zero = [3.0, 3.0, 0.0, 3.0, 3.0, 0.0, 4.0, 3.0, 0.0];
    let mask_parts = [Part {
        outer: &zero,
        holes: &[],
    }];
    let entries: [Entry<'_>; 0] = [];
    let masks = [mask(912, MaskKind::ProtectedZone, &mask_parts)];
    let plan = Plan {
        revision: 20,
        hash: 201,
        entries: &entries,
        paths: &[],
        masks: &masks,
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &plan,
        ),
        Err(BuildError::MaskZeroLength { .. })
    ));
}

#[test]
fn refusals_are_atomic_and_inputs_unchanged() {
    let (points, triangles) = surface(9, 1.0, |_, _| 0.0);
    let property = ring(&[[1.0, 1.0], [8.0, 1.0], [8.0, 8.0], [1.0, 8.0]]);
    let points_before = points.clone();
    let triangles_before = triangles.clone();
    let short = [2.0, 2.0, 0.0, 7.0, 2.0, 0.0];
    let short_parts = [Part {
        outer: &short,
        holes: &[],
    }];
    let short_entries = [constant_entry(
        0,
        &short_parts,
        1.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let short_plan = Plan {
        revision: 7,
        hash: 77,
        entries: &short_entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &short_plan,
        ),
        Err(BuildError::PlanShape { .. })
    ));

    let outer = ring(&[[2.0, 2.0], [7.0, 2.0], [7.0, 7.0], [2.0, 7.0]]);
    let parts = [Part {
        outer: &outer,
        holes: &[],
    }];
    let wrong_entries = [Entry {
        order: 1,
        ..constant_entry(0, &parts, 1.0, TerrainEffect::Grade, sharp())
    }];
    let wrong_plan = Plan {
        revision: 8,
        hash: 88,
        entries: &wrong_entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &wrong_plan,
        ),
        Err(BuildError::Order {
            expected: 0,
            current: 1
        })
    ));

    let entries = [constant_entry(
        0,
        &parts,
        1.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let no_property_plan = Plan {
        revision: 9,
        hash: 99,
        entries: &entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: None,
            },
            &no_property_plan,
        ),
        Err(BuildError::PropertyRequired)
    ));

    let outside = ring(&[[2.0, 2.0], [9.0, 2.0], [9.0, 7.0], [2.0, 7.0]]);
    let outside_parts = [Part {
        outer: &outside,
        holes: &[],
    }];
    let outside_entries = [constant_entry(
        0,
        &outside_parts,
        1.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let outside_plan = Plan {
        revision: 10,
        hash: 110,
        entries: &outside_entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &outside_plan,
        ),
        Err(BuildError::Region(RegionError::Breakline))
    ));

    let zero = [2.0, 2.0, 0.0, 2.0, 2.0, 0.0, 3.0, 2.0, 0.0];
    let zero_parts = [Part {
        outer: &zero,
        holes: &[],
    }];
    let zero_entries = [constant_entry(
        0,
        &zero_parts,
        1.0,
        TerrainEffect::Grade,
        sharp(),
    )];
    let zero_plan = Plan {
        revision: 11,
        hash: 111,
        entries: &zero_entries,
        paths: &[],
        masks: &[],
        controls: &[],
        control_tolerance: 1e-3,
    };
    assert!(matches!(
        build_terrain(
            &Baseline {
                points: &points,
                triangles: &triangles,
                property: Some(&property),
            },
            &zero_plan,
        ),
        Err(BuildError::ZeroLength { .. })
    ));

    assert_eq!(points, points_before);
    assert_eq!(triangles, triangles_before);
}
