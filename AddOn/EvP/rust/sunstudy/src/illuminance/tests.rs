#![expect(
    clippy::unwrap_used,
    clippy::indexing_slicing,
    clippy::float_cmp,
    reason = "test assertions"
)]

use super::*;
use crate::climate::{Climate, ClimateInterval, ClimateSetup, DiffuseSky, SHARE_FULL, SkyMasks};
use crate::sky::PatchSet;

const OPEN: u8 = 2;
const BLOCKED: u8 = 0;

fn at(altitude_deg: f64, azimuth_deg: f64) -> [f64; 3] {
    let (a, z) = (altitude_deg.to_radians(), azimuth_deg.to_radians());
    [a.cos() * z.sin(), a.cos() * z.cos(), a.sin()]
}

fn climate(normals: Vec<[f64; 3]>, threshold: f64, source: LuxSource) -> Climate {
    let dome = Dome::new(PatchSet::Tregenza);
    let mut masks = SkyMasks::new(normals.len(), dome.len());
    for s in 0..normals.len() {
        masks.set(s, &vec![SHARE_FULL; dome.len()], &[]).unwrap();
    }
    Climate::new(ClimateSetup {
        dome,
        normals,
        masks,
        sky: DiffuseSky::Perez1993,
        canopy_transmittance: [0.0; 12],
        lux: Some(LuxOptions {
            threshold_lux: threshold,
            source,
        }),
    })
    .unwrap()
}

fn interval(sun: [f64; 3], dni: f64, dhi: f64) -> ClimateInterval {
    ClimateInterval {
        month: 5,
        hours: 1.0,
        day_of_year: 172,
        sun,
        dni,
        dhi,
        albedo: 0.2,
        dew_point_c: 10.0,
        ghi: f64::NAN,
        file_lux: [f64::NAN; 3],
    }
}

fn lux(c: &Climate) -> LuxTotals {
    c.totals().lux.unwrap()
}

/// An overcast hour (ε = 1, first bin) by hand from Perez 1990 Table 4's first
/// row, written out here rather than read from the module's table: the
/// diffuse efficacy a + b·W + c·cos Z + d·ln Δ, with W from the dew point and
/// Δ from Kasten's air mass and Spencer's eccentricity.
#[test]
fn an_overcast_hour_takes_the_first_bins_diffuse_efficacy() {
    let mut c = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    let altitude: f64 = 40.0;
    let dhi = 180.0;
    c.add(&interval(at(altitude, 180.0), 0.0, dhi), None)
        .unwrap();
    let zenith = (90.0 - altitude).to_radians();
    let air_mass = 1.0 / (zenith.cos() + 0.15 * (93.885 - (90.0 - altitude)).powf(-1.253));
    let g = 2.0 * std::f64::consts::PI * 171.0 / 365.0;
    let eccentricity = 1.000_11
        + 0.034_221 * g.cos()
        + 0.001_28 * g.sin()
        + 0.000_719 * (2.0 * g).cos()
        + 0.000_077 * (2.0 * g).sin();
    let delta = dhi * air_mass / (1367.0 * eccentricity);
    let water = (0.07 * 10.0 - 0.075_f64).exp();
    let kd = 97.24 - 0.46 * water + 12.00 * zenith.cos() - 8.91 * delta.ln();
    let t = lux(&c);
    // An open horizontal sample: the diffuse horizontal illuminance, nothing
    // from the ground, no beam.
    let got = t.lux_hours[0][5];
    assert!((got - dhi * kd).abs() < 1e-4 * got, "{got} vs {}", dhi * kd);
    assert!(
        (105.0..135.0).contains(&kd),
        "an overcast efficacy, lm/W: {kd}"
    );
    assert_eq!(t.report.efficacy_hours[5], 1.0);
    assert_eq!(t.report.file_hours[5], 0.0);
}

#[test]
fn a_clear_hour_adds_the_beam_and_the_sky_on_an_open_horizontal() {
    let mut c = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    let sun = at(55.0, 170.0);
    let (dni, dhi) = (780.0, 95.0);
    c.add(&interval(sun, dni, dhi), Some(&[OPEN])).unwrap();
    let p = parameters(&PerezInput {
        sun,
        dni,
        dhi,
        day_of_year: 172,
        dew_point_c: Some(10.0),
    })
    .unwrap();
    let (kd, kb) = efficacy(&p);
    let expected = dni * kb * sun[2] + dhi * kd;
    let got = lux(&c).lux_hours[0][5];
    assert!(
        (got - expected).abs() < 1e-4 * expected,
        "{got} vs {expected}"
    );
    // A clear summer noon: tens of kilolux, as published clear-sky values.
    assert!((60_000.0..120_000.0).contains(&got), "{got} lx");
    assert!(
        (80.0..115.0).contains(&kb),
        "a clear direct efficacy, lm/W: {kb}"
    );
}

#[test]
fn the_files_illuminance_is_used_when_valid_and_says_so() {
    let sun = at(50.0, 180.0);
    let mut i = interval(sun, 600.0, 120.0);
    i.file_lux = [70_000.0, 55_000.0, 25_000.0];
    let mut from_file = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    from_file.add(&i, Some(&[OPEN])).unwrap();
    let t = lux(&from_file);
    let expected = 55_000.0 * sun[2] + 25_000.0;
    assert!((t.lux_hours[0][5] - expected).abs() < 1e-4 * expected);
    assert_eq!(
        (t.report.file_hours[5], t.report.efficacy_hours[5]),
        (1.0, 0.0)
    );

    // Told to ignore the file: the efficacy, and the report says which.
    let mut derived = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::EfficacyOnly);
    derived.add(&i, Some(&[OPEN])).unwrap();
    let d = lux(&derived);
    assert_eq!(
        (d.report.file_hours[5], d.report.efficacy_hours[5]),
        (0.0, 1.0)
    );
    assert!((d.lux_hours[0][5] - expected).abs() > 1.0);

    // A file with only one of the two fields is not used.
    i.file_lux = [70_000.0, f64::NAN, 25_000.0];
    let mut half = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    half.add(&i, Some(&[OPEN])).unwrap();
    assert_eq!(lux(&half).report.efficacy_hours[5], 1.0);
}

#[test]
fn missing_radiation_is_unavailable_not_dark_unless_the_file_has_illuminance() {
    let sun = at(30.0, 180.0);
    let mut c = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    c.add(&interval(sun, f64::NAN, f64::NAN), None).unwrap();
    let t = lux(&c);
    assert_eq!(t.report.unavailable_hours[5], 1.0);
    assert_eq!(t.lux_hours[0][5], 0.0);
    assert_eq!(t.hours_above[0][5], 0.0);

    // The file's illuminance stands without the radiation: an isotropic sky.
    let mut i = interval(sun, f64::NAN, f64::NAN);
    i.file_lux = [f64::NAN, 0.0, 30_000.0];
    let mut f = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    f.add(&i, None).unwrap();
    let t = lux(&f);
    assert_eq!(t.report.file_hours[5], 1.0);
    assert!((t.lux_hours[0][5] - 30_000.0).abs() < 1.0);
}

#[test]
fn night_adds_nothing_and_is_not_counted() {
    let mut c = climate(vec![[0.0, 0.0, 1.0]], 1.0, LuxSource::FileWhenValid);
    c.add(&interval(at(-10.0, 0.0), 0.0, 0.0), None).unwrap();
    let t = lux(&c);
    assert_eq!(t.lux_hours[0][5], 0.0);
    assert_eq!(
        t.report.file_hours[5] + t.report.efficacy_hours[5] + t.report.unavailable_hours[5],
        0.0
    );
}

/// The threshold is decided per interval from illuminance, never from sun
/// hours: a bright overcast hour counts on a sample the sun never reaches,
/// and a dim low-sun hour does not count on a sample the sun reaches.
#[test]
fn the_threshold_is_crossed_interval_by_interval_not_by_sun_hours() {
    let up = [0.0, 0.0, 1.0];
    let mut c = climate(vec![up, up], 20_000.0, LuxSource::FileWhenValid);
    // Bright overcast: about 30 klx of diffuse, no sun.
    c.add(&interval(at(45.0, 180.0), 0.0, 260.0), None).unwrap();
    // A low clear sun: lit, but a few klx.
    c.add(
        &interval(at(5.0, 250.0), 300.0, 30.0),
        Some(&[BLOCKED, OPEN]),
    )
    .unwrap();
    let t = lux(&c);
    // Sample 0 is shaded from the low sun, sample 1 is lit by it; both count
    // the overcast hour only.
    assert_eq!(t.hours_above[0][5], 1.0);
    assert_eq!(t.hours_above[1][5], 1.0);
    assert!(t.lux_hours[1][5] > t.lux_hours[0][5]);
}

/// Enough samples to split across threads: every sample gets the same.
#[test]
fn many_samples_split_across_threads_agree() {
    let up = [0.0, 0.0, 1.0];
    let n = 5000;
    let mut c = climate(vec![up; n], 50_000.0, LuxSource::FileWhenValid);
    let paths = vec![OPEN; n];
    c.add(&interval(at(55.0, 170.0), 780.0, 95.0), Some(&paths))
        .unwrap();
    let t = lux(&c);
    assert!(t.lux_hours.iter().all(|m| m[5] == t.lux_hours[0][5]));
    assert!(t.hours_above.iter().all(|m| m[5] == 1.0));
}
