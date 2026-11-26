//! The weather time base (#155 P2): where an EPW interval sits in time, and an
//! independent sun position to check the host's against.
//!
//! ⚠️ THE HOST'S SUN IS THE AUTHORITY. [`position`] exists only to cross-check
//! the directions Archicad returns ([`cross_check`]); it never feeds an
//! analysis. It is the NOAA solar calculator's algorithm (after Meeus,
//! *Astronomical Algorithms*), geometric, without refraction, good to about
//! 0.01° in declination and a few seconds in the equation of time for
//! 1950–2050.
//!
//! ⚠️ EPW CLOCKS ARE STANDARD TIME AT THE STATION. A record stamped hour h
//! covers (h − 1, h] in the station's standard time; daylight saving never
//! applies. [`interval_midpoint`] turns a record into a UTC instant;
//! [`civil`] turns that into the project's standard-time clock for the host,
//! which must be called with daylight saving off.

use std::f64::consts::PI;

/// Julian day of 2000-01-01 12:00 UTC (J2000.0).
const J2000: f64 = 2_451_545.0;

/// A UTC instant as a Julian day (days since −4712-01-01 12:00 UTC).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Instant {
    /// Julian day, UTC.
    pub julian_day: f64,
}

/// A civil date and time on some clock.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Civil {
    /// Gregorian year.
    pub year: i32,
    /// 1–12.
    pub month: u32,
    /// 1–31.
    pub day: u32,
    /// Seconds since that day's midnight, 0 ≤ s < 86 400.
    pub seconds: f64,
}

/// Julian day of 00:00 UTC on a Gregorian date (Meeus ch. 7).
#[must_use]
pub fn julian_day(year: i32, month: u32, day: u32) -> f64 {
    let (y, m) = if month <= 2 {
        (year - 1, month + 12)
    } else {
        (year, month)
    };
    let a = (f64::from(y) / 100.0).floor();
    let b = 2.0 - a + (a / 4.0).floor();
    (365.25 * (f64::from(y) + 4716.0)).floor()
        + (30.6001 * f64::from(m + 1)).floor()
        + f64::from(day)
        + b
        - 1524.5
}

/// Day of the year, 1–366, of a Gregorian date.
#[must_use]
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "a difference of Julian days within one year is 0-365"
)]
pub fn day_of_year(year: i32, month: u32, day: u32) -> u32 {
    (julian_day(year, month, day) - julian_day(year, 1, 1)).round() as u32 + 1
}

/// The instant at `civil` on a clock `offset_hours` ahead of UTC.
#[must_use]
pub fn instant(civil: Civil, offset_hours: f64) -> Instant {
    Instant {
        julian_day: julian_day(civil.year, civil.month, civil.day)
            + (civil.seconds - offset_hours * 3600.0) / 86_400.0,
    }
}

/// The civil date and time of `at` on a clock `offset_hours` ahead of UTC
/// (Meeus ch. 7, Gregorian dates only).
#[must_use]
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "calendar fields of a Gregorian date are small positive integers"
)]
#[expect(clippy::many_single_char_names, reason = "Meeus's notation")]
pub fn civil(at: Instant, offset_hours: f64) -> Civil {
    let jd = at.julian_day + offset_hours / 24.0 + 0.5;
    let z = jd.floor();
    let f = jd - z;
    let alpha = ((z - 1_867_216.25) / 36_524.25).floor();
    let a = z + 1.0 + alpha - (alpha / 4.0).floor();
    let b = a + 1524.0;
    let c = ((b - 122.1) / 365.25).floor();
    let d = (365.25 * c).floor();
    let e = ((b - d) / 30.6001).floor();
    let day = b - d - (30.6001 * e).floor();
    let month = if e < 14.0 { e - 1.0 } else { e - 13.0 };
    let year = if month > 2.0 { c - 4716.0 } else { c - 4715.0 };
    Civil {
        year: year as i32,
        month: month as u32,
        day: day as u32,
        seconds: (f * 86_400.0).clamp(0.0, 86_399.999_999),
    }
}

/// An EPW record's position in time.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RecordTime {
    /// The calendar year the weather is placed in.
    pub year: i32,
    /// 1–12.
    pub month: u32,
    /// 1–31.
    pub day: u32,
    /// EPW hour field, 1–24: the hour the interval ends in.
    pub hour: u32,
    /// EPW minute field, 0–60: the interval's end within the hour; 0 or 60
    /// means the end of the hour (both are written in practice).
    pub minute: u32,
    /// Records per hour, 1–60.
    pub records_per_hour: u32,
    /// The station's standard time, hours ahead of UTC (EPW header).
    pub station_offset_hours: f64,
}

/// The UTC instant at the middle of the record's interval, and the interval's
/// length in hours. `None` for fields out of range.
#[must_use]
pub fn interval_midpoint(r: &RecordTime) -> Option<(Instant, f64)> {
    let ok = (1..=12).contains(&r.month)
        && (1..=31).contains(&r.day)
        && (1..=24).contains(&r.hour)
        && r.minute <= 60
        && (1..=60).contains(&r.records_per_hour)
        && 60 % r.records_per_hour == 0
        && r.station_offset_hours.is_finite();
    if !ok {
        return None;
    }
    let length_hours = 1.0 / f64::from(r.records_per_hour);
    // The end of the interval, in minutes after the station's midnight.
    let minute = if r.minute == 0 || r.records_per_hour == 1 {
        60
    } else {
        r.minute
    };
    let end_minutes = f64::from(r.hour - 1) * 60.0 + f64::from(minute);
    let mid_minutes = end_minutes - 30.0 * length_hours;
    let midnight = instant(
        Civil {
            year: r.year,
            month: r.month,
            day: r.day,
            seconds: 0.0,
        },
        r.station_offset_hours,
    );
    Some((
        Instant {
            julian_day: midnight.julian_day + mid_minutes / 1440.0,
        },
        length_hours,
    ))
}

/// The sun's position seen from a place.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SunPosition {
    /// Geometric altitude above the horizon, degrees (no refraction).
    pub altitude: f64,
    /// Azimuth clockwise from true north, degrees, 0–360.
    pub azimuth: f64,
    /// Declination, degrees.
    pub declination: f64,
    /// Equation of time, minutes.
    pub equation_of_time: f64,
}

impl SunPosition {
    /// Unit vector to the sun in the sky frame (x east, y north, z up).
    #[must_use]
    pub fn direction(&self) -> [f64; 3] {
        let (a, p) = (self.altitude.to_radians(), self.azimuth.to_radians());
        [p.sin() * a.cos(), p.cos() * a.cos(), a.sin()]
    }
}

/// The sun at `at` from latitude and longitude (degrees, north and east
/// positive), by the NOAA algorithm. A cross-check only.
#[must_use]
pub fn position(at: Instant, latitude: f64, longitude: f64) -> SunPosition {
    let t = (at.julian_day - J2000) / 36_525.0;
    let l0 = (280.466_46 + t * (36_000.769_83 + 0.000_303_2 * t)).rem_euclid(360.0);
    let m = 357.529_11 + t * (35_999.050_29 - 0.000_153_7 * t);
    let e = 0.016_708_634 - t * (0.000_042_037 + 0.000_000_126_7 * t);
    let mr = m.to_radians();
    let centre = mr.sin() * (1.914_602 - t * (0.004_817 + 0.000_014 * t))
        + (2.0 * mr).sin() * (0.019_993 - 0.000_101 * t)
        + (3.0 * mr).sin() * 0.000_289;
    let omega = (125.04 - 1934.136 * t).to_radians();
    let lambda = (l0 + centre - 0.005_69 - 0.004_78 * omega.sin()).to_radians();
    let seconds = 21.448 - t * (46.815 + t * (0.000_59 - t * 0.001_813));
    let obliquity = (23.0 + (26.0 + seconds / 60.0) / 60.0 + 0.002_56 * omega.cos()).to_radians();
    let declination = (obliquity.sin() * lambda.sin()).asin();
    let y = (obliquity / 2.0).tan().powi(2);
    let l0r = l0.to_radians();
    let eot = 4.0
        * (y * (2.0 * l0r).sin() - 2.0 * e * mr.sin() + 4.0 * e * y * mr.sin() * (2.0 * l0r).cos()
            - 0.5 * y * y * (4.0 * l0r).sin()
            - 1.25 * e * e * (2.0 * mr).sin())
        .to_degrees();
    let utc_minutes = (at.julian_day + 0.5).rem_euclid(1.0) * 1440.0;
    let solar_minutes = (utc_minutes + eot + 4.0 * longitude).rem_euclid(1440.0);
    let hour_angle = (solar_minutes / 4.0 - 180.0).to_radians();
    let phi = latitude.to_radians();
    let cos_zenith = (phi.sin() * declination.sin()
        + phi.cos() * declination.cos() * hour_angle.cos())
    .clamp(-1.0, 1.0);
    let zenith = cos_zenith.acos();
    // Azimuth from the hour angle, clockwise from north.
    let azimuth = (-hour_angle.sin() * declination.cos())
        .atan2(phi.cos() * declination.sin() - phi.sin() * declination.cos() * hour_angle.cos())
        .to_degrees()
        .rem_euclid(360.0);
    SunPosition {
        altitude: 90.0 - zenith.to_degrees(),
        azimuth,
        declination: declination.to_degrees(),
        equation_of_time: eot,
    }
}

/// How far the host's sun is from the independent one.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CrossCheck {
    /// Instants compared (both suns above the horizon).
    pub compared: usize,
    /// Mean angle between the two directions, degrees.
    pub mean_degrees: f64,
    /// Largest angle, degrees.
    pub max_degrees: f64,
}

impl CrossCheck {
    /// The #155 P2 rule: mean within 0.5° and every instant within 2°.
    #[must_use]
    pub fn accepted(&self) -> bool {
        self.compared > 0 && self.mean_degrees <= 0.5 && self.max_degrees <= 2.0
    }
}

/// Compare unit directions pairwise, in the same frame. Pairs where either sun
/// is below the horizon are skipped: the host may not report them.
#[must_use]
pub fn cross_check(host: &[[f64; 3]], independent: &[[f64; 3]]) -> CrossCheck {
    let mut compared = 0_usize;
    let mut sum = 0.0;
    let mut max: f64 = 0.0;
    for (a, b) in host.iter().zip(independent) {
        if a[2] <= 0.0 || b[2] <= 0.0 {
            continue;
        }
        let cos = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]).clamp(-1.0, 1.0);
        let angle = cos.acos() * 180.0 / PI;
        compared += 1;
        sum += angle;
        max = max.max(angle);
    }
    CrossCheck {
        compared,
        mean_degrees: if compared > 0 {
            sum / f64::from(u32::try_from(compared).unwrap_or(u32::MAX))
        } else {
            0.0
        },
        max_degrees: max,
    }
}

#[cfg(test)]
mod tests;
