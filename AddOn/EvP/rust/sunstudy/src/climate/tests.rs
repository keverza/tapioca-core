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

fn setup(dome: Dome, normals: Vec<[f64; 3]>, path: u8, sky: DiffuseSky, tau: f64) -> ClimateSetup {
    let mut masks = SkyMasks::new(normals.len(), dome.len());
    for s in 0..normals.len() {
        masks.set(s, &vec![path; dome.len()]).unwrap();
    }
    ClimateSetup {
        dome,
        normals,
        masks,
        sky,
        canopy_transmittance: [tau; 12],
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
    c.finish()
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
fn masks_give_the_geometric_view_factor() {
    let dome = Dome::new(PatchSet::Tregenza);
    let mut masks = SkyMasks::new(2, dome.len());
    masks.set(0, &vec![OPEN; dome.len()]).unwrap();
    let half: Vec<u8> = (0..dome.len())
        .map(|p| if p % 2 == 0 { OPEN } else { CANOPY })
        .collect();
    masks.set(1, &half).unwrap();
    assert!((masks.view_factor(&dome, 0, [0.0, 0.0, 1.0], 0.0) - 1.0).abs() < 1e-12);
    let leafless = masks.view_factor(&dome, 1, [0.0, 0.0, 1.0], 1.0);
    assert!((leafless - 1.0).abs() < 1e-12);
    assert_eq!(masks.path(1, 1), Path::Canopy);
    assert_eq!(masks.path(1, 0), Path::Open);
    assert!(masks.set(0, &[OPEN]).is_err());
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
    let before = c.clone().finish();
    assert_eq!(c.add(&noon, None), Err(ClimateRefusal::Beam));
    assert_eq!(
        c.add(&noon, Some(&[OPEN, OPEN])),
        Err(ClimateRefusal::Length)
    );
    assert_eq!(c.finish(), before);
}
