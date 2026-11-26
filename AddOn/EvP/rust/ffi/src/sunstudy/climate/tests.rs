#![expect(
    clippy::indexing_slicing,
    reason = "test assertions on fixed-size outputs"
)]

use super::*;

const OK: i32 = 0;

fn view<T>(data: &[T]) -> SliceView<T> {
    SliceView {
        data: data.as_ptr(),
        count: data.len(),
    }
}

fn message(error: &[u8]) -> String {
    let end = error.iter().position(|b| *b == 0).unwrap_or(error.len());
    String::from_utf8_lossy(&error[..end]).into_owned()
}

#[test]
fn sky_patches_size_then_fill_in_the_domes_order() {
    let mut error = [0_u8; 256];
    let mut written = 0_usize;
    // SAFETY: valid local buffers; zero capacity with a null-free empty slice.
    let code = unsafe {
        tapioca_sunstudy_sky_patches(
            0,
            0.0,
            std::ptr::NonNull::dangling().as_ptr(),
            0,
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK, "{}", message(&error));
    assert_eq!(written, 145);
    let mut out = vec![SkyPatchRecord::default(); written];
    // SAFETY: valid local buffers.
    let code = unsafe {
        tapioca_sunstudy_sky_patches(
            1,
            0.5,
            out.as_mut_ptr(),
            145,
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK);
    assert_eq!(written, 577, "a Reinhart dome does not fit 145");
    let mut out = vec![SkyPatchRecord::default(); 577];
    // SAFETY: valid local buffers.
    let code = unsafe {
        tapioca_sunstudy_sky_patches(
            1,
            0.5,
            out.as_mut_ptr(),
            out.len(),
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK);
    let omega: f64 = out.iter().map(|p| p.solid_angle).sum();
    assert!((omega - std::f64::consts::TAU).abs() < 1e-12);
    // Turned by 0.5 rad: the first patch (north) now points to (−sin, cos).
    let first = out[0].direction;
    let h = first[0].hypot(first[1]);
    assert!(
        (first[0] / h + 0.5_f64.sin()).abs() < 1e-9 && (first[1] / h - 0.5_f64.cos()).abs() < 1e-9
    );
    // SAFETY: valid local buffers.
    let bad = unsafe {
        tapioca_sunstudy_sky_patches(
            2,
            0.0,
            out.as_mut_ptr(),
            out.len(),
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_ne!(bad, OK);
}

fn create(
    normals: &[f64],
    (open, canopy): (&[u8], &[u8]),
    sky: u8,
    error: &mut [u8],
) -> (i32, *mut ClimateHandle) {
    let request = ClimateCreateRequest {
        normals: view(normals),
        open: view(open),
        canopy: view(canopy),
        canopy_transmittance: [0.5; 12],
        rotation: 0.0,
        patch_set: 0,
        sky,
        lux_source: 0,
        reserved: [0; 5],
        lux_threshold: 50_000.0,
    };
    let mut handle: *mut ClimateHandle = std::ptr::null_mut();
    // SAFETY: valid local request and slot.
    let code = unsafe {
        tapioca_sunstudy_climate_create(
            &raw const request,
            &raw mut handle,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    (code, handle)
}

#[test]
fn a_run_crosses_the_boundary_and_reads_back_its_totals() {
    let mut error = [0_u8; 256];
    // Two samples: open horizontal, and vertical facing south through canopy.
    let normals = [0.0, 0.0, 1.0, 0.0, -1.0, 0.0];
    let mut open = vec![255_u8; 145];
    open.extend(vec![0_u8; 145]);
    let mut canopy = vec![0_u8; 145];
    canopy.extend(vec![255_u8; 145]);
    let (code, handle) = create(&normals, (&open, &canopy), 0, &mut error);
    assert_eq!(code, OK, "{}", message(&error));
    let a = 40.0_f64.to_radians();
    let interval = ClimateIntervalRecord {
        sun: [0.0, -a.cos(), a.sin()],
        hours: 1.0,
        dni: 600.0,
        dhi: 120.0,
        albedo: 0.25,
        dew_point_c: f64::NAN,
        ghi: 600.0 * a.sin() + 132.0, // the file reads 12 W/m² above its DNI and DHI
        file_lux: [f64::NAN, 60_000.0, 20_000.0],
        day_of_year: 160,
        month: 5,
        reserved: [0; 3],
    };
    let beam = [2_u8, 1];
    // SAFETY: live handle, local records.
    let code = unsafe {
        tapioca_sunstudy_climate_add(
            handle,
            &raw const interval,
            view(&beam),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK, "{}", message(&error));
    // A beam is required with the sun up.
    // SAFETY: as above.
    let missing = unsafe {
        tapioca_sunstudy_climate_add(
            handle,
            &raw const interval,
            view(&[] as &[u8]),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_ne!(missing, OK);
    assert!(message(&error).contains("ClimateBeamMissing"));

    let (mut direct, mut diffuse, mut reflected) = (vec![0.0; 24], vec![0.0; 24], vec![0.0; 24]);
    let mut svf = vec![0.0; 2];
    let (mut lux_hours, mut hours_above) = (vec![0.0; 24], vec![0.0; 24]);
    let mut report = ClimateReportRecord::default();
    let output = ClimateReadOutput {
        direct: direct.as_mut_ptr(),
        diffuse: diffuse.as_mut_ptr(),
        reflected: reflected.as_mut_ptr(),
        sky_view: svf.as_mut_ptr(),
        lux_hours: lux_hours.as_mut_ptr(),
        hours_above: hours_above.as_mut_ptr(),
        capacity: 2,
        report: &raw mut report,
    };
    // SAFETY: live handle, disjoint local outputs.
    let code = unsafe {
        tapioca_sunstudy_climate_read(handle, &raw const output, error.as_mut_ptr(), error.len())
    };
    assert_eq!(code, OK, "{}", message(&error));
    assert_eq!((report.sample_count, report.patch_count), (2, 145));
    let open = 600.0 * a.sin() + 120.0;
    assert!((report.open_horizontal[5] - open).abs() < 1e-9);
    // The file's GHI crosses the boundary beside the model's, for the check.
    assert!((report.compared_model[5] - open).abs() < 1e-9);
    assert!((report.compared_file[5] - (open + 12.0)).abs() < 1e-9);
    assert!((report.compared_hours[5] - 1.0).abs() < 1e-12);
    // One hour is below the comparison floor (1 kWh/m² a month): shown as
    // not compared, never as a pass.
    assert!(report.closure_monthly[5].is_nan() && report.closure_annual.is_nan());
    assert_eq!(report.closure_worst_month, -1);
    assert_eq!(report.closure_within, 0);
    assert!((report.closure_tolerance - 0.05).abs() < 1e-15);
    assert!((report.closure_hours - 1.0).abs() < 1e-12);
    // The file's illuminance crossed and was used, and the report says so.
    assert_eq!(report.lux_kept, 1);
    assert!((report.lux_threshold - 50_000.0).abs() < 1e-12);
    assert!((report.lux_file_hours[5] - 1.0).abs() < 1e-12);
    assert!(report.lux_efficacy_hours[5].abs() < 1e-12);
    // The open horizontal sample: 60 klx x sin a + 20 klx of sky, above 50 klx.
    let open_lux = 60_000.0 * a.sin() + 20_000.0;
    assert!((lux_hours[5] - open_lux).abs() < 1e-3 * open_lux);
    assert!((hours_above[5] - f64::from(u8::from(open_lux >= 50_000.0))).abs() < 1e-12);
    assert!((direct[5] + diffuse[5] - open).abs() < 1e-9 * open);
    // The canopy sample: half the beam it faces.
    assert!((direct[12 + 5] - 0.5 * 600.0 * a.cos()).abs() < 1e-9);
    assert!(
        (svf[0] - 1.0).abs() < 1e-12 && svf[1].abs() < 1e-12,
        "canopy is not open sky"
    );
    assert!(reflected[12 + 5] > 0.0 && reflected[5].abs() < 1e-12);
    // SAFETY: the handle is released once.
    let released =
        unsafe { tapioca_sunstudy_climate_destroy(handle, error.as_mut_ptr(), error.len()) };
    assert_eq!(released, OK);
}

#[test]
fn malformed_runs_are_refused() {
    let mut error = [0_u8; 256];
    let (code, _) = create(&[0.0, 0.0, 1.0], (&[255_u8; 10], &[]), 0, &mut error);
    assert_ne!(code, OK);
    assert!(message(&error).contains("ClimateMasks"));
    let (code, _) = create(
        &[0.0, 0.0, 1.0],
        (&[200_u8; 145], &[100_u8; 145]),
        0,
        &mut error,
    );
    assert_ne!(code, OK, "open and canopy together exceed the patch");
    let (code, _) = create(&[0.0, 0.0], (&[], &[]), 0, &mut error);
    assert_ne!(code, OK);
    let (code, _) = create(&[0.0, 0.0, 1.0], (&[255_u8; 145], &[]), 7, &mut error);
    assert_ne!(code, OK);
    // SAFETY: null is accepted.
    let released = unsafe {
        tapioca_sunstudy_climate_destroy(std::ptr::null_mut(), error.as_mut_ptr(), error.len())
    };
    assert_eq!(released, OK);
}

#[test]
fn sky_cells_split_each_patch_into_weighted_rays() {
    let mut error = [0_u8; 256];
    let mut written = 0_usize;
    // SAFETY: valid local buffers; a zero-capacity size query.
    let code = unsafe {
        tapioca_sunstudy_sky_cells(
            0,
            0.0,
            2,
            std::ptr::NonNull::dangling().as_ptr(),
            0,
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK, "{}", message(&error));
    assert_eq!(written, 145 * 4);
    let mut cells = vec![SkyCellRecord::default(); written];
    // SAFETY: valid local buffers.
    let code = unsafe {
        tapioca_sunstudy_sky_cells(
            0,
            0.0,
            2,
            cells.as_mut_ptr(),
            cells.len(),
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(code, OK);
    assert_eq!((cells[0].patch, cells[3].patch, cells[4].patch), (0, 0, 1));
    let projected: f64 = cells.iter().map(|c| c.moment[2]).sum();
    assert!((projected - std::f64::consts::PI).abs() < 1e-12);
    // SAFETY: valid local buffers.
    let bad = unsafe {
        tapioca_sunstudy_sky_cells(
            0,
            0.0,
            17,
            cells.as_mut_ptr(),
            cells.len(),
            &raw mut written,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_ne!(bad, OK);
}
