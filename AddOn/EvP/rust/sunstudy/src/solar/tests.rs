#![expect(clippy::unwrap_used, reason = "test assertions")]

use super::*;

/// (month, day, hour, minute) with records per hour and the station offset.
fn record(
    (month, day, hour, minute): (u32, u32, u32, u32),
    per_hour: u32,
    offset: f64,
) -> RecordTime {
    RecordTime {
        year: 2026,
        month,
        day,
        hour,
        minute,
        records_per_hour: per_hour,
        station_offset_hours: offset,
    }
}

#[test]
fn julian_days_match_the_published_examples() {
    // Meeus, Astronomical Algorithms, example 7.a and J2000.0.
    assert!((julian_day(1957, 10, 4) + 0.81 - 2_436_116.31).abs() < 1e-9);
    assert!((julian_day(2000, 1, 1) + 0.5 - J2000).abs() < 1e-9);
    assert!((julian_day(1987, 6, 19) - 2_446_965.5).abs() < 1e-9);
}

#[test]
fn civil_time_round_trips_across_midnight_and_the_year() {
    for (y, m, d, s, offset) in [
        (2026, 1, 1, 1800.0, 2.0),
        (2026, 12, 31, 85_000.0, -5.0),
        (2024, 2, 29, 43_200.0, 5.5),
        (2026, 6, 21, 0.0, 0.0),
    ] {
        let c = Civil {
            year: y,
            month: m,
            day: d,
            seconds: s,
        };
        let back = civil(instant(c, offset), offset);
        assert_eq!((back.year, back.month, back.day), (y, m, d));
        assert!((back.seconds - s).abs() < 1e-3, "{back:?}");
    }
}

#[test]
fn an_hourly_record_sits_at_the_middle_of_the_hour_before_its_stamp() {
    // Hour 1 on 1 January at a station 2 h ahead of UTC: 00:30 local is
    // 22:30 UTC on 31 December of the year before.
    let (at, hours) = interval_midpoint(&record((1, 1, 1, 60), 1, 2.0)).unwrap();
    assert!((hours - 1.0).abs() < 1e-12);
    let utc = civil(at, 0.0);
    assert_eq!((utc.year, utc.month, utc.day), (2025, 12, 31));
    assert!((utc.seconds - 81_000.0).abs() < 1e-3);
    // Minute 0 and minute 60 mean the same end of the hour.
    let (zero, _) = interval_midpoint(&record((6, 1, 13, 0), 1, 2.0)).unwrap();
    let (sixty, _) = interval_midpoint(&record((6, 1, 13, 60), 1, 2.0)).unwrap();
    assert!((zero.julian_day - sixty.julian_day).abs() < 1e-12);
    // Hour 24 ends at midnight: its middle is 23:30 the same day.
    let late = civil(
        interval_midpoint(&record((3, 10, 24, 0), 1, 0.0))
            .unwrap()
            .0,
        0.0,
    );
    assert_eq!((late.month, late.day), (3, 10));
    assert!((late.seconds - 84_600.0).abs() < 1e-3);
}

#[test]
fn a_quarter_hour_record_ends_at_its_minute() {
    let (at, hours) = interval_midpoint(&record((4, 2, 1, 15), 4, 0.0)).unwrap();
    assert!((hours - 0.25).abs() < 1e-12);
    assert!((civil(at, 0.0).seconds - 450.0).abs() < 1e-3);
    let (last, _) = interval_midpoint(&record((4, 2, 1, 60), 4, 0.0)).unwrap();
    assert!((civil(last, 0.0).seconds - 3150.0).abs() < 1e-3);
}

#[test]
fn records_out_of_range_are_refused() {
    assert!(interval_midpoint(&record((13, 1, 1, 0), 1, 0.0)).is_none());
    assert!(interval_midpoint(&record((1, 1, 0, 0), 1, 0.0)).is_none());
    assert!(interval_midpoint(&record((1, 1, 25, 0), 1, 0.0)).is_none());
    assert!(interval_midpoint(&record((1, 1, 1, 0), 7, 0.0)).is_none());
    assert!(interval_midpoint(&record((1, 1, 1, 0), 1, f64::NAN)).is_none());
}

fn noon_utc(month: u32, day: u32, longitude: f64) -> (Instant, f64) {
    // Iterate once on the equation of time to land on local solar noon.
    let mut at = instant(
        Civil {
            year: 2026,
            month,
            day,
            seconds: (12.0 - longitude / 15.0) * 3600.0,
        },
        0.0,
    );
    for _ in 0..2 {
        let eot = position(at, 0.0, longitude).equation_of_time;
        at = instant(
            Civil {
                year: 2026,
                month,
                day,
                seconds: (12.0 - longitude / 15.0) * 3600.0 - eot * 60.0,
            },
            0.0,
        );
    }
    (at, longitude)
}

#[test]
fn declination_and_the_equation_of_time_follow_the_year() {
    let solstice = position(noon_utc(6, 21, 0.0).0, 0.0, 0.0);
    assert!((solstice.declination - 23.44).abs() < 0.03, "{solstice:?}");
    let winter = position(noon_utc(12, 21, 0.0).0, 0.0, 0.0);
    assert!((winter.declination + 23.44).abs() < 0.03, "{winter:?}");
    // The equation of time's extremes: about +16.4 min in early November and
    // −14.2 min in mid-February.
    let november = position(noon_utc(11, 3, 0.0).0, 0.0, 0.0);
    assert!(
        (november.equation_of_time - 16.4).abs() < 0.3,
        "{november:?}"
    );
    let february = position(noon_utc(2, 11, 0.0).0, 0.0, 0.0);
    assert!(
        (february.equation_of_time + 14.2).abs() < 0.3,
        "{february:?}"
    );
}

#[test]
fn the_noon_sun_stands_due_south_or_north_at_its_meridian_altitude() {
    // Vilnius at the June solstice: 90 − 54.687 + 23.44.
    let (at, lon) = noon_utc(6, 21, 25.28);
    let p = position(at, 54.687, lon);
    assert!((p.altitude - (90.0 - 54.687 + 23.44)).abs() < 0.05, "{p:?}");
    assert!((p.azimuth - 180.0).abs() < 0.2, "{p:?}");
    // Southern hemisphere: the noon sun is due north.
    let (at, lon) = noon_utc(6, 21, 151.2);
    let p = position(at, -33.87, lon);
    assert!(p.azimuth < 0.2 || p.azimuth > 359.8, "{p:?}");
    assert!((p.altitude - (90.0 - 33.87 - 23.44)).abs() < 0.05, "{p:?}");
}

#[test]
fn the_morning_sun_rises_in_the_east() {
    let at = instant(
        Civil {
            year: 2026,
            month: 3,
            day: 20,
            seconds: 6.5 * 3600.0,
        },
        2.0,
    );
    let p = position(at, 54.687, 25.28);
    assert!(p.azimuth > 80.0 && p.azimuth < 100.0, "{p:?}");
    let d = p.direction();
    assert!(d[0] > 0.9);
}

#[test]
fn the_cross_check_accepts_agreement_and_refuses_an_hour_off() {
    let place = (54.687, 25.28);
    let suns = |shift_hours: f64| -> Vec<[f64; 3]> {
        (6..20)
            .map(|h| {
                let at = instant(
                    Civil {
                        year: 2026,
                        month: 6,
                        day: 1,
                        seconds: (f64::from(h) + shift_hours) * 3600.0,
                    },
                    2.0,
                );
                position(at, place.0, place.1).direction()
            })
            .collect()
    };
    let same = cross_check(&suns(0.0), &suns(0.0));
    assert!(same.accepted() && same.max_degrees < 1e-5, "{same:?}");
    // Daylight saving leaking into the host: an hour is ~15° of hour angle.
    let dst = cross_check(&suns(1.0), &suns(0.0));
    assert!(!dst.accepted() && dst.mean_degrees > 5.0, "{dst:?}");
    // Night pairs are skipped; nothing compared is not accepted.
    assert!(!cross_check(&[[0.0, 1.0, -0.1]], &[[0.0, 1.0, -0.1]]).accepted());
}

#[test]
fn days_of_the_year_count_from_the_first_of_january() {
    assert_eq!(day_of_year(2026, 1, 1), 1);
    assert_eq!(day_of_year(2026, 3, 1), 60);
    assert_eq!(day_of_year(2024, 3, 1), 61);
    assert_eq!(day_of_year(2024, 12, 31), 366);
}
