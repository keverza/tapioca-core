#![expect(
    clippy::indexing_slicing,
    reason = "test assertions on fixed-size outputs"
)]

use super::super::climate::{
    ClimateCreateRequest, ClimateIntervalRecord, tapioca_sunstudy_climate_add,
    tapioca_sunstudy_climate_create, tapioca_sunstudy_climate_destroy,
};
use super::*;

const OK: i32 = 0;

fn view<T>(data: &[T]) -> SliceView<T> {
    SliceView {
        data: data.as_ptr(),
        count: data.len(),
    }
}

fn instants(stamps: &[WeatherStamp], rotation: f64) -> (i32, Vec<WeatherInstant>) {
    let request = WeatherInstantRequest {
        stamps: view(stamps),
        year: 2026,
        records_per_hour: 1,
        station_offset_hours: 2.0,
        project_offset_hours: 2.0,
        latitude: 54.687,
        longitude: 25.28,
        rotation,
    };
    let mut out = vec![WeatherInstant::default(); stamps.len()];
    let mut error = [0_u8; 256];
    // SAFETY: valid local buffers.
    let code = unsafe {
        tapioca_sunstudy_weather_instants(
            &raw const request,
            out.as_mut_ptr(),
            out.len(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    (code, out)
}

fn stamp(month: u8, day: u8, hour: u8) -> WeatherStamp {
    WeatherStamp {
        month,
        day,
        hour,
        minute: 60,
    }
}

#[test]
fn records_land_on_the_project_clock_with_an_independent_sun() {
    let (code, out) = instants(&[stamp(6, 21, 13), stamp(1, 1, 1), stamp(13, 1, 1)], 0.0);
    assert_eq!(code, OK);
    // Hour 13 covers 12:00-13:00 station time: midpoint 12:30 on the same
    // clock, the sun high and south-west of south.
    let noon = out[0];
    assert_eq!(noon.valid, 1);
    assert_eq!((noon.project_month, noon.project_day), (6, 21));
    assert!((noon.project_seconds - 45_000.0).abs() < 1e-3);
    assert_eq!(noon.day_of_year, 172);
    assert!(noon.independent_sun[2] > 0.8 && noon.independent_sun[1] < 0.0);
    // Hour 1 on 1 January: 00:30, the sun far below the horizon.
    assert_eq!(out[1].day_of_year, 1);
    assert!(out[1].independent_sun[2] < -0.5);
    assert_eq!(out[2].valid, 0, "month 13 is refused, not guessed");
}

#[test]
fn the_independent_sun_turns_with_the_model_frame() {
    let (_, plain) = instants(&[stamp(6, 21, 13)], 0.0);
    let (_, turned) = instants(&[stamp(6, 21, 13)], std::f64::consts::FRAC_PI_2);
    let [x, y, z] = plain[0].independent_sun;
    let t = turned[0].independent_sun;
    assert!((t[0] + y).abs() < 1e-12 && (t[1] - x).abs() < 1e-12 && (t[2] - z).abs() < 1e-12);
}

#[test]
fn the_cross_check_crosses_the_boundary() {
    let up = [0.0, 0.6, 0.8, 0.0, -0.6, 0.8];
    let mut record = SunCrossCheckRecord::default();
    let mut error = [0_u8; 256];
    // SAFETY: valid local buffers.
    let code = unsafe {
        tapioca_sunstudy_sun_cross_check(
            view(&up),
            view(&up),
            &raw mut record,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK);
    assert_eq!((record.compared, record.accepted), (2, 1));
    // SAFETY: valid local buffers.
    let bad = unsafe {
        tapioca_sunstudy_sun_cross_check(
            view(&up[..3]),
            view(&up),
            &raw mut record,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_ne!(bad, OK);
}

#[test]
fn a_run_reads_its_daily_light_integral() {
    let mut error = [0_u8; 256];
    let normals = [0.0, 0.0, 1.0];
    let open = [255_u8; 145];
    let request = ClimateCreateRequest {
        normals: view(&normals),
        open: view(&open),
        canopy: view(&[] as &[u8]),
        canopy_transmittance: [0.0; 12],
        rotation: 0.0,
        patch_set: 0,
        sky: 1,
        reserved: [0; 6],
    };
    let mut handle = std::ptr::null_mut();
    // SAFETY: valid local request and slot.
    let code = unsafe {
        tapioca_sunstudy_climate_create(
            &raw const request,
            &raw mut handle,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK);
    // 24 one-hour intervals in March: 12 dark, 12 of 500 W/m² diffuse.
    for h in 0..24 {
        let interval = ClimateIntervalRecord {
            sun: [0.0, -1.0, 0.0],
            hours: 1.0,
            dni: 0.0,
            dhi: if h < 12 { 500.0 } else { 0.0 },
            albedo: 0.2,
            dew_point_c: f64::NAN,
            ghi: f64::NAN,
            day_of_year: 80,
            month: 2,
            reserved: [0; 3],
        };
        // SAFETY: live handle; no beam needed with the sun down.
        let code = unsafe {
            tapioca_sunstudy_climate_add(
                handle,
                &raw const interval,
                view(&[] as &[u8]),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(code, OK);
    }
    let mut dli = vec![0.0; 12];
    // SAFETY: live handle, local output.
    let code = unsafe {
        tapioca_sunstudy_climate_dli(
            handle,
            0.0,
            0.0,
            dli.as_mut_ptr(),
            1,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK);
    // 6 kWh/m² in one day: 44.42 mol/m²/day; months without weather are NaN.
    assert!((dli[2] - 44.4204).abs() < 1e-3, "{}", dli[2]);
    assert!(dli[0].is_nan() && dli[11].is_nan());
    // SAFETY: as above.
    let bad = unsafe {
        tapioca_sunstudy_climate_dli(
            handle,
            -1.0,
            0.0,
            dli.as_mut_ptr(),
            1,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_ne!(bad, OK);
    // SAFETY: released once.
    let released =
        unsafe { tapioca_sunstudy_climate_destroy(handle, error.as_mut_ptr(), error.len()) };
    assert_eq!(released, OK);
}
