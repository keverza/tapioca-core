use super::*;
use crate::terrain::design::Band;
use crate::terrain::edit::Effect;
use crate::terrain::hybrid::tests::{REGION, base, between, station, worked};
use crate::terrain::hybrid::{BuildRequest, Built, LineKind, build, build_locked};
use crate::terrain::region::RegionError;

fn request<'a>(
    points: &'a [f64],
    triangles: &'a [u32],
    stations: &'a [crate::terrain::design::Station],
) -> BuildRequest<'a> {
    BuildRequest {
        points,
        triangles,
        region: &REGION,
        band: Band {
            stations,
            effect: Effect::Both,
        },
        tolerance: 1e-6,
    }
}

fn at(built: &Built, vertex: u32) -> Option<[f64; 3]> {
    plan::vertices(&built.points).get(vertex as usize).copied()
}

#[test]
fn no_controls_is_the_original_build_bit_for_bit() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let input = request(&points, &triangles, &stations);
    assert_eq!(build(&input)?, build_locked(&input, Locked::default())?);
    Ok(())
}

#[test]
fn fixed_points_are_inserted_or_reused_at_their_exact_height() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    // First lies on the band, second is an original vertex away from influence.
    // Last duplicates the first: keep both supports and a single output vertex.
    let spots = [
        10.0,
        1.0,
        10.5,
        -0.5,
        -10.5,
        worked(-0.5, -10.5),
        10.0,
        1.0,
        10.5,
    ];
    let input = request(&points, &triangles, &stations);
    let locked = Locked {
        points: &spots,
        lines: &[],
    };
    let built = build_locked(&input, locked)?;
    assert_eq!(build_locked(&input, locked)?, built, "deterministic");
    assert_eq!(built.locked.len(), 2);
    for (&vertex, spot) in built.mesh.point_vertices.iter().zip(plan::vertices(&spots)) {
        assert_eq!(at(&built, vertex), Some(*spot));
        assert_eq!(built.owners.get(vertex as usize), Some(&None));
        assert!(built.mesh.triangles.contains(&vertex));
    }
    assert!(
        built
            .locked
            .iter()
            .any(|v| v.supports == [LockRef::Point(0), LockRef::Point(2)])
    );
    let [fill, cut] = between(&built, 0.0, 40.0);
    assert!((fill - 80.0 / 3.0).abs() < 1e-9 && (cut - 80.0 / 3.0).abs() < 1e-9);
    assert_eq!(built.volumes.context.fill.to_bits(), 0.0_f64.to_bits());
    assert_eq!(built.volumes.context.cut.to_bits(), 0.0_f64.to_bits());
    for (face, &inside) in built
        .mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&built.mesh.inside)
    {
        if !inside {
            for &vertex in face {
                let proposed = at(&built, vertex).map(|p| p[2].to_bits());
                let base = plan::vertices(&built.mesh.points)
                    .get(vertex as usize)
                    .map(|p| p[2].to_bits());
                assert_eq!(proposed, base);
            }
        }
    }
    Ok(())
}

#[test]
fn a_grading_mismatch_is_located_and_never_clamped() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let original = points.clone();
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let input = request(&points, &triangles, &stations);
    for y in [-1.0, 1.0, 2.5] {
        let spot = [10.0, y, worked(10.0, y)];
        let error = build_locked(
            &input,
            Locked {
                points: &spot,
                lines: &[],
            },
        )
        .err();
        let Some(HybridError::LockedConflict(conflict)) = error else {
            return Err(error.unwrap_or(HybridError::LockedParameters));
        };
        assert_eq!(conflict.control, LockRef::Point(0));
        assert_eq!(conflict.other, None);
        assert_eq!(conflict.position, spot);
        assert!(conflict.magnitude.abs() > input.tolerance);
        assert_eq!(conflict.name(), "LockedConflict");
        assert!(conflict.suggestion().contains("band"));
        assert!(
            HybridError::LockedConflict(conflict)
                .message()
                .starts_with("LockedConflict:")
        );
    }
    assert_eq!(points, original);
    Ok(())
}

#[test]
fn sloping_locked_lines_keep_all_split_vertices_and_every_contributor() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let a = [5.0, -1.0, 10.25, 35.0, 1.0, 11.75];
    let b = [5.0, 1.0, 10.25, 35.0, -1.0, 11.75];
    let spot = [20.0, 0.0, 11.0];
    let input = request(&points, &triangles, &stations);
    let built = build_locked(
        &input,
        Locked {
            lines: &[&a, &b, &a],
            points: &spot,
        },
    )?;
    assert_eq!(
        built.lines.get(..3),
        Some(
            &[
                LineKind::Locked(0),
                LineKind::Locked(1),
                LineKind::Locked(2)
            ][..]
        )
    );
    assert!(built.locked.len() > 5, "base edges split the lines");
    for lock in &built.locked {
        let Some([x, _, z]) = at(&built, lock.vertex) else {
            return Err(HybridError::LockedParameters);
        };
        assert_eq!(z.to_bits(), lock.height.to_bits());
        assert!((z - (10.0 + 0.05 * x)).abs() < 1e-12);
        assert_eq!(built.owners.get(lock.vertex as usize), Some(&None));
    }
    let centre = built
        .locked
        .iter()
        .find(|v| v.supports.contains(&LockRef::Point(0)));
    assert_eq!(centre.map(|v| v.supports.len()), Some(4));
    let [fill, cut] = between(&built, 0.0, 40.0);
    assert!((fill - 80.0 / 3.0).abs() < 1e-9 && (cut - 80.0 / 3.0).abs() < 1e-9);
    Ok(())
}

#[test]
fn a_line_crossing_the_band_cannot_hide_a_conflict_between_its_endpoints() -> Result<(), HybridError>
{
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    // Endpoints match the untouched base; the band crosses the line's interior.
    let line = [20.0, -8.0, worked(20.0, -8.0), 20.0, 8.0, worked(20.0, 8.0)];
    let result = build_locked(
        &request(&points, &triangles, &stations),
        Locked {
            points: &[],
            lines: &[&line],
        },
    );
    let Some(HybridError::LockedConflict(conflict)) = result.err() else {
        return Err(HybridError::LockedParameters);
    };
    assert_eq!(
        conflict.control,
        LockRef::Segment {
            line: 0,
            segment: 0
        }
    );
    assert!(conflict.position[1].abs() < 8.0);
    assert!(conflict.magnitude.abs() > 1e-6);
    Ok(())
}

#[test]
fn crossing_and_coincident_controls_must_agree_before_grading_is_checked() -> Result<(), HybridError>
{
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let a = [5.0, -1.0, 10.25, 35.0, 1.0, 11.75];
    let b = [5.0, 1.0, 11.25, 35.0, -1.0, 12.75];
    let input = request(&points, &triangles, &stations);
    for locked in [
        Locked {
            points: &[],
            lines: &[&a, &b],
        },
        Locked {
            points: &[20.0, 0.0, 12.0],
            lines: &[&a],
        },
        Locked {
            points: &[20.0, 0.0, 11.0, 20.0, 0.0, 12.0],
            lines: &[],
        },
    ] {
        let error = build_locked(&input, locked).err();
        let Some(HybridError::LockedConflict(conflict)) = error else {
            return Err(error.unwrap_or(HybridError::LockedParameters));
        };
        assert!(conflict.other.is_some());
        assert!((conflict.magnitude - 1.0).abs() < 1e-12);
        assert_eq!(conflict.position, [20.0, 0.0, 11.0]);
        assert!(conflict.suggestion().contains("intersecting controls"));
    }
    Ok(())
}

#[test]
fn within_tolerance_the_fixed_value_wins_exactly_including_signed_zero() -> Result<(), HybridError>
{
    let (points, triangles) = base(|_, _| 0.0);
    let stations = [station(0.0, 0.0, 0.0), station(40.0, 0.0, 0.0)];
    let input = request(&points, &triangles, &stations);
    let spots = [10.0, 0.0, -0.0, 10.0, 0.0, 0.5e-6, 20.0, 0.0, 0.5e-6];
    let built = build_locked(
        &input,
        Locked {
            points: &spots,
            lines: &[],
        },
    )?;
    assert_eq!(built.locked.len(), 2);
    let first = built
        .locked
        .iter()
        .find(|v| v.supports.contains(&LockRef::Point(0)));
    assert_eq!(
        first.map(|v| v.height.to_bits()),
        Some((-0.0_f64).to_bits())
    );
    for lock in &built.locked {
        assert_eq!(
            at(&built, lock.vertex).map(|p| p[2].to_bits()),
            Some(lock.height.to_bits())
        );
    }
    Ok(())
}

#[test]
fn malformed_controls_and_controls_on_or_outside_the_region_refuse() {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let input = request(&points, &triangles, &stations);
    for locked in [
        Locked {
            points: &[1.0, 2.0],
            lines: &[],
        },
        Locked {
            points: &[1.0, 2.0, f64::NAN],
            lines: &[],
        },
        Locked {
            points: &[],
            lines: &[&[1.0, 2.0, 3.0]],
        },
        Locked {
            points: &[],
            lines: &[&[1.0, 2.0, 3.0, 1.0, 2.0, 4.0]],
        },
        Locked {
            points: &[],
            lines: &[&[1.0, 2.0, 3.0, f64::INFINITY, 2.0, 4.0]],
        },
    ] {
        assert_eq!(
            build_locked(&input, locked).err(),
            Some(HybridError::LockedParameters)
        );
    }
    for spot in [[-10.0, 0.0, 0.0], [55.0, 0.0, 0.0]] {
        assert_eq!(
            build_locked(
                &input,
                Locked {
                    points: &spot,
                    lines: &[]
                }
            )
            .err(),
            Some(HybridError::Region(RegionError::Point))
        );
    }
    let line = [-10.0, 0.0, 0.0, 10.0, 0.0, 0.0];
    assert_eq!(
        build_locked(
            &input,
            Locked {
                points: &[],
                lines: &[&line]
            }
        )
        .err(),
        Some(HybridError::Region(RegionError::Breakline))
    );
}

#[test]
fn a_fixed_control_does_not_create_an_unauthored_ramp_to_a_different_height() {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let spot = [-5.0, -10.0, 99.0];
    assert!(matches!(
        build_locked(
            &request(&points, &triangles, &stations),
            Locked {
                points: &spot,
                lines: &[]
            }
        ),
        Err(HybridError::LockedConflict(_))
    ));
}

#[test]
fn fixed_heights_do_not_weaken_bitwise_daylight_preservation() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let input = request(&points, &triangles, &stations);
    let baseline = build(&input)?;
    let line = baseline
        .lines
        .iter()
        .position(|kind| matches!(kind, LineKind::Daylight { .. }))
        .ok_or(HybridError::LockedParameters)?;
    let vertex = *baseline
        .mesh
        .breakline_vertices
        .get(line)
        .and_then(|vertices| vertices.first())
        .ok_or(HybridError::LockedParameters)?;
    let [x, y, z] = at(&baseline, vertex).ok_or(HybridError::LockedParameters)?;
    let exact = build_locked(
        &input,
        Locked {
            points: &[x, y, z],
            lines: &[],
        },
    )?;
    let vertex = *exact
        .mesh
        .point_vertices
        .first()
        .ok_or(HybridError::LockedParameters)?;
    assert_eq!(
        at(&exact, vertex).map(|p| p[2].to_bits()),
        Some(z.to_bits())
    );
    let error = build_locked(
        &input,
        Locked {
            points: &[x, y, z + 0.5e-6],
            lines: &[],
        },
    )
    .err();
    assert!(
        matches!(error, Some(HybridError::LockedConflict(c)) if c.magnitude.abs() < input.tolerance)
    );
    Ok(())
}

#[test]
fn overflow_in_a_conflict_magnitude_refuses_instead_of_reporting_infinity() {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let spots = [10.0, 0.0, -1e308, 10.0, 0.0, 1e308];
    assert_eq!(
        build_locked(
            &request(&points, &triangles, &stations),
            Locked {
                points: &spots,
                lines: &[]
            }
        )
        .err(),
        Some(HybridError::LockedParameters)
    );
}

#[test]
fn seeded_controls_hold_at_survey_coordinates_without_narrowing() -> Result<(), HybridError> {
    let mut seed = 19_u32;
    for origin in [0.0, 1_000_000.0] {
        let (mut points, triangles) = base(worked);
        let mut stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
        let mut region = REGION;
        for [x, y, _] in points
            .as_chunks_mut::<3>()
            .0
            .iter_mut()
            .chain(region.as_mut_slice().as_chunks_mut::<3>().0.iter_mut())
        {
            *x += origin;
            *y += origin;
        }
        for station in &mut stations {
            station.position[0] += origin;
            station.position[1] += origin;
        }
        let mut spots = Vec::new();
        for _ in 0..12 {
            seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let x = 1.0 + 38.0 * f64::from(seed) / f64::from(u32::MAX);
            spots.extend([origin + x, origin + 0.75, 10.0 + 0.05 * x]);
        }
        let mut input = request(&points, &triangles, &stations);
        input.region = &region;
        let built = build_locked(
            &input,
            Locked {
                points: &spots,
                lines: &[],
            },
        )?;
        for (&vertex, spot) in built.mesh.point_vertices.iter().zip(plan::vertices(&spots)) {
            assert_eq!(at(&built, vertex), Some(*spot));
        }
    }
    Ok(())
}
