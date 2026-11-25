#![expect(
    clippy::indexing_slicing,
    reason = "test assertions on fixed-size outputs"
)]

use super::*;
use std::fmt::Write as _;

const OK: i32 = 0;

fn view<T>(data: &[T]) -> SliceView<T> {
    SliceView {
        data: data.as_ptr(),
        count: data.len(),
    }
}

/// One January day, hourly; `bad_hour` writes an out-of-sequence hour.
fn one_day(bad_hour: bool) -> String {
    let mut text = String::from(
        "LOCATION,Testville,TV,LTU,Authored,000001,54.6,25.3,2.0,150.0\n\
         DESIGN CONDITIONS,0\nTYPICAL/EXTREME PERIODS,0\nGROUND TEMPERATURES,0\n\
         HOLIDAYS/DAYLIGHT SAVINGS,No,0,0,0\nCOMMENTS 1,one\nCOMMENTS 2,two\n\
         DATA PERIODS,1,1,Data,Sunday, 1/ 1, 1/ 1\n",
    );
    for hour in 1..=24 {
        let written = if bad_hour && hour == 5 { 7 } else { hour };
        let ghi = if (10..=15).contains(&hour) { 300 } else { 0 };
        let _ = writeln!(
            text,
            "2001,1,1,{written},0,?9,-3.0,-5.0,80,100000,{ghi},1300,300,{ghi},100,9999,0,0,0,0,\
             180,3.0,5,4,20.0,77777,9,999999999,10,0.0850,0,88,0.200,0.0,0.0"
        );
    }
    text
}

struct Read {
    code: i32,
    summary: WeatherSummary,
    records: Vec<WeatherRecordOut>,
    days: Vec<WeatherDayOut>,
    months: [WeatherMonthOut; 12],
}

fn read(text: &str, capacity: usize) -> Read {
    let bytes = text.as_bytes();
    let request = WeatherReadRequest { bytes: view(bytes) };
    let blank = WeatherRecordOut {
        year: 0,
        month: 0,
        day: 0,
        hour: 0,
        minute: 0,
        values: [0.0; FIELD_COUNT],
    };
    let mut out = Read {
        code: -1,
        summary: WeatherSummary::default(),
        records: vec![blank; capacity],
        days: vec![WeatherDayOut::default(); 366],
        months: [WeatherMonthOut::default(); 12],
    };
    let output = WeatherReadOutput {
        records: out.records.as_mut_ptr(),
        record_capacity: capacity,
        days: out.days.as_mut_ptr(),
        day_capacity: 366,
        months: out.months.as_mut_ptr(),
        summary: &raw mut out.summary,
    };
    // SAFETY: distinct live locals of the declared sizes.
    out.code = unsafe {
        tapioca_sunstudy_weather_read(
            &raw const request,
            &raw const output,
            std::ptr::null_mut(),
            0,
        )
    };
    out
}

#[test]
fn a_parsed_file_publishes_header_records_days_and_months() {
    let r = read(&one_day(false), 64);
    assert_eq!(r.code, OK);
    assert_eq!(r.summary.refusal, 0);
    assert_eq!(
        (
            r.summary.records,
            r.summary.days,
            r.summary.records_per_hour
        ),
        (24, 1, 1)
    );
    assert_eq!(&r.summary.city[..10], b"Testville\0");
    assert_eq!(
        (r.summary.period_start, r.summary.period_end),
        ([1, 1], [1, 1])
    );
    assert_eq!(
        r.summary.missing[9], 24,
        "diffuse horizontal (column 15) is 9999"
    );
    let noon = r.records[12];
    assert_eq!((noon.month, noon.day, noon.hour), (1, 1, 13));
    assert!((r.days[0].global_horizontal_wh_m2 - 1800.0).abs() < 1e-9);
    assert_eq!(r.months[0].records, 24);
    assert!(
        r.months[1].mean_dry_bulb_c.is_nan(),
        "February is outside the period"
    );
}

#[test]
fn a_refused_file_is_a_result_with_its_line() {
    let r = read(&one_day(true), 64);
    assert_eq!(r.code, OK);
    assert_eq!(r.summary.refusal, 10, "Sequence");
    assert_eq!(r.summary.line, 8 + 5);
    assert_eq!(r.summary.records, 0);
}

#[test]
fn too_small_an_output_is_a_call_failure() {
    let r = read(&one_day(false), 10);
    assert_ne!(r.code, OK);
}

#[test]
fn season_and_distance_cross_the_boundary() {
    let means: Vec<f64> = (0..365)
        .map(|d| if (100..=280).contains(&d) { 12.0 } else { -2.0 })
        .collect();
    let request = WeatherSeasonRequest {
        daily_mean_c: view(&means),
        window_days: 1,
        threshold_c: 5.0,
        wraps: 1,
        reserved: [0; 7],
    };
    let mut result = WeatherSeasonResult::default();
    // SAFETY: distinct live locals.
    let code = unsafe {
        tapioca_sunstudy_weather_season(
            &raw const request,
            &raw mut result,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(code, OK);
    assert_eq!((result.found, result.first, result.last), (1, 100, 280));
    let even = WeatherSeasonRequest {
        window_days: 2,
        ..request
    };
    // SAFETY: as above.
    let code = unsafe {
        tapioca_sunstudy_weather_season(&raw const even, &raw mut result, std::ptr::null_mut(), 0)
    };
    assert_ne!(code, OK, "an even window has no centre");
    let points = StationDistanceRequest {
        a: [54.0, 25.0],
        b: [55.0, 25.0],
    };
    let mut metres = 0.0;
    // SAFETY: distinct live locals.
    let code = unsafe {
        tapioca_sunstudy_station_distance(
            &raw const points,
            &raw mut metres,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(code, OK);
    assert!((metres - 111_195.0).abs() < 50.0, "{metres}");
}
