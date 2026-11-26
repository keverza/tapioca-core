#![expect(
    clippy::unwrap_used,
    clippy::indexing_slicing,
    reason = "test assertions"
)]

use super::*;
use crate::sky::PatchSet;

const OPEN: u8 = Path::Open as u8;
const CANOPY: u8 = Path::Canopy as u8;
const BLOCKED: u8 = Path::Blocked as u8;

/// Unit vector at altitude and azimuth (degrees, clockwise from +y).
fn at(altitude: f64, azimuth: f64) -> [f64; 3] {
    let (a, p) = (altitude.to_radians(), azimuth.to_radians());
    [p.sin() * a.cos(), p.cos() * a.cos(), a.sin()]
}

/// A path as (open, canopy) mask shares over a whole patch.
fn shares(path: u8) -> (u8, u8) {
    match Path::from_u8(path) {
        Path::Open => (SHARE_FULL, 0),
        Path::Canopy => (0, SHARE_FULL),
        Path::Blocked => (0, 0),
    }
}

fn setup(dome: Dome, normals: Vec<[f64; 3]>, path: u8, sky: DiffuseSky, tau: f64) -> ClimateSetup {
    let mut masks = SkyMasks::new(normals.len(), dome.len());
    for s in 0..normals.len() {
        let (open, canopy) = shares(path);
        masks
            .set(s, &vec![open; dome.len()], &vec![canopy; dome.len()])
            .unwrap();
    }
    ClimateSetup {
        dome,
        normals,
        masks,
        sky,
        canopy_transmittance: [tau; 12],
        lux: None,
    }
}

/// A clear June day and an overcast one, hourly, sun from an analytic arc.
fn day(month: u8, overcast: bool) -> Vec<ClimateInterval> {
    (5..20)
        .map(|h| {
            let t = (f64::from(h) + 0.5 - 12.5) / 7.5; // −1..1 across the day
            let altitude = 60.0 * (1.0 - t * t) - 8.0;
            let (dni, dhi) = if overcast {
                (0.0, 150.0 * (1.0 - t * t))
            } else {
                (750.0 * (1.0 - t * t), 110.0)
            };
            ClimateInterval {
                month,
                hours: 1.0,
                day_of_year: 172,
                sun: at(altitude, 180.0 + 100.0 * t),
                dni,
                dhi,
                albedo: 0.3,
                dew_point_c: 12.0,
                // The file's own GHI, consistent with its DNI and DHI.
                ghi: dni * (altitude.to_radians().sin()).max(0.0) + dhi,
                file_lux: [f64::NAN; 3],
            }
        })
        .collect()
}

fn run(setup: ClimateSetup, intervals: &[ClimateInterval], beam: u8) -> ClimateTotals {
    let samples = setup.normals.len();
    let mut c = Climate::new(setup).unwrap();
    for i in intervals {
        let paths = vec![beam; samples];
        c.add(i, i.needs_beam().then_some(paths.as_slice()))
            .unwrap();
    }
    c.totals()
}

fn close(a: f64, b: f64, rel: f64) -> bool {
    (a - b).abs() <= rel * b.abs().max(1e-9)
}

#[test]
fn an_open_horizontal_sample_receives_the_models_horizontal_total() {
    for set in [PatchSet::Tregenza, PatchSet::Reinhart] {
        let mut intervals = day(5, false);
        intervals.extend(day(5, true));
        let t = run(
            setup(
                Dome::new(set),
                vec![[0.0, 0.0, 1.0]],
                OPEN,
                DiffuseSky::Perez1993,
                0.0,
            ),
            &intervals,
            OPEN,
        );
        let open = t.report.open_horizontal[5];
        assert!(
            close(t.direct[0][5] + t.diffuse[0][5], open, 1e-9),
            "{} vs {open}",
            t.total(0, 5)
        );
        assert_eq!(t.reflected[0][5], 0.0);
        assert!(close(t.total(0, 5), open, 1e-9));
        // Other months untouched.
        assert_eq!(t.total(0, 4), 0.0);
    }
}

#[test]
fn an_open_vertical_sample_under_an_isotropic_sky_sees_half_the_sky_and_half_the_ground() {
    let intervals = day(6, true);
    let t = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            vec![at(0.0, 0.0)],
            OPEN,
            DiffuseSky::Isotropic,
            0.0,
        ),
        &intervals,
        OPEN,
    );
    let dhi: f64 = intervals.iter().map(|i| i.dhi).sum();
    assert!(
        close(t.diffuse[0][6], 0.5 * dhi, 1e-4),
        "{} vs {}",
        t.diffuse[0][6],
        0.5 * dhi
    );
    assert!(close(t.reflected[0][6], 0.5 * 0.3 * dhi, 1e-12));
    assert_eq!(t.direct[0][6], 0.0);
}

#[test]
fn a_blocked_sample_keeps_only_the_unshaded_ground() {
    let t = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            vec![at(0.0, 180.0)],
            BLOCKED,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &day(5, false),
        BLOCKED,
    );
    assert_eq!(t.direct[0][5], 0.0);
    assert_eq!(t.diffuse[0][5], 0.0);
    assert!(t.reflected[0][5] > 0.0);
}

#[test]
fn canopy_passes_the_months_transmittance() {
    let normals = vec![at(0.0, 180.0)];
    let intervals = day(5, false);
    let open = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            normals.clone(),
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &intervals,
        OPEN,
    );
    let leafy = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            normals,
            CANOPY,
            DiffuseSky::Perez1993,
            0.25,
        ),
        &intervals,
        CANOPY,
    );
    assert!(close(leafy.direct[0][5], 0.25 * open.direct[0][5], 1e-12));
    assert!(close(leafy.diffuse[0][5], 0.25 * open.diffuse[0][5], 1e-12));
    assert!(close(leafy.reflected[0][5], open.reflected[0][5], 1e-12));
}

#[test]
fn the_same_scene_turned_about_z_gives_the_same_totals() {
    let turn = |v: [f64; 3], a: f64| {
        let (s, c) = a.sin_cos();
        [c * v[0] - s * v[1], s * v[0] + c * v[1], v[2]]
    };
    let normals = vec![at(0.0, 150.0), at(35.0, 200.0), [0.0, 0.0, 1.0]];
    let intervals = day(5, false);
    let base = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            normals.clone(),
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &intervals,
        OPEN,
    );
    let angle = 0.7;
    let turned_intervals: Vec<_> = intervals
        .iter()
        .map(|i| ClimateInterval {
            sun: turn(i.sun, angle),
            ..*i
        })
        .collect();
    let turned_normals = normals.iter().map(|&n| turn(n, angle)).collect();
    let turned = run(
        setup(
            Dome::new(PatchSet::Tregenza).rotated(angle),
            turned_normals,
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &turned_intervals,
        OPEN,
    );
    for s in 0..3 {
        assert!(close(turned.direct[s][5], base.direct[s][5], 1e-9));
        assert!(
            close(turned.diffuse[s][5], base.diffuse[s][5], 1e-9),
            "{s}: {} vs {}",
            turned.diffuse[s][5],
            base.diffuse[s][5]
        );
    }
}

#[test]
fn missing_radiation_adds_nothing_and_is_counted() {
    let mut intervals = day(5, false);
    intervals[3].dni = f64::NAN;
    let complete = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            vec![[0.0, 0.0, 1.0]],
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &day(5, false),
        OPEN,
    );
    let gappy = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            vec![[0.0, 0.0, 1.0]],
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &intervals,
        OPEN,
    );
    assert_eq!(gappy.report.missing_hours[5], 1.0);
    assert_eq!(
        gappy.report.valid_hours[5],
        complete.report.valid_hours[5] - 1.0
    );
    assert!(gappy.total(0, 5) < complete.total(0, 5));
}

#[test]
fn a_sun_below_the_horizon_falls_back_to_an_isotropic_sky_and_reports_the_lost_beam() {
    let t = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            vec![[0.0, 0.0, 1.0]],
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &day(5, false),
        OPEN,
    );
    // The arc starts and ends below the horizon.
    assert!(t.report.isotropic_intervals >= 2);
    assert!(t.report.beam_below_horizon > 0.0);
}

#[test]
fn a_needed_beam_and_matching_lengths_are_required() {
    let s = setup(
        Dome::new(PatchSet::Tregenza),
        vec![[0.0, 0.0, 1.0]],
        OPEN,
        DiffuseSky::Perez1993,
        0.0,
    );
    let mut c = Climate::new(s).unwrap();
    let noon = day(5, false)[7];
    assert_eq!(c.add(&noon, None), Err(ClimateRefusal::Beam));
    assert_eq!(
        c.add(&noon, Some(&[OPEN, OPEN])),
        Err(ClimateRefusal::Length)
    );
    assert_eq!(
        c.add(&ClimateInterval { month: 12, ..noon }, Some(&[OPEN])),
        Err(ClimateRefusal::Interval)
    );
    let mut bad = setup(
        Dome::new(PatchSet::Tregenza),
        vec![[0.0, 0.0, 1.0]],
        OPEN,
        DiffuseSky::Perez1993,
        0.0,
    );
    bad.normals.push([0.0, 0.0, 1.0]);
    assert!(Climate::new(bad).is_err());
}

#[test]
fn masks_give_the_geometric_view_factor_from_their_shares() {
    let dome = Dome::new(PatchSet::Tregenza);
    let n = dome.len();
    let mut masks = SkyMasks::new(3, n);
    masks.set(0, &vec![SHARE_FULL; n], &[]).unwrap();
    // Half of every patch open, the other half behind canopy.
    masks
        .set(
            1,
            &vec![SHARE_FULL / 2; n],
            &vec![SHARE_FULL - SHARE_FULL / 2; n],
        )
        .unwrap();
    let up = [0.0, 0.0, 1.0];
    assert!((masks.view_factor(&dome, 0, up, 0.0) - 1.0).abs() < 1e-12);
    let open_half = f64::from(SHARE_FULL / 2) / f64::from(SHARE_FULL);
    assert!((masks.view_factor(&dome, 1, up, 0.0) - open_half).abs() < 1e-12);
    assert!((masks.view_factor(&dome, 1, up, 1.0) - 1.0).abs() < 1e-12);
    assert!((masks.pass(1, 7, 0.5) - (open_half + 0.5 * (1.0 - open_half))).abs() < 1e-12);
    // An untouched sample sees nothing.
    assert_eq!(masks.view_factor(&dome, 2, up, 1.0), 0.0);
    assert!(masks.set(0, &[SHARE_FULL], &[]).is_err());
    assert!(
        masks.set(0, &vec![200; n], &vec![100; n]).is_err(),
        "more than the whole patch"
    );
}

#[test]
fn the_daily_light_integral_converts_energy_over_the_hours_present() {
    // 6 kWh/m² in one full day: 6000 × 3600 × 0.45 × 4.57 µmol = 44.42 mol.
    let dli = daily_light_integral(6000.0, 24.0, ParConversion::default()).unwrap();
    assert!((dli - 44.4204).abs() < 1e-3, "{dli}");
    // The same energy over two days' worth of valid hours is half per day.
    let two = daily_light_integral(6000.0, 48.0, ParConversion::default()).unwrap();
    assert!((two - 0.5 * dli).abs() < 1e-12);
    assert_eq!(
        daily_light_integral(1.0, 0.0, ParConversion::default()),
        None
    );
}

#[test]
fn a_refused_interval_changes_nothing() {
    let s = setup(
        Dome::new(PatchSet::Tregenza),
        vec![[0.0, 0.0, 1.0]],
        OPEN,
        DiffuseSky::Perez1993,
        0.0,
    );
    let mut c = Climate::new(s).unwrap();
    let noon = day(5, false)[7];
    let before = c.totals();
    assert_eq!(c.add(&noon, None), Err(ClimateRefusal::Beam));
    assert_eq!(
        c.add(&noon, Some(&[OPEN, OPEN])),
        Err(ClimateRefusal::Length)
    );
    assert_eq!(c.totals(), before);
}

#[test]
fn a_consistent_file_closes_and_its_ghi_never_moves_a_total() {
    let normals = vec![[0.0, 0.0, 1.0]];
    let intervals: Vec<_> = day(5, false).into_iter().chain(day(5, true)).collect();
    let totals = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            normals.clone(),
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &intervals,
        OPEN,
    );
    let closure = horizontal_closure(&totals.report);
    assert!(closure.annual.abs() < 1e-12, "{}", closure.annual);
    assert!(closure.monthly[5].abs() < 1e-12);
    assert!(closure.monthly[0].is_nan()); // nothing to compare in January
    assert_eq!(closure.worst_month, Some(5));
    assert!(closure.within);
    assert!((closure.hours - 30.0).abs() < 1e-12);

    // A file whose GHI reads 10 % high: flagged, and no total moves.
    let high: Vec<_> = intervals
        .iter()
        .map(|i| ClimateInterval {
            ghi: i.ghi * 1.1,
            ..*i
        })
        .collect();
    let again = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            normals,
            OPEN,
            DiffuseSky::Perez1993,
            0.0,
        ),
        &high,
        OPEN,
    );
    assert_eq!(again.direct, totals.direct);
    assert_eq!(again.diffuse, totals.diffuse);
    assert_eq!(again.reflected, totals.reflected);
    let flagged = horizontal_closure(&again.report);
    assert!((flagged.annual - (1.0 / 1.1 - 1.0)).abs() < 1e-12);
    assert!(!flagged.within);
}

#[test]
fn hours_without_ghi_or_a_dark_month_are_not_compared() {
    let mut intervals = day(5, false);
    for i in intervals.iter_mut().take(4) {
        i.ghi = f64::NAN;
    }
    // A dark December: 0.5 kWh/m² in the file, below the comparison floor.
    intervals.push(ClimateInterval {
        month: 11,
        hours: 1.0,
        day_of_year: 355,
        sun: at(5.0, 180.0),
        dni: 0.0,
        dhi: 500.0,
        albedo: 0.2,
        dew_point_c: 0.0,
        ghi: 500.0,
        file_lux: [f64::NAN; 3],
    });
    let totals = run(
        setup(
            Dome::new(PatchSet::Tregenza),
            vec![[0.0, 0.0, 1.0]],
            OPEN,
            DiffuseSky::Isotropic,
            0.0,
        ),
        &intervals,
        OPEN,
    );
    let r = &totals.report;
    assert!((r.compared_hours[5] - 11.0).abs() < 1e-12);
    assert!(r.compared_model[5] < r.open_horizontal[5]);
    let closure = horizontal_closure(r);
    assert!(closure.annual.abs() < 1e-12);
    assert!(closure.monthly[11].is_nan());
    assert_eq!(closure.worst_month, Some(5));
}
