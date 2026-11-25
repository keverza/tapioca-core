//! `tapioca_sunstudy_weather_read`, `_weather_season` and `_station_distance`:
//! EPW weather files (#155 P1).
//!
//! A refused file is a result, not a call failure: the call succeeds and the
//! summary names the refusal and its line, so the caller can show where the
//! file is wrong. Records, days and months are written only for a parsed file.

use super::Regions;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::weather::season::{self, SeasonRule};
use tapioca_sunstudy::weather::{self, FIELD_COUNT, RefusalKind, WeatherFile};

/// The file's bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WeatherReadRequest {
    /// The whole file, as read from disk.
    pub bytes: SliceView<u8>,
}

/// One record; `values` follows the Rust `Field` order, NaN when missing.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WeatherRecordOut {
    /// Source year.
    pub year: i32,
    /// 1–12.
    pub month: u8,
    /// 1–31.
    pub day: u8,
    /// 1–24, the hour the interval ends in.
    pub hour: u8,
    /// As written.
    pub minute: u8,
    /// Field values.
    pub values: [f64; FIELD_COUNT],
}

/// One day of the period.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WeatherDayOut {
    /// 1–12.
    pub month: u8,
    /// 1–31.
    pub day: u8,
    /// Padding.
    pub reserved: [u8; 6],
    /// Mean valid dry bulb, °C; NaN when none.
    pub mean_dry_bulb_c: f64,
    /// Global horizontal energy, Wh/m², valid intervals.
    pub global_horizontal_wh_m2: f64,
    /// Intervals without a valid global horizontal value.
    pub global_missing: usize,
}

/// One calendar month.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WeatherMonthOut {
    /// Records in the month.
    pub records: usize,
    /// Records without a valid global horizontal value.
    pub global_missing: usize,
    /// °C; NaN when none.
    pub mean_dry_bulb_c: f64,
    /// °C; NaN when none.
    pub min_dry_bulb_c: f64,
    /// °C; NaN when none.
    pub max_dry_bulb_c: f64,
    /// kWh/m².
    pub global_horizontal_kwh_m2: f64,
    /// kWh/m².
    pub diffuse_horizontal_kwh_m2: f64,
    /// mm.
    pub precipitation_mm: f64,
}

/// Header, refusal and quality; written by every successful call.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WeatherSummary {
    /// 0 when parsed; otherwise the `RefusalKind` order + 1.
    pub refusal: u32,
    /// Records per hour.
    pub records_per_hour: u32,
    /// 1-based line at fault, 0 when not about a line.
    pub line: usize,
    /// 0-based column at fault.
    pub column: usize,
    /// Expected count (columns or records).
    pub expected: usize,
    /// Found count (columns or records).
    pub found: usize,
    /// Records written.
    pub records: usize,
    /// Days written.
    pub days: usize,
    /// Degrees, north positive.
    pub latitude: f64,
    /// Degrees, east positive.
    pub longitude: f64,
    /// Hours from UTC of the file's standard time.
    pub time_zone_hours: f64,
    /// Metres.
    pub elevation_m: f64,
    /// Records with radiation while the extraterrestrial radiation is zero.
    pub radiation_in_dark: usize,
    /// Missing per field.
    pub missing: [usize; FIELD_COUNT],
    /// Out of range per field.
    pub out_of_range: [usize; FIELD_COUNT],
    /// SHA-256 of the bytes.
    pub sha256: [u8; 32],
    /// 1 when the period includes 29 February.
    pub leap_year: u8,
    /// Month, day.
    pub period_start: [u8; 2],
    /// Month, day.
    pub period_end: [u8; 2],
    /// Padding.
    pub reserved: [u8; 3],
    /// NUL-terminated UTF-8, cut on a character boundary.
    pub city: [u8; 64],
    /// As above.
    pub region: [u8; 32],
    /// As above.
    pub country: [u8; 32],
    /// As above.
    pub source: [u8; 64],
    /// As above.
    pub wmo: [u8; 16],
    /// As above.
    pub comments1: [u8; 256],
    /// As above.
    pub comments2: [u8; 256],
}

impl Default for WeatherSummary {
    fn default() -> Self {
        Self {
            refusal: 0,
            records_per_hour: 0,
            line: 0,
            column: 0,
            expected: 0,
            found: 0,
            records: 0,
            days: 0,
            latitude: 0.0,
            longitude: 0.0,
            time_zone_hours: 0.0,
            elevation_m: 0.0,
            radiation_in_dark: 0,
            missing: [0; FIELD_COUNT],
            out_of_range: [0; FIELD_COUNT],
            sha256: [0; 32],
            leap_year: 0,
            period_start: [0; 2],
            period_end: [0; 2],
            reserved: [0; 3],
            city: [0; 64],
            region: [0; 32],
            country: [0; 32],
            source: [0; 64],
            wmo: [0; 16],
            comments1: [0; 256],
            comments2: [0; 256],
        }
    }
}

/// Caller-owned outputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WeatherReadOutput {
    /// Records, `record_capacity` long.
    pub records: *mut WeatherRecordOut,
    /// Capacity in records; a parsed file with more is a call failure.
    pub record_capacity: usize,
    /// Days, `day_capacity` long (366 covers any period).
    pub days: *mut WeatherDayOut,
    /// Capacity in days.
    pub day_capacity: usize,
    /// Exactly 12 months, January first.
    pub months: *mut WeatherMonthOut,
    /// Required summary.
    pub summary: *mut WeatherSummary,
}

fn text<const N: usize>(value: &str) -> [u8; N] {
    let mut out = [0_u8; N];
    let mut end = value.len().min(N.saturating_sub(1));
    while !value.is_char_boundary(end) {
        end -= 1;
    }
    if let (Some(dst), Some(src)) = (out.get_mut(..end), value.as_bytes().get(..end)) {
        dst.copy_from_slice(src);
    }
    out
}

fn refusal_code(kind: RefusalKind) -> u32 {
    1 + match kind {
        RefusalKind::Empty => 0,
        RefusalKind::Header => 1,
        RefusalKind::Location => 2,
        RefusalKind::DataPeriod => 3,
        RefusalKind::SeveralPeriods => 4,
        RefusalKind::RecordsPerHour => 5,
        RefusalKind::Columns => 6,
        RefusalKind::Number => 7,
        RefusalKind::Count => 8,
        RefusalKind::Sequence => 9,
    }
}

fn summary_of(file: &WeatherFile, days: usize) -> WeatherSummary {
    let h = &file.header;
    let [c1, c2] = &h.comments;
    WeatherSummary {
        records_per_hour: h.records_per_hour,
        records: file.records.len(),
        days,
        latitude: h.latitude,
        longitude: h.longitude,
        time_zone_hours: h.time_zone_hours,
        elevation_m: h.elevation_m,
        radiation_in_dark: file.quality.radiation_in_dark,
        missing: file.quality.missing,
        out_of_range: file.quality.out_of_range,
        sha256: file.sha256,
        leap_year: u8::from(h.leap_year),
        period_start: [h.period_start.0, h.period_start.1],
        period_end: [h.period_end.0, h.period_end.1],
        city: text(&h.city),
        region: text(&h.region),
        country: text(&h.country),
        source: text(&h.source),
        wmo: text(&h.wmo),
        comments1: text(c1),
        comments2: text(c2),
        ..WeatherSummary::default()
    }
}

/// Parse an EPW file.
///
/// # Safety
/// The request, its bytes and the output descriptor must be live, initialized
/// and immutable. The record and day arrays at their capacities, 12 months and
/// the summary must be live, aligned, exclusively writable and disjoint from
/// every other region and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_weather_read(
    request: *const WeatherReadRequest,
    output: *const WeatherReadOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            regions.input(request.bytes.data, request.bytes.count);
            regions.output(target.records, target.record_capacity);
            regions.output(target.days, target.day_capacity);
            regions.output(target.months, 12);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let bytes = context.slice(request.bytes)?;
            let file = match weather::parse(bytes) {
                Ok(file) => file,
                Err(refusal) => {
                    target.summary.write(WeatherSummary {
                        refusal: refusal_code(refusal.kind),
                        line: refusal.line,
                        column: refusal.column,
                        expected: refusal.expected,
                        found: refusal.found,
                        ..WeatherSummary::default()
                    });
                    return Ok(());
                }
            };
            let days = season::daily(&file);
            if file.records.len() > target.record_capacity || days.len() > target.day_capacity {
                return Err(Failure::Argument("WeatherOutputTooSmall"));
            }
            let records: Vec<WeatherRecordOut> = file
                .records
                .iter()
                .map(|r| WeatherRecordOut {
                    year: r.year,
                    month: r.month,
                    day: r.day,
                    hour: r.hour,
                    minute: r.minute,
                    values: r.values,
                })
                .collect();
            let day_out: Vec<WeatherDayOut> = days
                .iter()
                .map(|d| WeatherDayOut {
                    month: d.month,
                    day: d.day,
                    mean_dry_bulb_c: d.mean_dry_bulb_c,
                    global_horizontal_wh_m2: d.global_horizontal_wh_m2,
                    global_missing: d.global_missing,
                    ..WeatherDayOut::default()
                })
                .collect();
            let months = season::monthly(&file).map(|m| WeatherMonthOut {
                records: m.records,
                global_missing: m.global_missing,
                mean_dry_bulb_c: m.mean_dry_bulb_c,
                min_dry_bulb_c: m.min_dry_bulb_c,
                max_dry_bulb_c: m.max_dry_bulb_c,
                global_horizontal_kwh_m2: m.global_horizontal_kwh_m2,
                diffuse_horizontal_kwh_m2: m.diffuse_horizontal_kwh_m2,
                precipitation_mm: m.precipitation_mm,
            });
            // SAFETY: lengths <= checked disjoint capacities.
            std::ptr::copy_nonoverlapping(records.as_ptr(), target.records, records.len());
            // SAFETY: as above.
            std::ptr::copy_nonoverlapping(day_out.as_ptr(), target.days, day_out.len());
            // SAFETY: 12 months registered above.
            std::ptr::copy_nonoverlapping(months.as_ptr(), target.months, 12);
            target.summary.write(summary_of(&file, day_out.len()));
            Ok(())
        })
    }
}

/// Growing-season inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WeatherSeasonRequest {
    /// Daily mean dry bulb, °C, NaN when missing.
    pub daily_mean_c: SliceView<f64>,
    /// Running-mean window, days (odd).
    pub window_days: usize,
    /// In season at or above, °C.
    pub threshold_c: f64,
    /// 1 when the days form a closed year (a season may cross New Year).
    pub wraps: u8,
    /// Padding.
    pub reserved: [u8; 7],
}

/// The longest warm run.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WeatherSeasonResult {
    /// 1 when a run exists.
    pub found: u8,
    /// Padding.
    pub reserved: [u8; 7],
    /// First day index.
    pub first: usize,
    /// Last day index, inclusive; before `first` when it wraps.
    pub last: usize,
}

/// Find the growing season in daily means.
///
/// # Safety
/// The request and its array must be live and immutable; `result` must be
/// live, aligned, exclusively writable and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_weather_season(
    request: *const WeatherSeasonRequest,
    result: *mut WeatherSeasonResult,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(result, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.daily_mean_c.data, request.daily_mean_c.count);
            regions.check()?;
            if request.window_days == 0
                || request.window_days % 2 == 0
                || !request.threshold_c.is_finite()
            {
                return Err(Failure::Argument("WeatherSeasonRule"));
            }
            let context = regions.context();
            let means = context.slice(request.daily_mean_c)?;
            let rule = SeasonRule {
                window_days: request.window_days,
                threshold_c: request.threshold_c,
                wraps: request.wraps != 0,
            };
            let found = season::growing_season(means, rule);
            result.write(
                found.map_or_else(WeatherSeasonResult::default, |(first, last)| {
                    WeatherSeasonResult {
                        found: 1,
                        first,
                        last,
                        ..WeatherSeasonResult::default()
                    }
                }),
            );
            Ok(())
        })
    }
}

/// Two points, degrees.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StationDistanceRequest {
    /// Latitude, longitude of the first point.
    pub a: [f64; 2],
    /// Latitude, longitude of the second point.
    pub b: [f64; 2],
}

/// Great-circle distance, metres.
///
/// # Safety
/// `request` must be live and immutable; `metres` live, aligned, exclusively
/// writable and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_station_distance(
    request: *const StationDistanceRequest,
    metres: *mut f64,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(metres, 1);
            regions.check()?;
            let r = regions.context().record(request)?;
            let [la, oa] = r.a;
            let [lb, ob] = r.b;
            if ![la, oa, lb, ob].iter().all(|v| v.is_finite()) || la.abs() > 90.0 || lb.abs() > 90.0
            {
                return Err(Failure::Argument("StationDistanceCoordinates"));
            }
            metres.write(weather::distance_m(la, oa, lb, ob));
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
