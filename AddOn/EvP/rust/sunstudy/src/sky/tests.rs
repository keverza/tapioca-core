#![expect(
    clippy::unwrap_used,
    clippy::indexing_slicing,
    reason = "test assertions"
)]

use super::dome::dot;
use super::perez::{PerezInput, PerezRefusal};
use super::*;
use std::f64::consts::{PI, TAU};

fn domes() -> [Dome; 2] {
    [Dome::new(PatchSet::Tregenza), Dome::new(PatchSet::Reinhart)]
}

/// Unit vector at altitude and azimuth (degrees, clockwise from north).
fn at(altitude: f64, azimuth: f64) -> [f64; 3] {
    let (a, p) = (altitude.to_radians(), azimuth.to_radians());
    [p.sin() * a.cos(), p.cos() * a.cos(), a.sin()]
}

/// Horizontal facing `azimuth`.
fn facing(azimuth: f64) -> [f64; 3] {
    at(0.0, azimuth)
}

#[test]
fn patch_sets_have_their_counts_and_close_the_hemisphere() {
    for dome in domes() {
        assert_eq!(dome.len(), dome.set().count());
        let omega: f64 = dome.patches().iter().map(|p| p.solid_angle).sum();
        let projected: f64 = dome.patches().iter().map(|p| p.moment[2]).sum();
        assert!((omega - TAU).abs() < 1e-12, "{omega}");
        assert!((projected - PI).abs() < 1e-12, "{projected}");
        for p in dome.patches() {
            assert!(p.solid_angle > 0.0);
            let n = dot(p.direction, p.direction);
            assert!((n - 1.0).abs() < 1e-12);
            // The centroid lies inside the patch.
            let altitude = p.direction[2].asin();
            assert!(altitude >= p.altitude[0] && altitude <= p.altitude[1]);
        }
    }
}

#[test]
fn the_first_patch_is_centred_on_north_and_the_cap_is_last() {
    for dome in domes() {
        let first = dome.patches()[0].direction;
        assert!(first[0].abs() < 1e-12 && first[1] > 0.0);
        // The second patch is east of north: clockwise.
        assert!(dome.patches()[1].direction[0] > 0.0);
        let cap = dome.patches().last().unwrap();
        assert_eq!(cap.altitude[1], std::f64::consts::FRAC_PI_2);
    }
}

#[test]
fn domes_are_reproducible_bit_for_bit() {
    for set in [PatchSet::Tregenza, PatchSet::Reinhart] {
        let a = Dome::new(set);
        let b = Dome::new(set);
        let bits = |d: &Dome| -> Vec<u64> {
            d.patches()
                .iter()
                .flat_map(|p| {
                    p.moment
                        .iter()
                        .chain(&[p.solid_angle])
                        .map(|v| v.to_bits())
                        .collect::<Vec<_>>()
                })
                .collect()
        };
        assert_eq!(bits(&a), bits(&b));
    }
}

#[test]
fn an_open_horizontal_plane_sees_all_the_sky_and_a_vertical_one_half() {
    for dome in domes() {
        let open = vec![true; dome.len()];
        let up = dome.view_factor([0.0, 0.0, 1.0], &open);
        assert!((up - 1.0).abs() < 1e-12, "{up}");
        for azimuth in [0.0, 7.5, 45.0, 90.0, 180.0, 222.0] {
            let v = dome.view_factor(facing(azimuth), &open);
            assert!((v - 0.5).abs() < 2e-4, "{:?} {azimuth}: {v}", dome.set());
        }
    }
}

#[test]
fn a_horizontal_plane_below_a_uniform_horizon_sees_cos_squared() {
    // Sky blocked below altitude α: the view factor is cos² α, exact when α
    // falls on a row edge (24° for Tregenza, 30° for both).
    for (dome, alpha) in [
        (Dome::new(PatchSet::Tregenza), 24.0_f64),
        (Dome::new(PatchSet::Tregenza), 36.0),
        (Dome::new(PatchSet::Reinhart), 30.0),
    ] {
        let edge = alpha.to_radians();
        let open: Vec<bool> = dome
            .patches()
            .iter()
            .map(|p| p.altitude[0] >= edge - 1e-12)
            .collect();
        let v = dome.view_factor([0.0, 0.0, 1.0], &open);
        let expected = edge.cos().powi(2);
        assert!((v - expected).abs() < 1e-12, "{alpha}: {v} vs {expected}");
    }
}

#[test]
fn a_missing_flag_counts_as_obstructed() {
    let dome = Dome::new(PatchSet::Tregenza);
    assert_eq!(dome.view_factor([0.0, 0.0, 1.0], &[]), 0.0);
}

/// Vertical sky component: the facing half's projected luminance over the
/// unobstructed horizontal's, for a relative distribution.
fn vsc(dome: &Dome, relative: &[f64], normal: [f64; 3]) -> f64 {
    let field = normalise(dome, relative, 1.0).unwrap();
    let projected = dome.projected(normal);
    projected
        .iter()
        .zip(&field.values)
        .map(|(w, l)| w * l)
        .sum()
}

#[test]
fn the_traditional_overcast_gives_the_vertical_sky_component_of_39_6_percent() {
    // Analytic: (π/6 + 4/9) / (7π/9) = 0.39617.
    let exact = (PI / 6.0 + 4.0 / 9.0) / (7.0 * PI / 9.0);
    for dome in domes() {
        let relative =
            cie_relative(&dome, SkyModel::CieTraditionalOvercast, [0.0, 0.0, 1.0]).unwrap();
        for azimuth in [0.0, 90.0, 200.0] {
            let v = vsc(&dome, &relative, facing(azimuth));
            assert!(
                (v - exact).abs() < 0.0025,
                "{:?} {azimuth}: {v} vs {exact}",
                dome.set()
            );
        }
    }
}

#[test]
fn cie_type_1_is_a_third_as_bright_at_the_horizon_and_type_12_is_brightest_near_the_sun() {
    let sun = at(40.0, 180.0);
    let horizon = cie::relative(1, at(0.0, 0.0), sun).unwrap();
    assert!((horizon - 1.0 / (1.0 + 4.0 * (-0.7_f64).exp())).abs() < 1e-12);
    assert_eq!(cie::relative(1, [0.0, 0.0, 1.0], sun).unwrap(), 1.0);
    let near = cie::relative(12, at(42.0, 180.0), sun).unwrap();
    let away = cie::relative(12, at(42.0, 0.0), sun).unwrap();
    assert!(near > 5.0 * away, "{near} {away}");
    assert_eq!(cie::relative(0, sun, sun), None);
    assert_eq!(cie::relative(16, sun, sun), None);
    assert_eq!(cie::relative(1, at(-5.0, 0.0), sun), None);
}

fn input(altitude: f64, dni: f64, dhi: f64) -> PerezInput {
    PerezInput {
        sun: at(altitude, 160.0),
        dni,
        dhi,
        day_of_year: 172,
        dew_point_c: None,
    }
}

/// Clear, intermediate and overcast, and a low winter sun.
fn conditions() -> [PerezInput; 5] {
    [
        input(55.0, 820.0, 95.0),
        input(35.0, 420.0, 210.0),
        input(30.0, 0.0, 160.0),
        input(10.0, 350.0, 60.0),
        input(65.0, 150.0, 300.0),
    ]
}

#[test]
fn the_perez_field_conserves_dhi_and_reports_its_clamp() {
    for dome in domes() {
        for c in conditions() {
            let sky = perez_sky(&dome, &c).unwrap();
            let h: f64 = dome
                .patches()
                .iter()
                .zip(&sky.field.values)
                .map(|(p, l)| p.moment[2] * l)
                .sum();
            assert!((h - c.dhi).abs() < 1e-9 * c.dhi, "{h} vs {}", c.dhi);
            assert!(sky.field.values.iter().all(|&l| l >= 0.0));
            assert!((0.0..1.0).contains(&sky.field.clamped_share));
        }
    }
}

#[test]
fn perez_bins_follow_clearness() {
    let overcast = perez::parameters(&input(30.0, 0.0, 160.0)).unwrap();
    assert_eq!(overcast.bin, 0);
    assert!((overcast.clearness - 1.0).abs() < 1e-12);
    let clear = perez::parameters(&input(55.0, 820.0, 95.0)).unwrap();
    assert_eq!(clear.bin, 7);
    let (diffuse, direct) = perez::efficacy(&clear);
    assert!(
        (90.0..160.0).contains(&diffuse) && (80.0..120.0).contains(&direct),
        "{diffuse} {direct}"
    );
}

#[test]
fn perez_refuses_a_set_sun_and_unusable_irradiance_and_reports_holds() {
    assert_eq!(
        perez::parameters(&input(-1.0, 0.0, 10.0)),
        Err(PerezRefusal::SunDown)
    );
    assert_eq!(
        perez::parameters(&input(20.0, 100.0, 0.0)),
        Err(PerezRefusal::Irradiance)
    );
    assert_eq!(
        perez::parameters(&input(20.0, f64::NAN, 50.0)),
        Err(PerezRefusal::Irradiance)
    );
    let mut bad_day = input(20.0, 100.0, 50.0);
    bad_day.day_of_year = 0;
    assert_eq!(perez::parameters(&bad_day), Err(PerezRefusal::Day));
    let high = perez::parameters(&input(89.0, 900.0, 50.0)).unwrap();
    assert!(high.held.altitude);
    let dim = perez::parameters(&input(20.0, 0.0, 2.0)).unwrap();
    assert!(dim.held.brightness);
}

#[test]
fn the_transposition_returns_dhi_on_a_horizontal_plane() {
    for c in conditions() {
        let e = perez::transposed_diffuse(&c, [0.0, 0.0, 1.0]).unwrap();
        assert!((e - c.dhi).abs() < 1e-9 * c.dhi, "{e} vs {}", c.dhi);
    }
}

#[test]
fn the_patch_integrated_sky_differs_from_the_1990_transposition_by_a_measured_bound() {
    // Two different Perez models on one input: the 1993 luminance
    // distribution integrated over the dome, and the 1990 irradiance
    // transposition, whose circumsolar term is a point at the sun. They do
    // not agree to 1 %: measured on these cases, up to 9 % of DHI with the
    // sun at 30° or higher, and up to 62 % with a 10° sun facing the plane.
    // The bounds below are those measurements rounded up; a regression in
    // either model moves them.
    for dome in domes() {
        for c in conditions() {
            let sky = perez_sky(&dome, &c).unwrap();
            let low = c.sun[2] < 25.0_f64.to_radians().sin();
            let bound = if low { 0.65 } else { 0.10 };
            for azimuth in [0.0, 90.0, 160.0, 250.0] {
                let n = facing(azimuth);
                let integrated: f64 = dome
                    .projected(n)
                    .iter()
                    .zip(&sky.field.values)
                    .map(|(w, l)| w * l)
                    .sum();
                let analytic = perez::transposed_diffuse(&c, n).unwrap();
                let rel = (integrated - analytic).abs() / c.dhi;
                assert!(
                    rel < bound,
                    "{:?} az {azimuth}: {integrated} vs {analytic}",
                    dome.set()
                );
            }
        }
    }
}

/// Sun altitude, DNI, DHI, element altitude and azimuth, ε, Δ, `L/L_ref`.
type Case = (f64, f64, f64, f64, f64, f64, f64, f64);

#[test]
fn perez_luminance_matches_an_independent_reading_of_the_paper() {
    // (sun altitude, DNI, DHI, element altitude, element azimuth, clearness,
    // brightness, L/L_ref), day 172, sun azimuth 160°. The expected values
    // come from a separate implementation of the published equations and
    // tables (Radiance's gendaylit, as ported in Python), run outside this crate.
    let cases: [Case; 15] = [
        (
            55.0,
            820.0,
            95.0,
            6.0,
            160.0,
            7.976_174,
            0.087_596,
            1.428_850_590_262_312_5,
        ),
        (
            55.0,
            820.0,
            95.0,
            30.0,
            200.0,
            7.976_174,
            0.087_596,
            0.745_744_790_722_577_9,
        ),
        (
            55.0,
            820.0,
            95.0,
            80.0,
            0.0,
            7.976_174,
            0.087_596,
            0.364_797_894_140_793_67,
        ),
        (
            35.0,
            420.0,
            210.0,
            6.0,
            160.0,
            2.041_226,
            0.276_108,
            3.523_652_277_632_171_6,
        ),
        (
            35.0,
            420.0,
            210.0,
            45.0,
            160.0,
            2.041_226,
            0.276_108,
            5.247_362_844_343_98,
        ),
        (
            35.0,
            420.0,
            210.0,
            80.0,
            0.0,
            2.041_226,
            0.276_108,
            0.707_725_861_680_812_3,
        ),
        (
            30.0,
            0.0,
            160.0,
            6.0,
            340.0,
            1.0,
            0.241_092,
            1.180_074_420_540_991,
        ),
        (
            30.0,
            0.0,
            160.0,
            30.0,
            200.0,
            1.0,
            0.241_092,
            2.187_757_538_635_485,
        ),
        (
            30.0,
            0.0,
            160.0,
            80.0,
            0.0,
            1.0,
            0.241_092,
            2.039_444_215_582_296_5,
        ),
        (
            10.0,
            350.0,
            60.0,
            6.0,
            160.0,
            2.521_596,
            0.253_173,
            10.026_664_390_273_925,
        ),
        (
            10.0,
            350.0,
            60.0,
            6.0,
            340.0,
            2.521_596,
            0.253_173,
            1.028_922_931_695_201_8,
        ),
        (
            10.0,
            350.0,
            60.0,
            45.0,
            160.0,
            2.521_596,
            0.253_173,
            1.319_610_789_564_254_4,
        ),
        (
            65.0,
            150.0,
            300.0,
            6.0,
            160.0,
            1.460_203,
            0.250_089,
            1.340_585_834_906_081_5,
        ),
        (
            65.0,
            150.0,
            300.0,
            30.0,
            200.0,
            1.460_203,
            0.250_089,
            1.548_199_938_140_962_1,
        ),
        (
            65.0,
            150.0,
            300.0,
            80.0,
            0.0,
            1.460_203,
            0.250_089,
            1.437_375_636_012_316_4,
        ),
    ];
    for (sun_alt, dni, dhi, alt, az, eps, delta, expected) in cases {
        let i = input(sun_alt, dni, dhi);
        let p = perez::parameters(&i).unwrap();
        assert!(
            (p.clearness - eps).abs() < 1e-6,
            "{sun_alt}: ε {}",
            p.clearness
        );
        assert!(
            (p.brightness - delta).abs() < 1e-6,
            "{sun_alt}: Δ {}",
            p.brightness
        );
        let (c, _) = perez::coefficients(&p);
        let v = perez::relative(&c, at(alt, az), i.sun);
        assert!(
            (v - expected).abs() < 1e-9 * expected,
            "{sun_alt} {alt} {az}: {v} vs {expected}"
        );
    }
}
