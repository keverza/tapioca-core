use super::*;
use crate::slope::Slope;
use crate::terrain::daylight::Locus;
use crate::terrain::design::{Camber, Station};
use crate::terrain::edit::Effect;
use crate::terrain::hybrid::conflict::{Conflict, ConflictCode};
use crate::terrain::volume::exact::triangle_cut_fill;
use crate::terrain::volume::{Grid, Proposed, VolumeRequest, cut_fill};

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// A 5-m grid over `[-20.5, 59.5] x [-30.5, 29.5]` carrying `z`; the half-metre
/// offset keeps grid lines off the band's own lines.
pub(super) fn base(z: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let (columns, rows) = (17_u32, 13_u32);
    let mut points = Vec::new();
    for row in 0..rows {
        for column in 0..columns {
            let (x, y) = (
                -20.5 + 5.0 * f64::from(column),
                -30.5 + 5.0 * f64::from(row),
            );
            points.extend([x, y, z(x, y)]);
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

pub(super) fn worked(x: f64, y: f64) -> f64 {
    10.0 + 0.05 * x - 0.2 * y
}

pub(super) const REGION: [f64; 12] = [
    -10.0, -15.0, 0.0, 50.0, -15.0, 0.0, 50.0, 15.0, 0.0, -10.0, 15.0, 0.0,
];

const HALF: Slope = Slope::RiseRun {
    rise: 1.0,
    run: 2.0,
};

pub(super) fn station(x: f64, z: f64, shoulder: f64) -> Station {
    Station {
        position: [x, 0.0, z],
        half_left: 2.0,
        half_right: 2.0,
        crossfall: None,
        shoulder_left: shoulder,
        shoulder_right: shoulder,
        cut: HALF,
        fill: HALF,
        cut_limit: 10.0,
        fill_limit: 10.0,
    }
}

fn run(points: &[f64], triangles: &[u32], stations: &[Station]) -> Result<Built, HybridError> {
    build(&BuildRequest {
        points,
        triangles,
        region: &REGION,
        band: Band {
            stations,
            effect: Effect::Both,
        },
        tolerance: 1e-6,
    })
}

/// `[fill, cut]` of the built triangles whose centroid lies in `x0 < x < x1`.
pub(super) fn between(built: &Built, x0: f64, x1: f64) -> [f64; 2] {
    let proposed = plan::vertices(&built.points);
    let based = plan::vertices(&built.mesh.points);
    let mut total = [0.0, 0.0];
    for triple in built.mesh.triangles.as_chunks::<3>().0 {
        let mut corners = [[0.0; 2]; 3];
        let mut difference = [0.0; 3];
        for ((corner, delta), &v) in corners.iter_mut().zip(&mut difference).zip(triple) {
            let (Some(&[x, y, z]), Some(&[_, _, b])) =
                (proposed.get(v as usize), based.get(v as usize))
            else {
                return [f64::NAN; 2];
            };
            *corner = [x, y];
            *delta = z - b;
        }
        let centroid = corners.iter().map(|c| c[0]).sum::<f64>() / 3.0;
        if centroid > x0 && centroid < x1 {
            let [fill, cut] = triangle_cut_fill(corners, difference);
            total = [total[0] + fill, total[1] + cut];
        }
    }
    total
}

#[test]
fn worked_dense_stations() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    for count in [8, 16, 20, 40] {
        let stations: Vec<_> = (0..=count)
            .map(|i| {
                let x = f64::from(i) * 40.0 / f64::from(count);
                station(x, 10.0 + 0.05 * x, 0.0)
            })
            .collect();
        let built = run(&points, &triangles, &stations)?;
        assert_eq!(run(&points, &triangles, &stations)?, built);
        let [fill, cut] = between(&built, 0.0, 40.0);
        assert!(
            close(fill, 80.0 / 3.0, 1e-9),
            "{count} segments: fill {fill}"
        );
        assert!(close(cut, 80.0 / 3.0, 1e-9), "{count} segments: cut {cut}");
    }
    Ok(())
}

#[test]
fn worked_fixture_is_exact_on_the_built_tin() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let built = run(&points, &triangles, &stations)?;
    assert_eq!(run(&points, &triangles, &stations)?, built, "deterministic");
    assert_eq!(
        built.lines,
        vec![
            LineKind::Edge(Hand::Left),
            LineKind::Daylight {
                side: Hand::Left,
                root: 0
            },
            LineKind::Edge(Hand::Right),
            LineKind::Daylight {
                side: Hand::Right,
                root: 0
            },
            LineKind::End(false),
            LineKind::End(true),
        ]
    );
    // Between the end sections the surfaces are planar pieces whose creases are
    // all constraints, so the triangle sum is the analytic volume.
    let [fill, cut] = between(&built, 0.0, 40.0);
    assert!(
        close(fill, 80.0 / 3.0, 1e-9) && close(cut, 80.0 / 3.0, 1e-9),
        "{fill} {cut}"
    );
    // The ends add their own (unconstrained cap) volume to the site total.
    assert!(built.volumes.site.fill >= fill - 1e-9 && built.volumes.site.cut >= cut - 1e-9);
    assert_eq!(built.volumes.context.fill, 0.0);
    assert_eq!(built.volumes.context.cut, 0.0);

    // Heights: constraint vertices exact, outside untouched.
    let proposed = plan::vertices(&built.points);
    for (&[x, y, z], (&[_, _, b], owner)) in proposed
        .iter()
        .zip(plan::vertices(&built.mesh.points).iter().zip(&built.owners))
    {
        let inside_band = (0.0..=40.0).contains(&x) && y.abs() <= 2.0 + 1e-12;
        if inside_band {
            assert!(close(z, 10.0 + 0.05 * x, 1e-12), "{x},{y}");
        }
        if (y.abs() - (2.0 + 4.0 / 3.0)).abs() <= 1e-9 || x < -10.0 || x > 50.0 || y.abs() > 15.0 {
            assert_eq!(z.to_bits(), b.to_bits(), "{x},{y}");
            assert_eq!(*owner, None);
        }
        assert!(close(b, worked(x, y), 1e-12));
    }
    // No triangle straddles a band edge or a daylight line.
    for triple in built.mesh.triangles.as_chunks::<3>().0 {
        let ys: Vec<f64> = triple
            .iter()
            .filter_map(|&v| proposed.get(v as usize).map(|p| p[1]))
            .collect();
        let (low, high) = ys
            .iter()
            .fold((f64::INFINITY, f64::NEG_INFINITY), |(l, h), &y| {
                (l.min(y), h.max(y))
            });
        for line in [-10.0 / 3.0, -2.0, 2.0, 10.0 / 3.0] {
            let xs: Vec<f64> = triple
                .iter()
                .filter_map(|&v| proposed.get(v as usize).map(|p| p[0]))
                .collect();
            if xs.iter().all(|&x| (0.0..=40.0).contains(&x)) {
                assert!(!(low < line - 1e-9 && high > line + 1e-9), "{triple:?}");
            }
        }
    }
    // Outside the region every triangle is its input triangle or a split piece.
    let kept = built.mesh.kept.iter().filter(|&&k| k).count();
    assert!(kept > 0);

    // Grid oracle over the same band at 0.1 m: within 1 %.
    let existing = Field::new(&points, &triangles).map_err(|_| HybridError::Field)?;
    let surface =
        Field::new(&built.points, &built.mesh.triangles).map_err(|_| HybridError::Field)?;
    let oracle = cut_fill(&VolumeRequest {
        existing: &existing,
        proposed: Proposed::Field(&surface),
        grid: Grid {
            cell: 0.1,
            subdivisions: 1,
        },
        domain: Some([0.0, -15.0, 40.0, 15.0]),
        property: None,
    })
    .map_err(HybridError::Volume)?;
    assert!(
        close(oracle.site.fill, 80.0 / 3.0, 0.01 * 80.0 / 3.0),
        "{}",
        oracle.site.fill
    );
    assert!(
        close(oracle.site.cut, 80.0 / 3.0, 0.01 * 80.0 / 3.0),
        "{}",
        oracle.site.cut
    );
    Ok(())
}

#[test]
fn a_shoulder_becomes_its_own_line_and_keeps_the_volume_exact() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.5), station(40.0, 12.0, 0.5)];
    let built = run(&points, &triangles, &stations)?;
    assert!(built.lines.contains(&LineKind::Shoulder(Hand::Left)));
    assert!(built.lines.contains(&LineKind::Shoulder(Hand::Right)));
    let [fill, cut] = between(&built, 0.0, 40.0);
    assert!(
        close(fill, 125.0 / 3.0, 1e-9) && close(cut, 125.0 / 3.0, 1e-9),
        "{fill} {cut}"
    );
    Ok(())
}

#[test]
fn unmet_vertical_and_escaping_designs_refuse() {
    // Ground falling 70 % to the left: the 1:2 fill never meets it.
    let (steep, triangles) = base(|x, y| 10.0 + 0.05 * x - 0.7 * y);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    assert_eq!(
        run(&steep, &triangles, &stations).err(),
        Some(HybridError::DaylightNotReached {
            station: 0,
            side: Hand::Left
        })
    );
    let (points, triangles) = base(worked);
    let mut walls = stations;
    for station in &mut walls {
        station.cut = Slope::Angle {
            radians: std::f64::consts::FRAC_PI_2,
        };
    }
    assert_eq!(
        run(&points, &triangles, &walls).err(),
        Some(HybridError::Vertical {
            station: 0,
            side: Hand::Right
        })
    );
    // A band reaching past the region ring cannot be stitched.
    let long = [station(-20.0, 9.0, 0.0), station(40.0, 12.0, 0.0)];
    assert_eq!(
        run(&points, &triangles, &long).err(),
        Some(HybridError::Region(RegionError::Breakline))
    );
    assert!(
        HybridError::DaylightNotReached {
            station: 0,
            side: Hand::Left
        }
        .message()
        .starts_with("DaylightNotReached:")
    );
}

#[test]
fn a_crown_is_a_centreline_crease_and_stays_exact() -> Result<(), HybridError> {
    let (points, triangles) = base(worked);
    let mut stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    for station in &mut stations {
        station.crossfall = Some(Camber::Crown(Slope::Percent(2.0)));
    }
    let built = run(&points, &triangles, &stations)?;
    assert_eq!(
        built.lines,
        vec![
            LineKind::Edge(Hand::Left),
            LineKind::Daylight {
                side: Hand::Left,
                root: 0
            },
            LineKind::Edge(Hand::Right),
            LineKind::Daylight {
                side: Hand::Right,
                root: 0
            },
            LineKind::Crown,
            LineKind::End(false),
            LineKind::End(true),
        ]
    );
    // Edges 0.04 m below the centreline: the left (downhill) edge is now 0.36 m
    // above the ground (fill, daylight 1.2 m), the right 0.44 m below (cut,
    // daylight 22/15 m). Per metre: fill 0.36 + 0.216, cut 0.44 + 0.3227.
    let roots: Vec<f64> = built
        .daylight
        .iter()
        .flat_map(|pair| pair.iter())
        .filter_map(|side| match side {
            Some(Locus::Met { roots }) => roots.first().copied(),
            _ => None,
        })
        .collect();
    assert_eq!(roots.len(), 4);
    assert!(roots.chunks(2).all(|pair| matches!(pair, [l, r]
        if close(*l, 1.2, 1e-9) && close(*r, 22.0 / 15.0, 1e-9))));
    let [fill, cut] = between(&built, 0.0, 40.0);
    assert!(
        close(fill, 40.0 * 0.576, 1e-9)
            && close(cut, 40.0 * (0.44 + 0.5 * 0.44 * 22.0 / 15.0), 1e-9),
        "{fill} {cut}"
    );
    // The centreline vertices are exact and no band triangle straddles it.
    let proposed = plan::vertices(&built.points);
    for &[x, y, z] in proposed {
        if (0.0..=40.0).contains(&x) && y.abs() <= 2.0 + 1e-12 {
            assert!(close(z, 10.0 + 0.05 * x - 0.02 * y.abs(), 1e-12), "{x},{y}");
        }
    }
    for triple in built.mesh.triangles.as_chunks::<3>().0 {
        let corners: Vec<[f64; 3]> = triple
            .iter()
            .filter_map(|&v| proposed.get(v as usize).copied())
            .collect();
        if corners.iter().all(|c| (0.0..=40.0).contains(&c[0])) {
            let low = corners.iter().map(|c| c[1]).fold(f64::INFINITY, f64::min);
            let high = corners
                .iter()
                .map(|c| c[1])
                .fold(f64::NEG_INFINITY, f64::max);
            assert!(!(low < -1e-9 && high > 1e-9), "{triple:?}");
        }
    }
    Ok(())
}

fn request<'a>(
    points: &'a [f64],
    triangles: &'a [u32],
    stations: &'a [Station],
    effect: Effect,
) -> BuildRequest<'a> {
    BuildRequest {
        points,
        triangles,
        region: &REGION,
        band: Band { stations, effect },
        tolerance: 1e-6,
    }
}

fn codes(conflicts: &[Conflict]) -> Vec<(ConflictCode, u32, Hand)> {
    conflicts
        .iter()
        .map(|c| (c.code, c.station, c.side))
        .collect()
}

#[test]
fn every_hard_conflict_is_located() -> Result<(), HybridError> {
    // Ground falling 70 % to the left: neither the left 1:2 fill nor the right
    // 1:2 cut ever meets it; the build refuses at the first, assess finds all.
    let (steep, triangles) = base(|x, y| 10.0 + 0.05 * x - 0.7 * y);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let found = assess(&request(&steep, &triangles, &stations, Effect::Both))?;
    assert_eq!(
        codes(&found),
        vec![
            (ConflictCode::DaylightNotReached, 0, Hand::Left),
            (ConflictCode::DaylightNotReached, 1, Hand::Left),
            (ConflictCode::DaylightNotReached, 0, Hand::Right),
            (ConflictCode::DaylightNotReached, 1, Hand::Right),
        ]
    );
    // Each search stopped out beyond its own edge, still off the ground.
    assert!(found.iter().all(|c| {
        let outside = if c.side == Hand::Left {
            c.position[1] > 2.0
        } else {
            c.position[1] < -2.0
        };
        outside && c.magnitude.abs() > 0.0
    }));
    assert_eq!(
        run(&steep, &triangles, &stations).err(),
        Some(HybridError::DaylightNotReached {
            station: 0,
            side: Hand::Left
        })
    );

    // Limits shorter than the 4/3 m daylight: located at the limit, reach 4/3.
    let (points, triangles) = base(worked);
    let mut short = stations;
    for station in &mut short {
        station.cut_limit = 0.5;
        station.fill_limit = 0.5;
    }
    let found = assess(&request(&points, &triangles, &short, Effect::Both))?;
    assert_eq!(found.len(), 4);
    let first = found.first().copied().ok_or(HybridError::Field)?;
    assert_eq!(first.code, ConflictCode::DaylightBeyondLimit);
    assert!(close(first.position[0], 0.0, 1e-12) && close(first.position[1], 2.5, 1e-12));
    assert!(close(first.magnitude, 4.0 / 3.0, 1e-9));
    assert!(close(first.position[2], worked(0.0, 2.5), 1e-12));

    // A vertical cut face on the right.
    let mut walls = stations;
    for station in &mut walls {
        station.cut = Slope::Angle {
            radians: std::f64::consts::FRAC_PI_2,
        };
    }
    let found = assess(&request(&points, &triangles, &walls, Effect::Both))?;
    assert_eq!(
        codes(&found),
        vec![
            (ConflictCode::VerticalFace, 0, Hand::Right),
            (ConflictCode::VerticalFace, 1, Hand::Right),
        ]
    );
    assert!(found.iter().all(|c| close(c.position[1], -2.0, 1e-12)));

    // The left edge reaches the ground at the last station: fill, then at grade.
    let graded = [station(0.0, 10.0, 0.0), station(40.0, 11.6, 0.0)];
    let found = assess(&request(&points, &triangles, &graded, Effect::Both))?;
    assert_eq!(
        codes(&found),
        vec![(ConflictCode::DaylightTopology, 1, Hand::Left)]
    );
    assert!(found.iter().all(|c| c.magnitude == 0.0 && c.code.hard()));
    assert_eq!(
        run(&points, &triangles, &graded).err(),
        Some(HybridError::DaylightTopology { side: Hand::Left })
    );
    assert!(
        ConflictCode::DaylightTopology
            .suggestion()
            .contains("split")
    );
    Ok(())
}

#[test]
fn a_contradicted_edge_is_a_soft_located_conflict() -> Result<(), HybridError> {
    // Fill only: the right edge is in cut, so the band is held at the ground
    // there, but the band still builds and says where.
    let (points, triangles) = base(worked);
    let stations = [station(0.0, 10.0, 0.0), station(40.0, 12.0, 0.0)];
    let built = build(&request(&points, &triangles, &stations, Effect::Fill))?;
    assert_eq!(
        codes(&built.conflicts),
        vec![
            (ConflictCode::EdgeContradictsEffect, 0, Hand::Right),
            (ConflictCode::EdgeContradictsEffect, 1, Hand::Right),
        ]
    );
    assert!(
        built
            .conflicts
            .iter()
            .all(|c| close(c.magnitude, -0.4, 1e-12) && !c.code.hard())
    );
    assert_eq!(
        built
            .conflicts
            .first()
            .map(|c| [c.position[0], c.position[1]]),
        Some([0.0, -2.0])
    );
    // Nothing on the right moved down.
    let [_, cut] = between(&built, 0.0, 40.0);
    assert!(close(cut, 0.0, 1e-12), "{cut}");
    // The unrestricted band has none.
    assert_eq!(
        run(&points, &triangles, &stations)?.conflicts,
        Vec::<Conflict>::new()
    );
    Ok(())
}
