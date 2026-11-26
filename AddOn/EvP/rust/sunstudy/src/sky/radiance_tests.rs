//! The Perez sky against Radiance's `gendaymtx` on the same patches (#269,
//! #268): the acceptance comparison that replaces the 1990 transposition.
//!
//! Opt-in: set `TAPIOCA_RADIANCE_DIR` to a folder of pairs `<name>.wea` and
//! `<name>.mtx`, each matrix made from its weather file with
//!
//! ```text
//! gendaymtx -m 1 -s -O1 -h <name>.wea > <name>.mtx     (Tregenza, 146 rows)
//! gendaymtx -m 2 -s -O1 -h <name>.wea > <name>.mtx     (Reinhart, 578 rows)
//! ```
//!
//! `-s` leaves the sun out (sky only; `-d` is the sun ALONE, which lights a
//! handful of patches and is not this comparison), `-O1` gives solar radiance in W/m²/sr,
//! `-h` drops the header. Row 0 is the ground; rows 1.. are the sky patches in
//! the order of [`super::dome`], which is Radiance's (first patch centred on
//! north, clockwise, rows upward, cap last). `gendaymtx` writes each row as
//! one RGB triple per time step, a triple a line, and a blank line after it.
//!
//! The sun for each step comes from [`crate::solar::position`] at the `.wea`
//! hour (standard time at the file's meridian); Radiance computes its own, so
//! a small angular difference is part of what is measured.
//!
//! The bounds were set from the first fixtures (2026-11-26: Perugia `IGDG`,
//! Kaunas `IWEC`, Vilnius `TMYx`, both domes, `.wea` from Radiance's `epw2wea`):
//!
//! - over a year, every plane within 2.20 % of Σ DHI (bound 3 %). Ours is the
//!   lower throughout: `gendaymtx`'s horizontal integrates to about 1.022 ×
//!   DHI on its patches, where ours conserves DHI exactly by construction;
//! - per step with the sun at 30° or higher, the worst plane within 5.8 % of
//!   that step's DHI (bound 7 %);
//! - per step, the 95th percentile of a file's worst plane within 4.43 %
//!   (bound 6 %); below 10° of sun alone it reaches 13.3 %;
//! - steps beyond 30 % of DHI: 33 of about 25 000, all overcast with little
//!   DNI and the sun at the horizon or its Perez brightness held, where the
//!   two codes spread the sky differently while their horizontal totals still
//!   agree within 2 % (bound: 0.5 % of a file's steps). Reported, not hidden.

#![expect(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::panic,
    reason = "test assertions"
)]

use std::fmt::Write as _;

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

/// Rows of per-step radiance (the mean of each RGB triple). `gendaymtx`
/// writes one triple per line and ends each row with a blank line; a matrix
/// with a whole row per line (no blank lines) reads as well.
fn parse_mtx(text: &str) -> Vec<Vec<f64>> {
    let triples = |line: &str| -> Vec<f64> {
        let v: Vec<f64> = line
            .split_whitespace()
            .filter_map(|t| t.parse().ok())
            .collect();
        v.chunks(3).map(|c| c.iter().sum::<f64>() / 3.0).collect()
    };
    let lines: Vec<&str> = text.lines().filter(|l| !l.starts_with('#')).collect();
    if !lines.iter().any(|l| l.trim().is_empty()) {
        return lines.iter().map(|l| triples(l)).collect();
    }
    let mut rows = Vec::new();
    let mut row = Vec::new();
    for line in lines {
        if line.trim().is_empty() {
            if !row.is_empty() {
                rows.push(std::mem::take(&mut row));
            }
        } else {
            row.extend(triples(line));
        }
    }
    if !row.is_empty() {
        rows.push(row);
    }
    rows
}

fn day_of_year(month: u32, day: u32) -> u32 {
    const BEFORE: [u32; 12] = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
    BEFORE[(month - 1) as usize] + day
}

/// The planes compared: horizontal, then vertical facing +y, +x, −y, −x.
const PLANES: [[f64; 3]; 5] = [
    [0.0, 0.0, 1.0],
    [0.0, 1.0, 0.0],
    [1.0, 0.0, 0.0],
    [0.0, -1.0, 0.0],
    [-1.0, 0.0, 0.0],
];
const PLANE_NAMES: [&str; 5] = ["up", "+y", "+x", "-y", "-x"];

/// A plane's year, as a share of the year's Σ DHI (measured 2.20 %).
const YEAR_BOUND: f64 = 0.03;
/// One step's worst plane with the sun at 30° or higher, as a share of DHI
/// (measured 5.8 %).
const HIGH_SUN_STEP_BOUND: f64 = 0.07;
/// The 95th percentile of a file's per-step worst plane, any sun (measured
/// 4.43 %).
const STEP_P95_BOUND: f64 = 0.06;
/// The share of a file's steps allowed beyond 30 % of DHI, as a fraction
/// (numerator, denominator): 0.5 % (measured 0.32 % at most, 0.13 % over all
/// files).
const OUTLIER_PER_MILLE_BOUND: (usize, usize) = (5, 1000);

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
    let mut worst_year: f64 = 0.0;
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
        // Per plane: Σ ours, Σ theirs (W·h/m² over the steps), per-step |Δ|/DHI.
        let mut sums = [[0.0_f64; 2]; PLANES.len()];
        let mut dhi_sum = 0.0;
        let mut steps: Vec<(f64, f64)> = Vec::new(); // (altitude, worst plane share)
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
            dhi_sum += dhi;
            let mut worst_plane: f64 = 0.0;
            for (sum, normal) in sums.iter_mut().zip(PLANES) {
                let a = on_plane(&dome, &ours.field.values, normal);
                let b = on_plane(&dome, &theirs, normal);
                sum[0] += a;
                sum[1] += b;
                worst_plane = worst_plane.max((a - b).abs() / dhi);
            }
            steps.push((sun.altitude, worst_plane));
            if sun.altitude >= 30.0 {
                assert!(
                    worst_plane < HIGH_SUN_STEP_BOUND,
                    "{}: {month}/{day} {hour} h, sun {:.1}°: a plane {:.2} % of DHI apart",
                    path.display(),
                    sun.altitude,
                    100.0 * worst_plane
                );
            }
        }
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        let mut line = format!(
            "{name}: {} steps; year, (ours − gendaymtx) / ΣDHI:",
            steps.len()
        );
        for (sum, label) in sums.iter().zip(PLANE_NAMES) {
            let share = (sum[0] - sum[1]) / dhi_sum;
            write!(line, " {label} {:+.2} %", 100.0 * share).unwrap();
            worst_year = worst_year.max(share.abs());
        }
        eprintln!("{line}");
        let mut all: Vec<f64> = steps.iter().map(|(_, w)| *w).collect();
        all.sort_by(f64::total_cmp);
        let p95 = all[(all.len() - 1) * 95 / 100];
        let outliers = all.iter().filter(|w| **w > 0.3).count();
        eprintln!(
            "  all steps: p95 {:.2} % of DHI; {outliers} step(s) beyond 30 %",
            100.0 * p95
        );
        assert!(
            p95 < STEP_P95_BOUND,
            "{name}: p95 {:.2} % of DHI",
            100.0 * p95
        );
        assert!(
            outliers * OUTLIER_PER_MILLE_BOUND.1 < OUTLIER_PER_MILLE_BOUND.0 * all.len(),
            "{name}: {outliers} steps beyond 30 % of DHI"
        );
        for (lo, hi) in [(0.0, 10.0), (10.0, 30.0), (30.0, 90.0)] {
            let mut band: Vec<f64> = steps
                .iter()
                .filter(|(alt, _)| *alt >= lo && *alt < hi)
                .map(|(_, w)| *w)
                .collect();
            if band.is_empty() {
                continue;
            }
            band.sort_by(f64::total_cmp);
            let at = |percent: usize| band[(band.len() - 1) * percent / 100];
            eprintln!(
                "  sun {lo:>2.0}-{hi:<2.0}°: {} steps, worst plane per step: median {:.2} %, p95 {:.2} %, max {:.2} % of DHI",
                band.len(),
                100.0 * at(50),
                100.0 * at(95),
                100.0 * at(100),
            );
        }
    }
    assert!(
        cases > 0,
        "TAPIOCA_RADIANCE_DIR held no daylight step to compare"
    );
    eprintln!(
        "worst plane over a year: {:.2} % of ΣDHI",
        100.0 * worst_year
    );
    assert!(
        worst_year < YEAR_BOUND,
        "a plane's year is {:.2} % of ΣDHI apart",
        100.0 * worst_year
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
    // gendaymtx: a triple a line, a blank line after each row.
    let rows = parse_mtx("0 0 0\n1 1 1\n\n2 2 2\n3 4 5\n\n");
    assert_eq!(rows, vec![vec![0.0, 1.0], vec![2.0, 4.0]]);
    // A whole row a line, with or without blank lines between rows.
    assert_eq!(parse_mtx("0 0 0\t1 1 1\n\n2 2 2\t3 4 5\n"), rows);
    assert_eq!(parse_mtx("0 0 0\t1 1 1\n2 2 2\t3 4 5\n"), rows);
}
