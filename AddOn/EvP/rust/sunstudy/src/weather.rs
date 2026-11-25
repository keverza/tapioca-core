//! `EnergyPlus` weather (EPW) files (#155 P1): parse, validate, checksum.
//!
//! Takes the file's bytes, never a path: file IO stays in the C++ feature
//! layer. Header, one data period of 1–60 records per hour, 27 numeric fields
//! per record, coverage and missing counts, and a SHA-256 of the bytes.
//!
//! ⚠️ A RECORD COVERS THE INTERVAL BEFORE ITS TIMESTAMP. Hour 1 is 00:00–01:00
//! local standard time at the station, never daylight saving time. Radiation,
//! illuminance and precipitation describe that interval; temperature, wind
//! and the other state fields are read at its end ([`FieldSpec::interval`]).
//!
//! ⚠️ RADIATION IS A MEAN RATE, NOT THE INTERVAL'S ENERGY. Each radiation
//! value is Wh/m² per hour over its interval, i.e. mean W/m². An hourly
//! record's value equals its energy; a 15-minute record's energy is a quarter
//! of its value. Use [`WeatherFile::interval_hours`], never a bare sum of
//! records, so Wh/m² and W/m² are not mixed.
//!
//! ⚠️ MISSING IS NOT ZERO. A missing marker or an out-of-range value becomes
//! NaN and is counted per field in [`WeatherQuality`]; nothing is filled in.
//! Radiation reported while the extraterrestrial horizontal radiation is zero
//! (the sun is down) is counted, not removed.
//!
//! A malformed file is refused whole with the line and column at fault
//! ([`Refusal`]); a partly-read weather year is never returned.

pub mod calendar;
pub mod fields;
pub mod season;
pub mod sha256;

pub use fields::{FIELD_COUNT, FIELDS, Field, FieldSpec, Reading};

/// What the header says about the station and the data period.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WeatherHeader {
    /// `LOCATION` field 1.
    pub city: String,
    /// `LOCATION` field 2 (state, province or region).
    pub region: String,
    /// `LOCATION` field 3.
    pub country: String,
    /// `LOCATION` field 4: the data source (e.g. `IWEC Data`, `SRC-TMYx`).
    pub source: String,
    /// `LOCATION` field 5: the WMO station number, as written.
    pub wmo: String,
    /// Degrees, north positive.
    pub latitude: f64,
    /// Degrees, east positive.
    pub longitude: f64,
    /// Hours from UTC of the file's local standard time.
    pub time_zone_hours: f64,
    /// Station elevation, metres.
    pub elevation_m: f64,
    /// The header's leap-year flag, or a 29 February record.
    pub leap_year: bool,
    /// Records per hour, 1–60, dividing 60.
    pub records_per_hour: u32,
    /// First date of the data period (month, day).
    pub period_start: (u8, u8),
    /// Last date of the data period (month, day).
    pub period_end: (u8, u8),
    /// `COMMENTS 1` and `COMMENTS 2`, outer quotes removed.
    pub comments: [String; 2],
}

/// One record. `values` follows [`Field`] order; NaN is missing or invalid.
#[derive(Clone, Debug, PartialEq)]
pub struct WeatherRecord {
    /// The source year of this record (a TMY mixes years by month).
    pub year: i32,
    /// 1–12.
    pub month: u8,
    /// 1–31.
    pub day: u8,
    /// 1–24: the hour whose interval this record ends in.
    pub hour: u8,
    /// The minute field, as written (0 or 60 for hourly files).
    pub minute: u8,
    /// Field values in [`Field`] order.
    pub values: [f64; FIELD_COUNT],
}

impl WeatherRecord {
    /// The value of `field`; NaN when missing or invalid.
    #[must_use]
    pub fn get(&self, field: Field) -> f64 {
        self.values.get(field as usize).copied().unwrap_or(f64::NAN)
    }
}

/// Coverage and missing-data counts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WeatherQuality {
    /// Records carrying the field's missing marker (or an absent column).
    pub missing: [usize; FIELD_COUNT],
    /// Records whose value lay outside the documented range.
    pub out_of_range: [usize; FIELD_COUNT],
    /// Records with global, direct or diffuse radiation above zero while the
    /// extraterrestrial horizontal radiation is zero.
    pub radiation_in_dark: usize,
}

impl Default for WeatherQuality {
    fn default() -> Self {
        Self {
            missing: [0; FIELD_COUNT],
            out_of_range: [0; FIELD_COUNT],
            radiation_in_dark: 0,
        }
    }
}

/// A parsed weather file.
#[derive(Clone, Debug, PartialEq)]
pub struct WeatherFile {
    /// Station and period.
    pub header: WeatherHeader,
    /// Records in file order: day by day, interval by interval.
    pub records: Vec<WeatherRecord>,
    /// Missing and invalid counts.
    pub quality: WeatherQuality,
    /// SHA-256 of the file's bytes.
    pub sha256: [u8; 32],
}

impl WeatherFile {
    /// Seconds per record.
    #[must_use]
    pub fn interval_seconds(&self) -> u32 {
        3600 / self.header.records_per_hour.max(1)
    }

    /// Hours per record: multiply a radiation value by this for its energy.
    #[must_use]
    pub fn interval_hours(&self) -> f64 {
        1.0 / f64::from(self.header.records_per_hour.max(1))
    }
}

/// Why a file was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RefusalKind {
    /// No bytes, or fewer than the eight header lines.
    Empty,
    /// A required header line is absent or out of place (`line` says which).
    Header,
    /// `LOCATION` has too few fields or an unreadable number.
    Location,
    /// `DATA PERIODS` is unreadable, or its dates are invalid.
    DataPeriod,
    /// More than one data period.
    SeveralPeriods,
    /// Records per hour outside 1–60 or not dividing 60.
    RecordsPerHour,
    /// A data line has too few columns.
    Columns,
    /// A date, time or field is not a number.
    Number,
    /// The record count does not match the data period.
    Count,
    /// A record's date or hour is not the one expected at its place.
    Sequence,
}

/// A refusal with the place at fault. `line` is 1-based (0 when not about a
/// line), `column` 0-based; `expected`/`found` are counts for `Columns` and
/// `Count`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Refusal {
    /// What was wrong.
    pub kind: RefusalKind,
    /// 1-based line, 0 when not about a line.
    pub line: usize,
    /// 0-based column within the line.
    pub column: usize,
    /// Expected count, where one applies.
    pub expected: usize,
    /// Found count, where one applies.
    pub found: usize,
}

const fn refuse(kind: RefusalKind, line: usize) -> Refusal {
    Refusal {
        kind,
        line,
        column: 0,
        expected: 0,
        found: 0,
    }
}

const HEADER_LINES: usize = 8;

fn unquote(text: &str) -> String {
    let t = text.trim();
    t.strip_prefix('"')
        .and_then(|s| s.strip_suffix('"'))
        .unwrap_or(t)
        .to_owned()
}

fn number(text: Option<&str>, kind: RefusalKind, line: usize) -> Result<f64, Refusal> {
    text.and_then(|t| t.trim().parse::<f64>().ok())
        .filter(|v| v.is_finite())
        .ok_or(refuse(kind, line))
}

fn parse_location(text: &str, header: &mut WeatherHeader) -> Result<(), Refusal> {
    let cols: Vec<&str> = text.split(',').collect();
    let col = |i: usize| cols.get(i).copied();
    let name = |i: usize| col(i).map_or_else(String::new, |s| s.trim().to_owned());
    header.city = name(1);
    header.region = name(2);
    header.country = name(3);
    header.source = name(4);
    header.wmo = name(5);
    header.latitude = number(col(6), RefusalKind::Location, 1)?;
    header.longitude = number(col(7), RefusalKind::Location, 1)?;
    header.time_zone_hours = number(col(8), RefusalKind::Location, 1)?;
    header.elevation_m = number(col(9), RefusalKind::Location, 1)?;
    let plausible = header.latitude.abs() <= 90.0
        && header.longitude.abs() <= 180.0
        && header.time_zone_hours.abs() <= 14.0;
    if plausible {
        Ok(())
    } else {
        Err(refuse(RefusalKind::Location, 1))
    }
}

fn parse_periods(text: &str, header: &mut WeatherHeader) -> Result<(), Refusal> {
    let cols: Vec<&str> = text.split(',').map(str::trim).collect();
    let periods = cols
        .get(1)
        .and_then(|t| t.parse::<u32>().ok())
        .ok_or(refuse(RefusalKind::DataPeriod, HEADER_LINES))?;
    if periods != 1 {
        return Err(refuse(RefusalKind::SeveralPeriods, HEADER_LINES));
    }
    let per_hour = cols
        .get(2)
        .and_then(|t| t.parse::<u32>().ok())
        .ok_or(refuse(RefusalKind::DataPeriod, HEADER_LINES))?;
    if !(1..=60).contains(&per_hour) || 60 % per_hour != 0 {
        return Err(refuse(RefusalKind::RecordsPerHour, HEADER_LINES));
    }
    header.records_per_hour = per_hour;
    let date = |i: usize| cols.get(i).and_then(|t| calendar::parse_month_day(t));
    header.period_start = date(5).ok_or(refuse(RefusalKind::DataPeriod, HEADER_LINES))?;
    header.period_end = date(6).ok_or(refuse(RefusalKind::DataPeriod, HEADER_LINES))?;
    Ok(())
}

fn parse_header(lines: &[&str]) -> Result<WeatherHeader, Refusal> {
    const KEYS: [&str; HEADER_LINES] = [
        "LOCATION",
        "DESIGN CONDITIONS",
        "TYPICAL/EXTREME PERIODS",
        "GROUND TEMPERATURES",
        "HOLIDAYS/DAYLIGHT SAVINGS",
        "COMMENTS 1",
        "COMMENTS 2",
        "DATA PERIODS",
    ];
    for (index, key) in KEYS.iter().enumerate() {
        let line = lines.get(index).copied().unwrap_or("");
        if !line.to_ascii_uppercase().starts_with(key) {
            return Err(refuse(RefusalKind::Header, index + 1));
        }
    }
    let at = |i: usize| lines.get(i).copied().unwrap_or("");
    let mut header = WeatherHeader::default();
    parse_location(at(0), &mut header)?;
    header.leap_year = at(4)
        .split(',')
        .nth(1)
        .is_some_and(|flag| flag.trim().eq_ignore_ascii_case("yes"));
    for (slot, line) in header.comments.iter_mut().zip([at(5), at(6)]) {
        *slot = line
            .split_once(',')
            .map_or_else(String::new, |(_, rest)| unquote(rest));
    }
    parse_periods(at(7), &mut header)?;
    Ok(header)
}

fn small(text: Option<&str>, line: usize, column: usize) -> Result<i64, Refusal> {
    text.and_then(|t| t.trim().parse::<f64>().ok())
        .filter(|v| v.is_finite() && v.fract() == 0.0 && v.abs() < 1.0e6)
        .map(|v| {
            #[expect(clippy::cast_possible_truncation, reason = "integral and below 1e6")]
            let whole = v as i64;
            whole
        })
        .ok_or(Refusal {
            column,
            ..refuse(RefusalKind::Number, line)
        })
}

fn byte(value: i64, line: usize, column: usize) -> Result<u8, Refusal> {
    u8::try_from(value).map_err(|_| Refusal {
        column,
        ..refuse(RefusalKind::Number, line)
    })
}

fn parse_record(
    text: &str,
    line: usize,
    quality: &mut WeatherQuality,
) -> Result<WeatherRecord, Refusal> {
    let cols: Vec<&str> = text.split(',').collect();
    if cols.len() < fields::MIN_COLUMNS {
        return Err(Refusal {
            expected: fields::MIN_COLUMNS,
            found: cols.len(),
            ..refuse(RefusalKind::Columns, line)
        });
    }
    let col = |i: usize| cols.get(i).copied();
    let year =
        i32::try_from(small(col(0), line, 0)?).map_err(|_| refuse(RefusalKind::Number, line))?;
    let mut record = WeatherRecord {
        year,
        month: byte(small(col(1), line, 1)?, line, 1)?,
        day: byte(small(col(2), line, 2)?, line, 2)?,
        hour: byte(small(col(3), line, 3)?, line, 3)?,
        minute: byte(small(col(4), line, 4)?, line, 4)?,
        values: [f64::NAN; FIELD_COUNT],
    };
    for (slot, spec) in record.values.iter_mut().zip(FIELDS.iter()) {
        let index = spec.field as usize;
        let reading = match col(spec.column) {
            None => Reading::Missing,
            Some(raw) => {
                let raw = raw.trim().parse::<f64>().map_err(|_| Refusal {
                    column: spec.column,
                    ..refuse(RefusalKind::Number, line)
                })?;
                spec.read(raw)
            }
        };
        match reading {
            Reading::Value(v) => *slot = v,
            Reading::Missing => {
                if let Some(count) = quality.missing.get_mut(index) {
                    *count += 1;
                }
            }
            Reading::OutOfRange => {
                if let Some(count) = quality.out_of_range.get_mut(index) {
                    *count += 1;
                }
            }
        }
    }
    let dark = record.get(Field::ExtraterrestrialHorizontal) == 0.0;
    let lit = [
        Field::GlobalHorizontal,
        Field::DirectNormal,
        Field::DiffuseHorizontal,
    ]
    .iter()
    .any(|f| record.get(*f) > 0.0);
    if dark && lit {
        quality.radiation_in_dark += 1;
    }
    Ok(record)
}

fn check_sequence(header: &WeatherHeader, records: &[WeatherRecord]) -> Result<(), Refusal> {
    let days = calendar::period_days(header.period_start, header.period_end, header.leap_year);
    if days.is_empty() {
        return Err(refuse(RefusalKind::DataPeriod, HEADER_LINES));
    }
    let per_day = 24 * usize::try_from(header.records_per_hour).unwrap_or(1);
    let expected = days.len() * per_day;
    if records.len() != expected {
        return Err(Refusal {
            expected,
            found: records.len(),
            ..refuse(RefusalKind::Count, 0)
        });
    }
    let per_hour = per_day / 24;
    for (index, record) in records.iter().enumerate() {
        let (month, day) = days.get(index / per_day).copied().unwrap_or((0, 0));
        let hour = (index % per_day) / per_hour + 1;
        if record.month != month
            || record.day != day
            || usize::from(record.hour) != hour
            || record.minute > 60
        {
            return Err(refuse(RefusalKind::Sequence, HEADER_LINES + index + 1));
        }
    }
    Ok(())
}

/// Parse and validate an EPW file.
///
/// # Errors
/// [`Refusal`] for a missing or malformed header, an unsupported data period,
/// an unreadable record, or records that do not fill the data period in order.
pub fn parse(bytes: &[u8]) -> Result<WeatherFile, Refusal> {
    let text = String::from_utf8_lossy(bytes);
    let mut lines: Vec<&str> = text
        .split('\n')
        .map(|l| l.strip_suffix('\r').unwrap_or(l))
        .collect();
    while lines.last().is_some_and(|l| l.trim().is_empty()) {
        lines.pop();
    }
    if lines.len() < HEADER_LINES {
        return Err(refuse(RefusalKind::Empty, 0));
    }
    let (head, body) = lines.split_at(HEADER_LINES);
    let mut header = parse_header(head)?;
    let mut quality = WeatherQuality::default();
    let mut records = Vec::with_capacity(body.len());
    for (index, line) in body.iter().enumerate() {
        records.push(parse_record(line, HEADER_LINES + index + 1, &mut quality)?);
    }
    header.leap_year |= records.iter().any(|r| r.month == 2 && r.day == 29);
    check_sequence(&header, &records)?;
    Ok(WeatherFile {
        header,
        records,
        quality,
        sha256: sha256::sha256(bytes),
    })
}

/// Great-circle distance between two points, metres (mean Earth radius).
#[must_use]
pub fn distance_m(lat_a: f64, lon_a: f64, lat_b: f64, lon_b: f64) -> f64 {
    const EARTH_RADIUS_M: f64 = 6_371_008.8;
    let (pa, pb) = (lat_a.to_radians(), lat_b.to_radians());
    let dp = pb - pa;
    let dl = (lon_b - lon_a).to_radians();
    let h = (dp / 2.0).sin().powi(2) + pa.cos() * pb.cos() * (dl / 2.0).sin().powi(2);
    2.0 * EARTH_RADIUS_M * h.sqrt().min(1.0).asin()
}

#[cfg(test)]
mod tests;
