//! `tapioca_sunstudy_weather_instants`, `_sun_cross_check` and
//! `_climate_dli`: the weather time base (#155 P2) and the daily light
//! integral of a climate run (#278 Stage 5).
//!
//! The independent sun written here is a cross-check for the host's, never an
//! analysis input; the caller labels any result traced with it as such.

use super::Regions;
use super::climate::ClimateHandle;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::climate::{ParConversion, daily_light_integral};
use tapioca_sunstudy::solar::{self, RecordTime};

/// An EPW record's stamp, as written.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WeatherStamp {
    /// 1–12.
    pub month: u8,
    /// 1–31.
    pub day: u8,
    /// 1–24, the hour the interval ends in.
    pub hour: u8,
    /// 0–60.
    pub minute: u8,
}

/// Where the weather sits and who looks at it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WeatherInstantRequest {
    /// One per record.
    pub stamps: SliceView<WeatherStamp>,
    /// The calendar year the weather is placed in.
    pub year: i32,
    /// Records per hour, 1–60.
    pub records_per_hour: u32,
    /// The station's standard time, hours ahead of UTC (EPW header).
    pub station_offset_hours: f64,
    /// The project's standard time, hours ahead of UTC (no daylight saving).
    pub project_offset_hours: f64,
    /// Project latitude, degrees north.
    pub latitude: f64,
    /// Project longitude, degrees east.
    pub longitude: f64,
    /// Model frame turn, radians (as `tapioca_sunstudy_sky_patches`).
    pub rotation: f64,
}

/// One record's interval on the project's clock.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WeatherInstant {
    /// UTC Julian day of the interval's midpoint.
    pub julian_day: f64,
    /// Interval length, hours.
    pub hours: f64,
    /// Seconds after midnight on the project's standard-time clock.
    pub project_seconds: f64,
    /// Project-clock date of the midpoint.
    pub project_year: i32,
    /// 1–12.
    pub project_month: u8,
    /// 1–31.
    pub project_day: u8,
    /// Zero.
    pub reserved: [u8; 2],
    /// Day of the year of the record's own date, 1–366.
    pub day_of_year: u32,
    /// 1 when the stamp was read; 0 refused (the rest is zero).
    pub valid: u8,
    /// Zero.
    pub reserved2: [u8; 3],
    /// The independent (NOAA) sun at the midpoint, unit, model frame. A
    /// cross-check only.
    pub independent_sun: [f64; 3],
}

/// Place every record on the project's clock and compute the independent sun.
/// A stamp out of range gives `valid` 0; the call still succeeds.
///
/// # Safety
/// `request` and its stamps readable; `out` writable for `capacity` records,
/// disjoint from everything else.
#[unsafe(no_mangle)]
#[expect(clippy::many_single_char_names, reason = "rotation sin/cos and xyz")]
pub unsafe extern "C" fn tapioca_sunstudy_weather_instants(
    request: *const WeatherInstantRequest,
    out: *mut WeatherInstant,
    capacity: usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, capacity);
            regions.check()?;
            let r = regions.context().record(request)?;
            regions.input(r.stamps.data, r.stamps.count);
            regions.check()?;
            let context = regions.context();
            let stamps = context.slice(r.stamps)?;
            if capacity < stamps.len() {
                return Err(Failure::Argument(
                    "WeatherInstants: capacity below the record count",
                ));
            }
            let place = [r.project_offset_hours, r.latitude, r.longitude, r.rotation];
            if place.iter().any(|v| !v.is_finite()) {
                return Err(Failure::Argument("WeatherInstants: place not finite"));
            }
            let (s, c) = r.rotation.sin_cos();
            for (k, stamp) in stamps.iter().enumerate() {
                let record = RecordTime {
                    year: r.year,
                    month: u32::from(stamp.month),
                    day: u32::from(stamp.day),
                    hour: u32::from(stamp.hour),
                    minute: u32::from(stamp.minute),
                    records_per_hour: r.records_per_hour,
                    station_offset_hours: r.station_offset_hours,
                };
                let instant = solar::interval_midpoint(&record).map_or_else(
                    WeatherInstant::default,
                    |(at, hours)| {
                        let clock = solar::civil(at, r.project_offset_hours);
                        let [x, y, z] = solar::position(at, r.latitude, r.longitude).direction();
                        WeatherInstant {
                            julian_day: at.julian_day,
                            hours,
                            project_seconds: clock.seconds,
                            project_year: clock.year,
                            project_month: u8::try_from(clock.month).unwrap_or(0),
                            project_day: u8::try_from(clock.day).unwrap_or(0),
                            reserved: [0; 2],
                            day_of_year: solar::day_of_year(r.year, record.month, record.day),
                            valid: 1,
                            reserved2: [0; 3],
                            independent_sun: [c * x - s * y, s * x + c * y, z],
                        }
                    },
                );
                out.add(k).write(instant);
            }
            Ok(())
        })
    }
}

/// How far the host's sun is from the independent one.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SunCrossCheckRecord {
    /// Instants compared (both suns up).
    pub compared: usize,
    /// Mean angle, degrees.
    pub mean_degrees: f64,
    /// Largest angle, degrees.
    pub max_degrees: f64,
    /// 1 within #155 P2's 0.5° mean / 2° max.
    pub accepted: u8,
    /// Zero.
    pub reserved: [u8; 7],
}

/// Compare unit directions pairwise, three doubles each, same frame.
///
/// # Safety
/// The views readable; `out` one writable record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_sun_cross_check(
    host: SliceView<f64>,
    independent: SliceView<f64>,
    out: *mut SunCrossCheckRecord,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(host.data, host.count);
            regions.input(independent.data, independent.count);
            regions.output(out, 1);
            regions.check()?;
            let context = regions.context();
            let (a, b) = (context.slice(host)?, context.slice(independent)?);
            if a.len() != b.len() || !a.len().is_multiple_of(3) {
                return Err(Failure::Argument("SunCrossCheck: two equal lists of xyz"));
            }
            let check = solar::cross_check(a.as_chunks::<3>().0, b.as_chunks::<3>().0);
            out.write(SunCrossCheckRecord {
                compared: check.compared,
                mean_degrees: check.mean_degrees,
                max_degrees: check.max_degrees,
                accepted: u8::from(check.accepted()),
                reserved: [0; 7],
            });
            Ok(())
        })
    }
}

/// Mean daily light integral per sample and month, mol/m²/day, from the run's
/// totals over each month's valid hours: `capacity` × 12 values, NaN for a
/// month without valid hours. `par_fraction` and `micromol_per_joule` 0 take
/// the defaults (0.45, 4.57).
///
/// # Safety
/// `handle` live; `out` writable for `capacity` × 12 doubles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_climate_dli(
    handle: *const ClimateHandle,
    par_fraction: f64,
    micromol_per_joule: f64,
    out: *mut f64,
    capacity: usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            let values = capacity
                .checked_mul(12)
                .ok_or(Failure::Argument("ClimateDli: capacity"))?;
            regions.output(out, values);
            regions.check()?;
            memory_range(handle, 1)?;
            let climate = handle
                .as_ref()
                .ok_or(Failure::Argument("ClimateHandle: null"))?
                .climate();
            if capacity < climate.sample_count() {
                return Err(Failure::Argument(
                    "ClimateDli: capacity below the sample count",
                ));
            }
            let defaults = ParConversion::default();
            let pick = |v: f64, d: f64| {
                if v == 0.0 {
                    Ok(d)
                } else if v.is_finite() && v > 0.0 {
                    Ok(v)
                } else {
                    Err(Failure::Argument("ClimateDli: conversion"))
                }
            };
            let conversion = ParConversion {
                par_fraction: pick(par_fraction, defaults.par_fraction)?,
                micromol_per_joule: pick(micromol_per_joule, defaults.micromol_per_joule)?,
            };
            let totals = climate.totals();
            for s in 0..climate.sample_count() {
                for m in 0..12 {
                    let hours = totals.report.valid_hours.get(m).copied().unwrap_or(0.0);
                    let dli = daily_light_integral(totals.total(s, m), hours, conversion)
                        .unwrap_or(f64::NAN);
                    out.add(s * 12 + m).write(dli);
                }
            }
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
