use super::*;

fn rectangle(origin: [f64; 2]) -> (Vec<f64>, Vec<u32>) {
    let points = [
        -20.0, -30.0, 10.0, 60.0, -30.0, 10.0, 60.0, 30.0, 10.0, -20.0, 30.0, 10.0,
    ];
    let mut points = points.to_vec();
    for [x, y, _] in points.as_chunks_mut::<3>().0 {
        *x += origin[0];
        *y += origin[1];
    }
    (points, vec![0, 1, 2, 0, 2, 3])
}

fn peaked() -> Vec<Station> {
    [
        station(0.0, 11.0, 0.0),
        station(18.0, 11.0, 0.0),
        station(20.0, 13.0, 0.0),
        station(22.0, 11.0, 0.0),
        station(40.0, 11.0, 0.0),
    ]
    .into_iter()
    .enumerate()
    .map(|(i, mut section)| {
        if i == 2 {
            section.half_left = 8.0;
            section.half_right = 8.0;
        }
        section
    })
    .collect()
}

#[test]
fn two_triangle_ground_does_not_bridge_over_a_sampled_profile_and_width_peak()
-> Result<(), HybridError> {
    let (points, triangles) = rectangle([0.0, 0.0]);
    let stations = peaked();
    let input = BuildRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        band: Band {
            stations: &stations,
            effect: Effect::Both,
        },
        tolerance: 1e-6,
    };
    let base = Field::new(&points, &triangles).map_err(|_| HybridError::Field)?;
    let design = Design::new(&input.band, &base, input.tolerance).map_err(HybridError::Design)?;
    let built = build(&input)?;
    let proposed =
        Field::new(&built.points, &built.mesh.triangles).map_err(|_| HybridError::Field)?;
    for y in [-7.0, -3.0, 0.0, 3.0, 7.0] {
        let actual = proposed
            .height_at(20.0, y)
            .ok_or(HybridError::Field)?
            .height;
        let expected = design
            .height(20.0, y)
            .map_err(HybridError::Design)?
            .ok_or(HybridError::Field)?
            .height;
        assert!(
            (actual - expected).abs() <= 1e-9,
            "coarse source at station 20, lateral {y}: {actual} vs {expected}"
        );
    }
    assert!(built.mesh.triangles.len() > triangles.len());
    assert_eq!(built.volumes.context.cut.to_bits(), 0.0_f64.to_bits());
    assert_eq!(built.volumes.context.fill.to_bits(), 0.0_f64.to_bits());
    assert_eq!(build(&input)?, built);
    Ok(())
}

#[test]
fn sections_match_band_shoulders_and_daylight_on_coarse_and_dense_survey_ground()
-> Result<(), HybridError> {
    for origin in [[0.0, 0.0], [1_000_000.0, 6_000_000.0]] {
        for coarse in [true, false] {
            let (points, triangles) = if coarse {
                rectangle(origin)
            } else {
                let (mut points, triangles) = base(|_, _| 10.0);
                for [x, y, _] in points.as_chunks_mut::<3>().0 {
                    *x += origin[0];
                    *y += origin[1];
                }
                (points, triangles)
            };
            let mut stations = peaked();
            for section in &mut stations {
                section.position[0] += origin[0];
                section.position[1] += origin[1];
                section.shoulder_left = 0.5;
                section.shoulder_right = 0.25;
                section.crossfall = Some(Camber::Crown(Slope::Percent(2.0)));
            }
            let mut region = REGION;
            for [x, y, _] in region.as_mut_slice().as_chunks_mut::<3>().0 {
                *x += origin[0];
                *y += origin[1];
            }
            let request = BuildRequest {
                points: &points,
                triangles: &triangles,
                region: &region,
                band: Band {
                    stations: &stations,
                    effect: Effect::Both,
                },
                tolerance: 1e-6,
            };
            let field = Field::new(&points, &triangles).map_err(|_| HybridError::Field)?;
            let design = Design::new(&request.band, &field, request.tolerance)
                .map_err(HybridError::Design)?;
            let built = build(&request)?;
            let proposed =
                Field::new(&built.points, &built.mesh.triangles).map_err(|_| HybridError::Field)?;
            assert_eq!(
                built.mesh.points.get(..points.len()),
                Some(points.as_slice())
            );
            for station in &stations {
                for lateral in [
                    -station.half_right - 1.0,
                    -station.half_right - 0.125,
                    -station.half_right,
                    -0.5 * station.half_right,
                    0.0,
                    0.5 * station.half_left,
                    station.half_left,
                    station.half_left + 0.25,
                    station.half_left + 1.0,
                ] {
                    let [x, y, _] = station.position;
                    let actual = proposed
                        .height_at(x, y + lateral)
                        .ok_or(HybridError::Field)?
                        .height;
                    let expected = design
                        .height(x, y + lateral)
                        .map_err(HybridError::Design)?
                        .ok_or(HybridError::Field)?
                        .height;
                    assert!(
                        (actual - expected).abs() < 1e-8,
                        "{coarse} {origin:?} {x} {lateral}: {actual} vs {expected}"
                    );
                }
            }
            let sections: Vec<_> = built
                .lines
                .iter()
                .filter_map(|kind| match kind {
                    LineKind::Section(station) => Some(*station),
                    _ => None,
                })
                .collect();
            assert_eq!(sections, [1, 2, 3]);
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
                        assert_eq!(
                            plan::vertices(&built.points)
                                .get(vertex as usize)
                                .map(|p| p[2].to_bits()),
                            plan::vertices(&built.mesh.points)
                                .get(vertex as usize)
                                .map(|p| p[2].to_bits())
                        );
                    }
                }
            }
            assert_eq!(built.volumes.context.cut.to_bits(), 0.0_f64.to_bits());
            assert_eq!(built.volumes.context.fill.to_bits(), 0.0_f64.to_bits());
        }
    }
    Ok(())
}

#[test]
fn transverse_station_edges_retain_compatible_fixed_controls_and_locate_conflicts()
-> Result<(), HybridError> {
    let (points, triangles) = rectangle([0.0, 0.0]);
    let stations = peaked();
    let input = BuildRequest {
        points: &points,
        triangles: &triangles,
        region: &REGION,
        band: Band {
            stations: &stations,
            effect: Effect::Both,
        },
        tolerance: 1e-6,
    };
    let built = build_locked(
        &input,
        Locked {
            points: &[20.0, 0.0, 13.0],
            lines: &[],
        },
    )?;
    assert_eq!(built.locked.len(), 1);
    let lock = built.locked.first().ok_or(HybridError::LockedParameters)?;
    assert_eq!(lock.supports, [LockRef::Point(0)]);
    assert_eq!(lock.height.to_bits(), 13.0_f64.to_bits());
    assert_eq!(built.owners.get(lock.vertex as usize), Some(&None));
    let found = assess_locked(
        &input,
        Locked {
            points: &[20.0, 0.0, 11.0],
            lines: &[],
        },
    )?;
    assert!(found.fixed_complete);
    assert_eq!(found.locked.len(), 1);
    let conflict = found.locked.first().ok_or(HybridError::LockedParameters)?;
    assert_eq!(conflict.control, LockRef::Point(0));
    assert_eq!(conflict.position, [20.0, 0.0, 11.0]);
    assert!((conflict.magnitude - 2.0).abs() < 1e-10);
    Ok(())
}
