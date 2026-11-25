use super::*;
use crate::borrow::SliceView;
use tapioca_sunstudy::series::SunStep;

const OK: i32 = 0;
const ERR_ARG: i32 = 1;

fn message(error: &[u8]) -> &str {
    let end = error.iter().position(|b| *b == 0).unwrap_or(error.len());
    std::str::from_utf8(error.get(..end).unwrap_or_default()).unwrap_or("")
}

fn view<T>(data: &[T]) -> SliceView<T> {
    SliceView {
        data: data.as_ptr(),
        count: data.len(),
    }
}

fn sun(hour: i32, altitude: f64, direction: [f64; 3]) -> SunStep {
    SunStep {
        hour,
        minute: 0,
        direction,
        altitude_degrees: altitude,
    }
}

#[test]
fn version_matches_the_core_contract() {
    let mut version = 0;
    let mut error = [b'x'; 64];
    // SAFETY: exclusive local outputs of the declared sizes.
    let status =
        unsafe { tapioca_sunstudy_version(&raw mut version, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, OK);
    assert_eq!(version, tapioca_sunstudy::ABI_VERSION);
    assert_eq!(error.first(), Some(&0));
}

#[test]
fn null_version_is_refused_without_writing() {
    let mut error = [0; 64];
    // SAFETY: the null output is rejected before any write.
    let status =
        unsafe { tapioca_sunstudy_version(std::ptr::null_mut(), error.as_mut_ptr(), error.len()) };
    assert_eq!(status, ERR_ARG);
    assert!(error.starts_with(b"Null or misaligned"));
}

#[test]
fn series_publishes_kept_steps_and_summary() {
    let steps = [
        sun(5, -1.0, [0.0, 0.0, 1.0]),
        sun(12, 40.0, [3.0, 0.0, 4.0]),
    ];
    let request = SunSeriesRequest {
        steps: view(&steps),
        timestep_minutes: 30,
        min_altitude_degrees: 0.0,
    };
    let mut kept = [sun(0, 0.0, [0.0; 3]); 2];
    let mut summary = SunSeriesSummary::default();
    let output = SunSeriesOutput {
        steps: kept.as_mut_ptr(),
        capacity: kept.len(),
        summary: &raw mut summary,
    };
    let mut error = [0_u8; 128];
    // SAFETY: every region is a distinct live local of the declared size.
    let status = unsafe {
        tapioca_sunstudy_series(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    assert_eq!((summary.step_count, summary.source_step_count), (1, 2));
    assert!((summary.daylight_hours - 0.5).abs() < 1e-12);
    assert_ne!(summary.version, 0);
    assert_eq!(kept.first().map(|s| s.direction), Some([0.6, 0.0, 0.8]));
}

#[test]
fn series_refuses_a_short_output_without_writing() {
    let steps = [sun(12, 40.0, [0.0, 0.0, 1.0]); 3];
    let request = SunSeriesRequest {
        steps: view(&steps),
        timestep_minutes: 60,
        min_altitude_degrees: 0.0,
    };
    let mut kept = [sun(0, 0.0, [0.0; 3]); 2];
    let mut summary = SunSeriesSummary {
        step_count: 99,
        ..SunSeriesSummary::default()
    };
    let output = SunSeriesOutput {
        steps: kept.as_mut_ptr(),
        capacity: kept.len(),
        summary: &raw mut summary,
    };
    let mut error = [0_u8; 128];
    // SAFETY: every region is a distinct live local of the declared size.
    let status = unsafe {
        tapioca_sunstudy_series(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, ERR_ARG);
    assert_eq!(message(&error), "SunSeriesOutputTooSmall");
    assert_eq!(summary.step_count, 99);
}

#[test]
fn an_output_aliasing_its_input_is_refused() {
    let mut steps = [sun(12, 40.0, [0.0, 0.0, 1.0]); 2];
    let mut summary = SunSeriesSummary::default();
    let output = SunSeriesOutput {
        steps: steps.as_mut_ptr(),
        capacity: steps.len(),
        summary: &raw mut summary,
    };
    let request = SunSeriesRequest {
        steps: SliceView {
            data: output.steps.cast_const(),
            count: 2,
        },
        timestep_minutes: 60,
        min_altitude_degrees: 0.0,
    };
    // SAFETY: deliberate aliasing is detected before any read of the steps.
    let status = unsafe {
        tapioca_sunstudy_series(
            &raw const request,
            &raw const output,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, ERR_ARG);
    assert_eq!(summary, SunSeriesSummary::default());
}

#[test]
fn camera_reports_a_degenerate_sun_as_invalid_not_an_error() {
    let request = ShadowCameraRequest {
        aabb_min: [0.0; 3],
        aabb_max: [1.0; 3],
        sun_dir: [0.0; 3],
        margin: 1.0,
        resolution: 2048,
        min_bias: 0.005,
    };
    let mut camera = ShadowCamera {
        valid: 7,
        ..ShadowCamera::default()
    };
    // SAFETY: distinct live locals.
    let status = unsafe {
        tapioca_sunstudy_shadow_camera(&raw const request, &raw mut camera, std::ptr::null_mut(), 0)
    };
    assert_eq!(status, OK);
    assert_eq!(camera, ShadowCamera::default());
}

#[test]
fn ground_grid_answers_its_shape_before_filling() {
    let request = GroundGridRequest {
        aabb_min: [0.0; 3],
        aabb_max: [10.0; 3],
        spacing: 1.0,
        pad: 5.0,
        z_offset: 0.1,
        max_samples: 4_000_000,
    };
    let mut summary = GroundGridSummary::default();
    let shape = GroundGridOutput {
        positions: std::ptr::null_mut(),
        scalar_capacity: 0,
        summary: &raw mut summary,
    };
    // SAFETY: distinct live locals; null positions with zero capacity.
    let status = unsafe {
        tapioca_sunstudy_ground_grid(
            &raw const request,
            &raw const shape,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, OK);
    assert_eq!((summary.columns, summary.rows, summary.valid), (21, 21, 1));
    let mut positions = vec![0.0; summary.sample_count * 3 - 1];
    let short = GroundGridOutput {
        positions: positions.as_mut_ptr(),
        scalar_capacity: positions.len(),
        summary: &raw mut summary,
    };
    // SAFETY: distinct live allocations of the declared sizes.
    let status = unsafe {
        tapioca_sunstudy_ground_grid(
            &raw const request,
            &raw const short,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status, ERR_ARG);
    assert!(positions.iter().all(|p| p.abs() < f64::EPSILON));
}

#[test]
fn winding_refuses_a_short_group_list() {
    let vertices = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0];
    let triangles = [0_u32, 1, 2, 0, 2, 1];
    let groups = [0_u32];
    let request = WindingRequest {
        vertices: view(&vertices),
        triangles: view(&triangles),
        groups: view(&groups),
        weld_tolerance: 0.0,
    };
    let mut out = [0_u32; 6];
    let mut report = WindingReportOutput::default();
    let output = WindingOutput {
        triangles: out.as_mut_ptr(),
        capacity: out.len(),
        report: &raw mut report,
    };
    let mut error = [0_u8; 128];
    // SAFETY: distinct live locals of the declared sizes.
    let status = unsafe {
        tapioca_sunstudy_orient_outward(
            &raw const request,
            &raw const output,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, ERR_ARG);
    assert!(message(&error).starts_with("WindingGroupCount"));
    assert_eq!(out, [0; 6]);
}

#[test]
fn limits_refuse_a_non_boolean_flag() {
    let request = LimitsRequest {
        gpu_memory_bytes: 0,
        available_ram_bytes: 0,
        steps: 49,
        occupancy: 0.7,
        analysed_area: 0.0,
        max_texture_dimension: 0,
        gpu_known: 2,
    };
    let mut out = LimitsOutput::default();
    // SAFETY: distinct live locals.
    let status = unsafe {
        tapioca_sunstudy_limits(&raw const request, &raw mut out, std::ptr::null_mut(), 0)
    };
    assert_eq!(status, ERR_ARG);
    assert_eq!(out, LimitsOutput::default());
}

fn annual_handle(samples: usize) -> *mut AnnualHandle {
    let mut handle = std::ptr::null_mut();
    let mut error = [0; 64];
    // SAFETY: exclusive local outputs of the declared sizes.
    let status = unsafe {
        tapioca_sunstudy_annual_create(samples, 1, &raw mut handle, error.as_mut_ptr(), error.len())
    };
    assert_eq!(status, OK, "{}", message(&error));
    handle
}

#[test]
fn annual_sunless_days_represent_their_days_at_zero_hours() {
    let handle = annual_handle(3);
    let mut error = [0; 96];
    let june = AnnualDayRequest {
        weight_days: 30.0,
        month: 6,
        day: 15,
    };
    // SAFETY: live handle; null study = sunless day; local request.
    let status = unsafe {
        tapioca_sunstudy_annual_add_day(
            handle,
            std::ptr::null(),
            &raw const june,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    let bad = AnnualDayRequest {
        weight_days: 1.0,
        month: 13,
        day: 1,
    };
    // SAFETY: as above.
    let status = unsafe {
        tapioca_sunstudy_annual_add_day(
            handle,
            std::ptr::null(),
            &raw const bad,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, ERR_ARG);
    assert_eq!(message(&error), "AnnualInvalidMonth");
    let mut info = AnnualInfo::default();
    // SAFETY: live handle; exclusive local output.
    let status = unsafe {
        tapioca_sunstudy_annual_info(handle, &raw mut info, error.as_mut_ptr(), error.len())
    };
    assert_eq!(status, OK, "{}", message(&error));
    assert_eq!((info.sample_count, info.sampled_days), (3, 1));
    assert!((info.represented_days - 30.0).abs() < f64::EPSILON);
    assert_eq!(info.month_days.get(5).copied(), Some(30.0));
    assert_eq!(info.unoccluded.count, 3);

    // Shape, then fill.
    let mut values = [7.0_f64; 3];
    let mut request = AnnualValuesRequest {
        values: std::ptr::null_mut(),
        capacity: 0,
        quantity: 0,
        month: 0,
        per_day: 0,
        threshold: 0,
    };
    let (mut count, mut ceiling) = (0_usize, -1.0_f64);
    // SAFETY: live handle; local request and outputs, disjoint.
    let status = unsafe {
        tapioca_sunstudy_annual_values(
            handle,
            &raw const request,
            &raw mut count,
            &raw mut ceiling,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    assert_eq!(count, 3);
    assert!(ceiling.abs() < f64::EPSILON);
    request.values = values.as_mut_ptr();
    request.capacity = values.len();
    request.quantity = 2;
    // SAFETY: as above; `values` is writable for its capacity.
    let status = unsafe {
        tapioca_sunstudy_annual_values(
            handle,
            &raw const request,
            &raw mut count,
            &raw mut ceiling,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    assert_eq!(values, [0.0; 3]);
    request.quantity = 9;
    // SAFETY: as above.
    let status = unsafe {
        tapioca_sunstudy_annual_values(
            handle,
            &raw const request,
            &raw mut count,
            &raw mut ceiling,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, ERR_ARG);
    let mut kept = [AnnualArchivedDay::default(); 2];
    let (mut days, mut minutes, mut bytes) = (0_usize, 0_usize, 0_usize);
    // SAFETY: live handle; exclusive local outputs of the declared sizes.
    let status = unsafe {
        tapioca_sunstudy_annual_archive_days(
            handle,
            kept.as_mut_ptr(),
            kept.len(),
            std::ptr::null_mut(),
            0,
            &raw mut days,
            &raw mut minutes,
            &raw mut bytes,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    assert_eq!((days, minutes), (1, 0));
    assert_eq!(
        kept.first().map(|d| (d.month, d.day, d.step_count)),
        Some((6, 15, 0))
    );
    // SAFETY: the live handle is released once and not used again.
    let status =
        unsafe { tapioca_sunstudy_annual_destroy(handle, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, OK);
}

#[test]
fn annual_null_handles_are_refused() {
    let mut error = [0; 64];
    let request = AnnualDayRequest {
        weight_days: 1.0,
        month: 1,
        day: 1,
    };
    // SAFETY: the null handle is rejected before any use.
    let status = unsafe {
        tapioca_sunstudy_annual_add_day(
            std::ptr::null_mut(),
            std::ptr::null(),
            &raw const request,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_ne!(status, OK);
    // SAFETY: null is accepted by destroy.
    let status = unsafe {
        tapioca_sunstudy_annual_destroy(std::ptr::null_mut(), error.as_mut_ptr(), error.len())
    };
    assert_eq!(status, OK);
}

#[test]
fn annual_schedule_is_shape_then_fill_and_validated() {
    let mut error = [0; 64];
    let request = AnnualScheduleRequest {
        year: 2024,
        from: [2, 10],
        to: [3, 31],
        sampling: 0,
    };
    let (mut count, mut represented) = (0_usize, 0_u32);
    // SAFETY: local request and outputs; zero capacity writes no day.
    let status = unsafe {
        tapioca_sunstudy_annual_schedule(
            &raw const request,
            std::ptr::null_mut(),
            0,
            &raw mut count,
            &raw mut represented,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    assert_eq!((count, represented), (2, 20 + 31));
    let mut days = [AnnualScheduledDay::default(); 2];
    // SAFETY: as above; `days` writable for its capacity.
    let status = unsafe {
        tapioca_sunstudy_annual_schedule(
            &raw const request,
            days.as_mut_ptr(),
            days.len(),
            &raw mut count,
            &raw mut represented,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, OK, "{}", message(&error));
    let got: Vec<(u8, u8)> = days.iter().map(|d| (d.month, d.day)).collect();
    assert_eq!(got, vec![(2, 19), (3, 16)]);
    let bad = AnnualScheduleRequest {
        from: [2, 30],
        ..request
    };
    // SAFETY: as above.
    let status = unsafe {
        tapioca_sunstudy_annual_schedule(
            &raw const bad,
            std::ptr::null_mut(),
            0,
            &raw mut count,
            &raw mut represented,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, ERR_ARG);
    assert_eq!(message(&error), "AnnualInvalidDate");
}

fn receptor_request(samples: [&[f64]; 3], ceilings: &[f64], height: f64) -> ReceptorLayerRequest {
    let [positions, normals, areas] = samples;
    ReceptorLayerRequest {
        positions: view(positions),
        normals: view(normals),
        areas: view(areas),
        ceilings: view(ceilings),
        normal_offset: 0.01,
        height,
        min_host_up: 0.5,
        normal: 1,
        area: 1,
    }
}

#[test]
fn receptor_layer_lifts_marks_and_summarises() {
    // A flat sample, a wall sample, and a flat sample under a 2 m balcony.
    let positions = [0.0, 0.0, 0.01, 5.0, 0.01, 1.0, 1.0, 0.0, 0.01];
    let normals = [0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];
    let areas = [1.0, 1.0, 1.0];
    let ceilings = [-1.0, -1.0, 2.0];
    let request = receptor_request([&positions, &normals, &areas], &ceilings, 3.0);
    let mut hosts = [0.0; 9];
    let mut lifted = [0.0; 9];
    let mut facing = [0.0; 9];
    let mut weights = [0.0; 3];
    let mut status = [9_u8; 3];
    let mut summary = ReceptorLayerSummary::default();
    let output = ReceptorLayerOutput {
        hosts: hosts.as_mut_ptr(),
        positions: lifted.as_mut_ptr(),
        normals: facing.as_mut_ptr(),
        areas: weights.as_mut_ptr(),
        status: status.as_mut_ptr(),
        capacity: 3,
        summary: &raw mut summary,
    };
    // SAFETY: distinct live locals of the declared sizes.
    let status_code = unsafe {
        tapioca_sunstudy_receptor_layer(
            &raw const request,
            &raw const output,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(status_code, OK);
    assert_eq!(status, [0, 1, 2]);
    assert_eq!(
        (
            summary.count,
            summary.available,
            summary.steep_host,
            summary.blocked
        ),
        (3, 1, 1, 1)
    );
    assert!((summary.available_area - 1.0).abs() < 1e-12);
    assert!(
        (summary.unavailable_area - 1.0).abs() < 1e-12,
        "wall plan area is 0"
    );
    assert!((lifted.get(2).copied().unwrap_or_default() - 3.0).abs() < 1e-12);
    assert_eq!(facing.get(..3), Some(&[0.0, 0.0, 1.0][..]));
}

#[test]
fn receptor_layer_refuses_bad_options_and_short_ceilings() {
    let positions = [0.0, 0.0, 0.01];
    let normals = [0.0, 0.0, 1.0];
    let areas = [1.0];
    let mut summary = ReceptorLayerSummary::default();
    let output = ReceptorLayerOutput {
        hosts: std::ptr::null_mut(),
        positions: std::ptr::null_mut(),
        normals: std::ptr::null_mut(),
        areas: std::ptr::null_mut(),
        status: std::ptr::null_mut(),
        capacity: 0,
        summary: &raw mut summary,
    };
    let mut error = [0_u8; 64];
    for (ceilings, height) in [(&[][..], -1.0), (&[1.0, 2.0][..], 1.0)] {
        let request = receptor_request([&positions, &normals, &areas], ceilings, height);
        // SAFETY: distinct live locals; summary only.
        let code = unsafe {
            tapioca_sunstudy_receptor_layer(
                &raw const request,
                &raw const output,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(code, ERR_ARG);
        assert!(
            message(&error).starts_with("ReceptorLayer"),
            "{}",
            message(&error)
        );
    }
    assert_eq!(summary, ReceptorLayerSummary::default(), "nothing written");
}

#[test]
fn class_areas_publish_exact_boundaries_by_weight() {
    let hours = [2.999, 3.0, 6.0, 1.0];
    let weights = [1.0, 2.0, 4.0, 8.0];
    let edges = [3.0, 6.0];
    let request = ClassAreasRequest {
        hours: view(&hours),
        areas: view(&weights),
        edges: view(&edges),
    };
    let mut areas = [0.0; 3];
    let mut counts = [0_usize; 3];
    let mut summary = ClassAreasSummary::default();
    let output = ClassAreasOutput {
        areas: areas.as_mut_ptr(),
        counts: counts.as_mut_ptr(),
        capacity: 3,
        summary: &raw mut summary,
    };
    // SAFETY: distinct live locals of the declared sizes.
    let code = unsafe {
        tapioca_sunstudy_class_areas(
            &raw const request,
            &raw const output,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(code, OK);
    assert_eq!(areas, [9.0, 2.0, 4.0]);
    assert_eq!(counts, [2, 1, 1]);
    assert_eq!((summary.classes, summary.unclassified), (3, 0));
    assert!((summary.evaluated_area - 15.0).abs() < 1e-12);
    let short = ClassAreasOutput {
        capacity: 2,
        ..output
    };
    // SAFETY: as above; the shorter capacity is refused before any write.
    let code = unsafe {
        tapioca_sunstudy_class_areas(
            &raw const request,
            &raw const short,
            std::ptr::null_mut(),
            0,
        )
    };
    assert_eq!(code, ERR_ARG);
}
