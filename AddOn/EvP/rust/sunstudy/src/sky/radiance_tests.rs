//! The Perez sky against Radiance's `gendaymtx` on the same patches (#269,
//! #268): the acceptance comparison that replaces the 1990 transposition.
//!
//! Opt-in: set `TAPIOCA_RADIANCE_DIR` to a folder of pairs `<name>.wea` and
//! `<name>.mtx`, each matrix made from its weather file with
//!
//! ```text
//! gendaymtx -m 1 -d -O1 -h <name>.wea > <name>.mtx     (Tregenza, 146 rows)
//! gendaymtx -m 2 -d -O1 -h <name>.wea > <name>.mtx     (Reinhart, 578 rows)
//! ```
//!
//! `-d` leaves the sun out (sky only), `-O1` gives solar radiance in W/m²/sr,
//! `-h` drops the header. Row 0 is the ground; rows 1.. are the sky patches in
//! the order of [`super::dome`], which is Radiance's (first patch centred on
//! north, clockwise, rows upward, cap last). Each row holds one RGB triple per
//! time step.
//!
//! The sun for each step comes from [`crate::solar::position`] at the `.wea`
//! hour (standard time at the file's meridian); Radiance computes its own, so
//! a small angular difference is part of what is measured.
//!
//! The bounds are provisional until the first fixtures are run: they are
//! printed with every case, and set from the measured worst case then.

#![expect(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::panic,
    reason = "test assertions"
)]

use super::perez::PerezInput;
use super::*;
use crate::solar::{Civil, instant, position};

/// One `.wea` file: place and steps.
#[derive(Debug, PartialEq)]
struct Wea {
    /// Degrees north.
    latitude: f64,
    /// Degrees east (the file writes west positive).
    longitude: f64,
    /// Standard-time offset, hours ahead of UTC (the file writes the
    /// meridian in degrees, west positive).
    offset_hours: f64,
    /// (month, day, decimal hour, DNI, DHI).
    steps: Vec<(u32, u32, f64, f64, f64)>,
}

fn parse_wea(text: &str) -> Option<Wea> {
    let mut wea = Wea {
        latitude: f64::NAN,
        longitude: f64::NAN,
        offset_hours: f64::NAN,
        steps: Vec::new(),
    };
    for line in text.lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        match fields.as_slice() {
            ["latitude", v] => wea.latitude = v.parse().ok()?,
            ["longitude", v] => wea.longitude = -v.parse::<f64>().ok()?,
            ["time_zone", v] => wea.offset_hours = -v.parse::<f64>().ok()? / 15.0,
            [m, d, h, dni, dhi] if m.parse::<u32>().is_ok() => wea.steps.push((
                m.parse().ok()?,
                d.parse().ok()?,
                h.parse().ok()?,
                dni.parse().ok()?,
                dhi.parse().ok()?,
            )),
            _ => {}
        }
    }
    (wea.latitude.is_finite() && wea.longitude.is_finite() && wea.offset_hours.is_finite())
        .then_some(wea)
}

/// Rows of per-step radiance (the mean of each RGB triple).
fn parse_mtx(text: &str) -> Vec<Vec<f64>> {
    text.lines()
        .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
        .map(|l| {
            let v: Vec<f64> = l
                .split_whitespace()
                .filter_map(|t| t.parse().ok())
                .collect();
            v.chunks(3).map(|c| c.iter().sum::<f64>() / 3.0).collect()
        })
        .collect()
}

fn day_of_year(month: u32, day: u32) -> u32 {
    const BEFORE: [u32; 12] = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
    BEFORE[(month - 1) as usize] + day
}

/// Irradiance on a plane from a field: Σ projected·L.
fn on_plane(dome: &Dome, field: &[f64], normal: [f64; 3]) -> f64 {
    dome.projected(normal)
        .iter()
        .zip(field)
        .map(|(w, l)| w * l)
        .sum()
}

#[test]
fn the_perez_sky_matches_gendaymtx_when_fixtures_are_provided() {
    let Ok(dir) = std::env::var("TAPIOCA_RADIANCE_DIR") else {
        return;
    };
    let mut cases = 0;
    for entry in std::fs::read_dir(&dir).expect("TAPIOCA_RADIANCE_DIR is readable") {
        let path = entry.unwrap().path();
        if path.extension().and_then(|e| e.to_str()) != Some("wea") {
            continue;
        }
        let wea = parse_wea(&std::fs::read_to_string(&path).unwrap()).expect("a place header");
        let rows = parse_mtx(
            &std::fs::read_to_string(path.with_extension("mtx")).expect("a matching .mtx"),
        );
        let set = match rows.len() {
            146 => PatchSet::Tregenza,
            578 => PatchSet::Reinhart,
            n => panic!("{}: {n} rows is neither -m 1 nor -m 2", path.display()),
        };
        let dome = Dome::new(set);
        for (k, &(month, day, hour, dni, dhi)) in wea.steps.iter().enumerate() {
            let at = instant(
                Civil {
                    year: 2026,
                    month,
                    day,
                    seconds: hour * 3600.0,
                },
                wea.offset_hours,
            );
            let sun = position(at, wea.latitude, wea.longitude);
            let input = PerezInput {
                sun: sun.direction(),
                dni,
                dhi,
                day_of_year: day_of_year(month, day),
                dew_point_c: None,
            };
            let Ok(ours) = perez_sky(&dome, &input) else {
                continue; // night, or no diffuse: nothing to compare
            };
            let theirs: Vec<f64> = rows[1..].iter().map(|r| r[k]).collect();
            if theirs.iter().all(|&v| v == 0.0) {
                continue;
            }
            cases += 1;
            let mut worst_patch: f64 = 0.0;
            for (a, b) in ours.field.values.iter().zip(&theirs) {
                worst_patch = worst_patch.max((a - b).abs() / (dhi / std::f64::consts::PI));
            }
            let mut worst_plane: f64 = 0.0;
            for normal in [
                [0.0, 0.0, 1.0],
                [0.0, 1.0, 0.0],
                [1.0, 0.0, 0.0],
                [0.0, -1.0, 0.0],
                [-1.0, 0.0, 0.0],
            ] {
                let a = on_plane(&dome, &ours.field.values, normal);
                let b = on_plane(&dome, &theirs, normal);
                worst_plane = worst_plane.max((a - b).abs() / dhi);
            }
            eprintln!(
                "{} step {k} ({month}/{day} {hour:.2} h, sun {:.1}°): planes within {:.2} % of DHI, patches within {:.2} % of isotropic",
                path.display(),
                sun.altitude,
                100.0 * worst_plane,
                100.0 * worst_patch,
            );
            assert!(
                worst_plane < 0.05,
                "a plane differs by more than 5 % of DHI"
            );
        }
    }
    assert!(
        cases > 0,
        "TAPIOCA_RADIANCE_DIR held no daylight step to compare"
    );
}

#[test]
fn the_wea_and_matrix_readers_follow_radiance_s_layout() {
    let wea = parse_wea(
        "place Vilnius\nlatitude 54.63\nlongitude -25.28\ntime_zone -30\nsite_elevation 112\n\
         weather_data_file_units 1\n6 21 12.500 812 96\n6 21 13.500 0 120\n",
    )
    .unwrap();
    assert!((wea.longitude - 25.28).abs() < 1e-12);
    assert!((wea.offset_hours - 2.0).abs() < 1e-12);
    assert_eq!(
        wea.steps,
        vec![(6, 21, 12.5, 812.0, 96.0), (6, 21, 13.5, 0.0, 120.0)]
    );
    assert!(parse_wea("6 21 12.5 1 1\n").is_none());
    let rows = parse_mtx("0 0 0\t1 1 1\n\n2 2 2\t3 4 5\n");
    assert_eq!(rows, vec![vec![0.0, 1.0], vec![2.0, 4.0]]);
}
