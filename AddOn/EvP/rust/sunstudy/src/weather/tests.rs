#![expect(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::indexing_slicing,
    reason = "test assertions"
)]

use super::calendar::period_days;
use super::season::{SeasonRule, daily, growing_season, monthly, running_mean};
use super::*;

/// Authored from the `EnergyPlus` data dictionary, not from any audited reader.
struct Fixture {
    leap_header: bool,
    per_hour: u32,
    start: (u8, u8),
    end: (u8, u8),
    leap_days: bool,
}

impl Default for Fixture {
    fn default() -> Self {
        Self {
            leap_header: false,
            per_hour: 1,
            start: (1, 1),
            end: (12, 31),
            leap_days: false,
        }
    }
}

/// Fields 6..=34 for one record; `ghi` drives the radiation columns.
fn data_fields(dry: f64, ghi: f64, extra: f64) -> Vec<String> {
    let mut v: Vec<String> = vec![
        format!("{dry:.1}"),
        "1.0".into(),
        "80".into(),
        "100000".into(),
        format!("{extra}"),
        "1300".into(),
        "300".into(),
        format!("{ghi}"),
        format!("{}", ghi / 2.0),
        format!("{}", ghi / 2.0),
        "0".into(),
        "0".into(),
        "0".into(),
        "0".into(),
        "180".into(),
        "3.0".into(),
        "5".into(),
        "4".into(),
        "20.0".into(),
        "77777".into(),
        "9".into(),
        "999999999".into(),
        "10".into(),
        "0.0850".into(),
        "0".into(),
        "88".into(),
        "0.200".into(),
        "1.5".into(),
        "1.0".into(),
    ];
    v.truncate(29);
    v
}

fn header(f: &Fixture) -> String {
    format!(
        "LOCATION,Testville,TV,LTU,Authored,000001,54.6,25.3,2.0,150.0\n\
         DESIGN CONDITIONS,0\n\
         TYPICAL/EXTREME PERIODS,0\n\
         GROUND TEMPERATURES,0\n\
         HOLIDAYS/DAYLIGHT SAVINGS,{},0,0,0\n\
         COMMENTS 1,\"Authored, with a comma\"\n\
         COMMENTS 2,second\n\
         DATA PERIODS,1,{},Data,Sunday, {}/{},{}/{}\n",
        if f.leap_header { "Yes" } else { "No" },
        f.per_hour,
        f.start.0,
        f.start.1,
        f.end.0,
        f.end.1
    )
}

/// A file whose dry bulb follows a seasonal cosine (-5 °C in January,
/// +19 °C in July) and whose radiation is 400 W/m² from 09:00 to 15:00.
fn build(f: &Fixture, edit: impl Fn(usize, &mut Vec<String>)) -> String {
    let mut text = header(f);
    let leap = f.leap_header || f.leap_days;
    let days = period_days(f.start, f.end, leap);
    let mut index = 0;
    for (n, (month, day)) in days.iter().enumerate() {
        #[expect(clippy::cast_precision_loss, reason = "day index")]
        let phase = (n as f64 / 365.0) * std::f64::consts::TAU;
        let dry = 7.0 - 12.0 * phase.cos();
        for hour in 1..=24 {
            for step in 1..=f.per_hour {
                let lit = (10..=15).contains(&hour);
                let ghi = if lit { 400.0 } else { 0.0 };
                let extra = if lit { 1000.0 } else { 0.0 };
                let minute = if f.per_hour == 1 {
                    0
                } else {
                    step * 60 / f.per_hour
                };
                let mut cols = vec![
                    "2001".to_owned(),
                    month.to_string(),
                    day.to_string(),
                    hour.to_string(),
                    minute.to_string(),
                    "?9?9".to_owned(),
                ];
                cols.extend(data_fields(dry, ghi, extra));
                edit(index, &mut cols);
                text.push_str(&cols.join(","));
                text.push_str("\r\n");
                index += 1;
            }
        }
    }
    text
}

fn parsed(text: &str) -> WeatherFile {
    parse(text.as_bytes()).expect("parses")
}

fn refusal(text: &str) -> Refusal {
    parse(text.as_bytes()).expect_err("refused")
}

#[test]
fn an_hourly_year_reads_its_header_and_every_field() {
    let file = parsed(&build(&Fixture::default(), |_, _| {}));
    let h = &file.header;
    assert_eq!(
        (h.city.as_str(), h.country.as_str(), h.wmo.as_str()),
        ("Testville", "LTU", "000001")
    );
    assert_eq!(
        (h.latitude, h.longitude, h.time_zone_hours, h.elevation_m),
        (54.6, 25.3, 2.0, 150.0)
    );
    assert_eq!(h.comments[0], "Authored, with a comma");
    assert_eq!((h.records_per_hour, h.leap_year), (1, false));
    assert_eq!(file.records.len(), 8760);
    assert_eq!(file.interval_seconds(), 3600);
    let noon = &file.records[12];
    assert_eq!((noon.month, noon.day, noon.hour), (1, 1, 13));
    assert_eq!(noon.get(Field::GlobalHorizontal), 400.0);
    assert_eq!(
        noon.get(Field::CeilingHeight),
        77777.0,
        "unlimited ceiling is a value"
    );
    assert_eq!(noon.get(Field::Albedo), 0.2);
    assert_eq!(file.quality, WeatherQuality::default());
}

#[test]
fn a_fifteen_minute_file_keeps_energy_per_hour() {
    let fixture = Fixture {
        per_hour: 4,
        start: (6, 1),
        end: (6, 2),
        ..Fixture::default()
    };
    let file = parsed(&build(&fixture, |_, _| {}));
    assert_eq!(file.records.len(), 2 * 24 * 4);
    assert_eq!(file.interval_seconds(), 900);
    assert_eq!(file.records[3].hour, 1, "four records share hour 1");
    assert_eq!(file.records[4].hour, 2);
    let days = daily(&file);
    assert_eq!(days.len(), 2);
    // 6 lit hours at a mean 400 W/m^2: 2400 Wh/m^2, not 4 x 2400.
    assert!((days[0].global_horizontal_wh_m2 - 2400.0).abs() < 1e-9);
}

#[test]
fn a_leap_year_is_read_from_the_header_or_a_29_february_record() {
    let by_header = parsed(&build(
        &Fixture {
            leap_header: true,
            ..Fixture::default()
        },
        |_, _| {},
    ));
    assert_eq!(by_header.records.len(), 8784);
    assert!(by_header.header.leap_year);
    let by_records = parsed(&build(
        &Fixture {
            leap_days: true,
            ..Fixture::default()
        },
        |_, _| {},
    ));
    assert!(
        by_records.header.leap_year,
        "the header says No, 29 February is there"
    );
    assert_eq!(by_records.records.len(), 8784);
}

#[test]
fn missing_radiation_is_counted_never_zero() {
    let text = build(&Fixture::default(), |i, cols| {
        if i < 10 {
            cols[13] = "9999".into();
            cols[14] = "9999".into();
        }
        if i == 20 {
            cols[6] = "99.9".into();
            cols[8] = "250".into(); // relative humidity out of range
        }
    });
    let file = parsed(&text);
    assert!(file.records[0].get(Field::GlobalHorizontal).is_nan());
    assert_eq!(file.quality.missing[Field::GlobalHorizontal as usize], 10);
    assert_eq!(file.quality.missing[Field::DirectNormal as usize], 10);
    assert_eq!(file.quality.missing[Field::DryBulb as usize], 1);
    assert_eq!(
        file.quality.out_of_range[Field::RelativeHumidity as usize],
        1
    );
    assert!(file.records[20].get(Field::RelativeHumidity).is_nan());
}

#[test]
fn radiation_after_dark_is_reported_not_removed() {
    let text = build(&Fixture::default(), |i, cols| {
        if i == 2 {
            cols[13] = "35".into();
        }
    });
    let file = parsed(&text);
    assert_eq!(file.quality.radiation_in_dark, 1);
    assert_eq!(file.records[2].get(Field::GlobalHorizontal), 35.0);
}

#[test]
fn an_older_file_without_precipitation_columns_reads_them_as_missing() {
    let text = build(&Fixture::default(), |_, cols| cols.truncate(32));
    let file = parsed(&text);
    assert_eq!(
        file.quality.missing[Field::PrecipitationDepth as usize],
        8760
    );
    assert_eq!(file.quality.missing[Field::Albedo as usize], 8760);
}

#[test]
fn a_truncated_file_is_refused_with_both_counts() {
    let text = build(&Fixture::default(), |_, _| {});
    let cut: Vec<&str> = text.lines().take(8 + 5000).collect();
    let r = refusal(&cut.join("\n"));
    assert_eq!(
        (r.kind, r.expected, r.found),
        (RefusalKind::Count, 8760, 5000)
    );
}

#[test]
fn a_cut_last_line_is_refused_at_its_line() {
    let mut text = build(&Fixture::default(), |_, _| {});
    text.truncate(text.len() - 60);
    let r = refusal(&text);
    assert_eq!(r.kind, RefusalKind::Columns);
    assert_eq!(r.line, 8 + 8760);
}

#[test]
fn a_bad_number_names_its_line_and_column() {
    let text = build(&Fixture::default(), |i, cols| {
        if i == 99 {
            cols[14] = "abc".into();
        }
    });
    let r = refusal(&text);
    assert_eq!((r.kind, r.line, r.column), (RefusalKind::Number, 108, 14));
}

#[test]
fn records_out_of_order_are_refused() {
    let text = build(&Fixture::default(), |i, cols| {
        if i == 30 {
            cols[3] = "9".into();
        }
    });
    let r = refusal(&text);
    assert_eq!((r.kind, r.line), (RefusalKind::Sequence, 39));
}

#[test]
fn malformed_headers_are_refused() {
    let good = build(&Fixture::default(), |_, _| {});
    assert_eq!(refusal("").kind, RefusalKind::Empty);
    let no_location = good.replacen("LOCATION", "PLACE", 1);
    assert_eq!(refusal(&no_location), refuse(RefusalKind::Header, 1));
    let bad_lat = good.replacen("54.6,25.3", "north,25.3", 1);
    assert_eq!(refusal(&bad_lat).kind, RefusalKind::Location);
    let two = good.replacen("DATA PERIODS,1,", "DATA PERIODS,2,", 1);
    assert_eq!(refusal(&two).kind, RefusalKind::SeveralPeriods);
    let seven = good.replacen("DATA PERIODS,1,1,", "DATA PERIODS,1,7,", 1);
    assert_eq!(refusal(&seven).kind, RefusalKind::RecordsPerHour);
}

#[test]
fn the_checksum_is_of_the_bytes() {
    assert_eq!(
        sha256::sha256(b"abc"),
        [
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae,
            0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61,
            0xf2, 0x00, 0x15, 0xad
        ]
    );
    let empty = sha256::sha256(b"");
    assert_eq!(&empty[..4], &[0xe3, 0xb0, 0xc4, 0x42]);
    let long = vec![b'a'; 1_000];
    let digest = sha256::sha256(&long);
    assert_eq!(
        &digest[..4],
        &[0x41, 0xed, 0xec, 0xe4],
        "multi-block, padded into a second block"
    );
    let a = parsed(&build(&Fixture::default(), |_, _| {}));
    let b = parsed(&build(&Fixture::default(), |i, cols| {
        if i == 0 {
            cols[6] = "-5.1".into();
        }
    }));
    assert_ne!(a.sha256, b.sha256, "an edit in place changes the identity");
}

#[test]
fn monthly_sums_are_energy_and_skip_missing() {
    let text = build(&Fixture::default(), |i, cols| {
        if i == 12 {
            cols[13] = "9999".into();
        }
    });
    let file = parsed(&text);
    let months = monthly(&file);
    assert_eq!(months[0].records, 744);
    assert_eq!(months[0].global_missing, 1);
    // 31 days x 6 h x 400 Wh/m^2, less the one missing hour.
    assert!((months[0].global_horizontal_kwh_m2 - (31.0 * 2400.0 - 400.0) / 1000.0).abs() < 1e-9);
    assert!(months[0].mean_dry_bulb_c < months[6].mean_dry_bulb_c);
    assert!((months[1].precipitation_mm - 28.0 * 24.0 * 1.5).abs() < 1e-9);
}

#[test]
fn the_growing_season_is_the_longest_warm_run() {
    let file = parsed(&build(&Fixture::default(), |_, _| {}));
    let means: Vec<f64> = daily(&file).iter().map(|d| d.mean_dry_bulb_c).collect();
    let (first, last) = growing_season(&means, SeasonRule::default()).unwrap();
    // 7 - 12 cos(phase) >= 5  <=>  cos(phase) <= 1/6: days 82..=283.
    assert!((80..=84).contains(&first), "{first}");
    assert!((281..=285).contains(&last), "{last}");
    assert!(
        growing_season(
            &means,
            SeasonRule {
                threshold_c: 30.0,
                ..SeasonRule::default()
            }
        )
        .is_none()
    );
    let all = growing_season(
        &means,
        SeasonRule {
            threshold_c: -20.0,
            ..SeasonRule::default()
        },
    );
    assert_eq!(all, Some((0, 364)));
}

#[test]
fn a_southern_season_wraps_over_new_year() {
    let means: Vec<f64> = (0..365)
        .map(|d| {
            let phase = (f64::from(d) / 365.0) * std::f64::consts::TAU;
            7.0 + 12.0 * phase.cos()
        })
        .collect();
    let (first, last) = growing_season(&means, SeasonRule::default()).unwrap();
    assert!(first > last, "wraps: {first}..{last}");
    let open = growing_season(
        &means,
        SeasonRule {
            wraps: false,
            ..SeasonRule::default()
        },
    )
    .unwrap();
    assert!(open.0 < open.1);
}

#[test]
fn the_running_mean_ignores_missing_days() {
    let smooth = running_mean(&[1.0, f64::NAN, 3.0], 3, false);
    assert_eq!(smooth, vec![1.0, 2.0, 3.0]);
    let all_missing = running_mean(&[f64::NAN; 3], 3, true);
    assert!(all_missing.iter().all(|v| v.is_nan()));
}

#[test]
fn station_distance_is_great_circle() {
    // Vilnius airport to Kaunas: about 91 km.
    let d = distance_m(54.6364, 25.2908, 54.88, 23.88);
    assert!((d - 95_000.0).abs() < 6_000.0, "{d}");
    assert!(distance_m(10.0, 20.0, 10.0, 20.0).abs() < 1e-9);
}

/// Opt-in: every `.epw` under `TAPIOCA_EPW_DIR` (e.g. the reference tree's
/// unzipped files) parses. Their redistribution terms are not checked, so they
/// are never committed.
#[test]
fn reference_files_parse_when_provided() {
    let Ok(dir) = std::env::var("TAPIOCA_EPW_DIR") else {
        return;
    };
    let mut seen = 0;
    let mut stack = vec![std::path::PathBuf::from(dir)];
    while let Some(path) = stack.pop() {
        for entry in std::fs::read_dir(&path).unwrap().flatten() {
            let p = entry.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("epw")) {
                let result = parse(&std::fs::read(&p).unwrap());
                assert!(
                    result.is_ok(),
                    "{}: {:?}",
                    p.display(),
                    result.as_ref().err()
                );
                let file = result.unwrap();
                assert!(file.records.len() >= 8760, "{}", p.display());
                seen += 1;
            }
        }
    }
    assert!(seen > 0, "no .epw under TAPIOCA_EPW_DIR");
}
