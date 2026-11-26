//! Sky patches and the climate accumulator as a Rust-owned handle (#269,
//! #155 F3, #278 Stage 5).
//!
//! Handle contract: one thread at a time. `add` and `destroy` need exclusive
//! use; `read` only reads. A refused interval changes nothing.

use super::Regions;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::climate::{
    Climate, ClimateInterval, ClimateRefusal, ClimateSetup, DiffuseSky, FILE_CONSISTENCY_TOLERANCE,
    SkyMasks, horizontal_closure,
};
use tapioca_sunstudy::illuminance::{LuxOptions, LuxSource};
use tapioca_sunstudy::sky::{Dome, PatchSet};

/// One sky patch in the model frame.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SkyPatchRecord {
    /// Unit centroid direction: where a mask ray is cast.
    pub direction: [f64; 3],
    /// ∫ d dΩ over the patch, steradians.
    pub moment: [f64; 3],
    /// ∫ dΩ, steradians.
    pub solid_angle: f64,
}

fn patch_set(code: u8) -> Result<PatchSet, Failure> {
    match code {
        0 => Ok(PatchSet::Tregenza),
        1 => Ok(PatchSet::Reinhart),
        _ => Err(Failure::Argument("SkyPatchSet: 0 Tregenza, 1 Reinhart")),
    }
}

fn dome(set: u8, rotation: f64) -> Result<Dome, Failure> {
    if !rotation.is_finite() {
        return Err(Failure::Argument("SkyRotation: not finite"));
    }
    Ok(Dome::new(patch_set(set)?).rotated(rotation))
}

/// The dome's patches in its order, turned by `rotation` radians about z
/// (sky-frame north lands on (−sin, cos)). With `capacity` below the count
/// only `written` is set, so a first call sizes the buffer.
///
/// # Safety
/// `out` writable for `capacity` records, `written` one writable slot, both
/// disjoint from each other and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_sky_patches(
    set: u8,
    rotation: f64,
    out: *mut SkyPatchRecord,
    capacity: usize,
    written: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; regions are checked before writes.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(written, 1);
            regions.output(out, capacity);
            regions.check()?;
            let dome = dome(set, rotation)?;
            written.write(dome.len());
            if capacity < dome.len() {
                return Ok(());
            }
            for (k, p) in dome.patches().iter().enumerate() {
                out.add(k).write(SkyPatchRecord {
                    direction: p.direction,
                    moment: p.moment,
                    solid_angle: p.solid_angle,
                });
            }
            Ok(())
        })
    }
}

/// One cell of a sky patch in the model frame.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SkyCellRecord {
    /// Unit centroid direction: where a mask ray is cast.
    pub direction: [f64; 3],
    /// ∫ d dΩ over the cell, steradians: its projected weight on a plane with
    /// normal n is max(0, n·moment).
    pub moment: [f64; 3],
    /// ∫ dΩ, steradians.
    pub solid_angle: f64,
    /// The patch it belongs to, in the dome's order.
    pub patch: u32,
    /// Zero.
    pub reserved: u32,
}

/// Every patch split into `per_side`² cells, patch by patch, as
/// `tapioca_sunstudy_sky_patches` orders and turns them. With `capacity`
/// below the count only `written` is set.
///
/// # Safety
/// As `tapioca_sunstudy_sky_patches`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_sky_cells(
    set: u8,
    rotation: f64,
    per_side: u32,
    out: *mut SkyCellRecord,
    capacity: usize,
    written: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; regions are checked before writes.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(written, 1);
            regions.output(out, capacity);
            regions.check()?;
            if !(1..=16).contains(&per_side) {
                return Err(Failure::Argument("SkyCells: 1-16 per side"));
            }
            let cells = dome(set, rotation)?.cells(per_side);
            written.write(cells.len());
            if capacity < cells.len() {
                return Ok(());
            }
            for (k, c) in cells.iter().enumerate() {
                out.add(k).write(SkyCellRecord {
                    direction: c.cell.direction,
                    moment: c.cell.moment,
                    solid_angle: c.cell.solid_angle,
                    patch: u32::try_from(c.patch).map_err(|_| Failure::Internal)?,
                    reserved: 0,
                });
            }
            Ok(())
        })
    }
}

/// What a run is built from.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClimateCreateRequest {
    /// Unit normals, model frame, three per sample.
    pub normals: SliceView<f64>,
    /// Per sample then per patch: the share seen open, 0–255.
    pub open: SliceView<u8>,
    /// The same layout: the share seen through canopy only; may be empty.
    pub canopy: SliceView<u8>,
    /// Canopy transmittance per month, January first, 0–1.
    pub canopy_transmittance: [f64; 12],
    /// Dome rotation, radians (as `tapioca_sunstudy_sky_patches`).
    pub rotation: f64,
    /// 0 Tregenza, 1 Reinhart.
    pub patch_set: u8,
    /// 0 Perez 1993, 1 isotropic.
    pub sky: u8,
    /// Illuminance source (#155 P4): 0 the file's fields when valid, else the
    /// luminous efficacy; 1 the efficacy always.
    pub lux_source: u8,
    /// Zero.
    pub reserved: [u8; 5],
    /// Exterior illuminance threshold, lux; 0 or less skips the illuminance.
    pub lux_threshold: f64,
}

/// One weather interval on the project's clock.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ClimateIntervalRecord {
    /// Unit vector to the sun at the interval's midpoint, model frame.
    pub sun: [f64; 3],
    /// Interval length, hours.
    pub hours: f64,
    /// Mean DNI, W/m²; NaN missing.
    pub dni: f64,
    /// Mean DHI, W/m²; NaN missing.
    pub dhi: f64,
    /// Ground albedo; outside (0, 1] uses the default 0.2.
    pub albedo: f64,
    /// Dew point, °C; NaN missing.
    pub dew_point_c: f64,
    /// The file's mean GHI, W/m²; NaN missing. Checked only, never summed.
    pub ghi: f64,
    /// The file's mean global horizontal, direct normal and diffuse horizontal
    /// illuminance, lux; NaN missing.
    pub file_lux: [f64; 3],
    /// 1–366.
    pub day_of_year: u32,
    /// 0 = January.
    pub month: u8,
    /// Zero.
    pub reserved: [u8; 3],
}

/// What the run did.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ClimateReportRecord {
    /// Hours with usable radiation per month.
    pub valid_hours: [f64; 12],
    /// Hours with missing radiation per month.
    pub missing_hours: [f64; 12],
    /// Daylight hours whose illuminance came from the file, per month ...
    pub lux_file_hours: [f64; 12],
    /// ... from the luminous efficacy ...
    pub lux_efficacy_hours: [f64; 12],
    /// ... or could not be had (missing radiation, nothing added).
    pub lux_unavailable_hours: [f64; 12],
    /// The illuminance threshold, lux (0 when the illuminance was skipped).
    pub lux_threshold: f64,
    /// Σ(DNI·sin α + DHI)·Δt per month, Wh/m².
    pub open_horizontal: [f64; 12],
    /// Over intervals with DNI, DHI and GHI present: the model's total ...
    pub compared_model: [f64; 12],
    /// ... the file's Σ GHI·Δt ...
    pub compared_file: [f64; 12],
    /// ... and their hours, per month.
    pub compared_hours: [f64; 12],
    /// `horizontal_closure`: (model − file) / file per month, NaN where a
    /// month does not compare ...
    pub closure_monthly: [f64; 12],
    /// ... over the year (NaN when no month compares) ...
    pub closure_annual: f64,
    /// ... the tolerance the year is judged against ...
    pub closure_tolerance: f64,
    /// ... and the hours compared.
    pub closure_hours: f64,
    /// DNI energy lost with the sun below the horizon, Wh/m².
    pub beam_below_horizon: f64,
    /// Largest negative-radiance share clamped in one interval.
    pub max_clamped_share: f64,
    /// Intervals whose sky fell back to isotropic.
    pub isotropic_intervals: u32,
    /// Intervals whose Perez parameters were held.
    pub held_intervals: u32,
    /// The month (0 = January) furthest from the file, −1 when none compares.
    pub closure_worst_month: i8,
    /// 1 when the year is within the tolerance.
    pub closure_within: u8,
    /// 1 when the run kept the illuminance.
    pub lux_kept: u8,
    /// Zero.
    pub reserved: [u8; 5],
    /// Samples.
    pub sample_count: usize,
    /// Patches per sample.
    pub patch_count: usize,
}

/// Where `read` writes. Each array has room for `capacity` samples: 12 values
/// a sample (January first) for the three components, one for `sky_view`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClimateReadOutput {
    /// Beam, Wh/m².
    pub direct: *mut f64,
    /// Sky, Wh/m².
    pub diffuse: *mut f64,
    /// Ground-reflected, Wh/m².
    pub reflected: *mut f64,
    /// Geometric sky view factor through the open shares, 0–1.
    pub sky_view: *mut f64,
    /// Lux-hours per sample and month; null skips it.
    pub lux_hours: *mut f64,
    /// Hours at or above the threshold per sample and month; null skips it.
    pub hours_above: *mut f64,
    /// Samples the arrays hold.
    pub capacity: usize,
    /// The report.
    pub report: *mut ClimateReportRecord,
}

/// Opaque climate run.
pub struct ClimateHandle {
    climate: Climate,
}

impl ClimateHandle {
    /// The run, for the other exports of this surface.
    pub(crate) fn climate(&self) -> &Climate {
        &self.climate
    }
}

fn refused(error: ClimateRefusal) -> Failure {
    match error {
        ClimateRefusal::Length => Failure::Argument("ClimateLength"),
        ClimateRefusal::Interval => Failure::Argument("ClimateInterval"),
        ClimateRefusal::Beam => Failure::Argument("ClimateBeamMissing"),
    }
}

/// SAFETY: `handle` null or live, not destroyed during the call.
unsafe fn climate<'a>(handle: *const ClimateHandle) -> Result<&'a Climate, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }
        .map(|h| &h.climate)
        .ok_or(Failure::Argument("ClimateHandle: null"))
}

/// SAFETY: as [`climate`], and no other thread may use the handle.
unsafe fn climate_mut<'a>(handle: *mut ClimateHandle) -> Result<&'a mut Climate, Failure> {
    memory_range(handle.cast_const(), 1)?;
    // SAFETY: non-null and aligned (checked); exclusivity is the caller's contract.
    unsafe { handle.as_mut() }
        .map(|h| &mut h.climate)
        .ok_or(Failure::Argument("ClimateHandle: null"))
}

/// Build the setup from the request's (already borrowed) arrays.
fn setup(
    request: &ClimateCreateRequest,
    normals: &[f64],
    open: &[u8],
    canopy: &[u8],
) -> Result<ClimateSetup, Failure> {
    let dome = dome(request.patch_set, request.rotation)?;
    let sky = match request.sky {
        0 => DiffuseSky::Perez1993,
        1 => DiffuseSky::Isotropic,
        _ => return Err(Failure::Argument("ClimateSky: 0 Perez 1993, 1 isotropic")),
    };
    if !normals.len().is_multiple_of(3) {
        return Err(Failure::Argument("ClimateNormals: three per sample"));
    }
    let samples = normals.len() / 3;
    let patches = dome.len();
    if open.len() != samples * patches || !(canopy.is_empty() || canopy.len() == open.len()) {
        return Err(Failure::Argument(
            "ClimateMasks: one share per sample and patch",
        ));
    }
    if request
        .canopy_transmittance
        .iter()
        .any(|t| !(0.0..=1.0).contains(t))
    {
        return Err(Failure::Argument(
            "ClimateCanopy: transmittance outside 0-1",
        ));
    }
    let mut masks = SkyMasks::new(samples, patches);
    let width = patches.max(1);
    for (s, row) in open.chunks_exact(width).enumerate() {
        let through = canopy.get(s * width..(s + 1) * width).unwrap_or(&[]);
        masks
            .set(s, row, through)
            .map_err(|_| Failure::Argument("ClimateMasks: open and canopy exceed a patch"))?;
    }
    Ok(ClimateSetup {
        dome,
        normals: normals.as_chunks::<3>().0.to_vec(),
        masks,
        sky,
        canopy_transmittance: request.canopy_transmittance,
        lux: lux_options(request)?,
    })
}

/// The request's illuminance settings; `None` below a positive threshold.
fn lux_options(request: &ClimateCreateRequest) -> Result<Option<LuxOptions>, Failure> {
    let source = match request.lux_source {
        0 => LuxSource::FileWhenValid,
        1 => LuxSource::EfficacyOnly,
        _ => {
            return Err(Failure::Argument(
                "ClimateLuxSource: 0 file when valid, 1 efficacy",
            ));
        }
    };
    if request.lux_threshold.is_nan() || request.lux_threshold <= 0.0 {
        return Ok(None);
    }
    Ok(Some(LuxOptions {
        threshold_lux: request.lux_threshold,
        source,
    }))
}

/// Start a run.
///
/// # Safety
/// `request` and its arrays readable; `out` one writable slot. Release with
/// `tapioca_sunstudy_climate_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_climate_create(
    request: *const ClimateCreateRequest,
    out: *mut *mut ClimateHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.normals.data, request.normals.count);
            regions.input(request.open.data, request.open.count);
            regions.input(request.canopy.data, request.canopy.count);
            regions.check()?;
            let context = regions.context();
            let normals = context.slice(request.normals)?;
            let open = context.slice(request.open)?;
            let canopy = context.slice(request.canopy)?;
            let climate = Climate::new(setup(&request, normals, open, canopy)?).map_err(refused)?;
            out.write(Box::into_raw(Box::new(ClimateHandle { climate })));
            Ok(())
        })
    }
}

/// Release a run. Null is accepted.
///
/// # Safety
/// `handle` null or a live handle no thread is using, never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_climate_destroy(
    handle: *mut ClimateHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; ownership returns to Rust once.
    unsafe {
        call(error, error_capacity, || {
            if !handle.is_null() {
                memory_range(handle.cast_const(), 1)?;
                // SAFETY: the caller transfers the live Box allocation back.
                drop(Box::from_raw(handle));
            }
            Ok(())
        })
    }
}

/// Add one interval. `beam` holds one path value per sample (0 blocked,
/// 1 canopy, 2 open); it may be empty when the sun is down or DNI is 0.
///
/// # Safety
/// `handle` live and used by no other thread; `interval` and `beam` readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_climate_add(
    handle: *mut ClimateHandle,
    interval: *const ClimateIntervalRecord,
    beam: SliceView<u8>,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(interval, 1);
            regions.input(beam.data, beam.count);
            regions.check()?;
            let context = regions.context();
            let r = context.record(interval)?;
            let beam = context.slice(beam)?;
            let climate = climate_mut(handle)?;
            climate
                .add(
                    &ClimateInterval {
                        month: r.month,
                        hours: r.hours,
                        day_of_year: r.day_of_year,
                        sun: r.sun,
                        dni: r.dni,
                        dhi: r.dhi,
                        albedo: r.albedo,
                        dew_point_c: r.dew_point_c,
                        ghi: r.ghi,
                        file_lux: r.file_lux,
                    },
                    (!beam.is_empty()).then_some(beam),
                )
                .map_err(refused)
        })
    }
}

/// The totals so far, the sky view factors and the report. With a capacity
/// below the sample count only the report is written.
///
/// # Safety
/// `handle` live; `output` readable and its regions writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_climate_read(
    handle: *const ClimateHandle,
    output: *const ClimateReadOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.check()?;
            let o = regions.context().record(output)?;
            let monthly = o
                .capacity
                .checked_mul(12)
                .ok_or(Failure::Argument("ClimateCapacity"))?;
            regions.output(o.report, 1);
            regions.output(o.direct, monthly);
            regions.output(o.diffuse, monthly);
            regions.output(o.reflected, monthly);
            regions.output(o.sky_view, o.capacity);
            if !o.lux_hours.is_null() {
                regions.output(o.lux_hours, monthly);
            }
            if !o.hours_above.is_null() {
                regions.output(o.hours_above, monthly);
            }
            regions.check()?;
            let climate = climate(handle)?;
            let totals = climate.totals();
            let r = &totals.report;
            let closure = horizontal_closure(r);
            let lux_report = totals
                .lux
                .as_ref()
                .map_or_else(Default::default, |l| l.report.clone());
            o.report.write(ClimateReportRecord {
                lux_file_hours: lux_report.file_hours,
                lux_efficacy_hours: lux_report.efficacy_hours,
                lux_unavailable_hours: lux_report.unavailable_hours,
                lux_threshold: lux_report.threshold_lux,
                lux_kept: u8::from(totals.lux.is_some()),
                valid_hours: r.valid_hours,
                missing_hours: r.missing_hours,
                open_horizontal: r.open_horizontal,
                compared_model: r.compared_model,
                compared_file: r.compared_file,
                compared_hours: r.compared_hours,
                closure_monthly: closure.monthly,
                closure_annual: closure.annual,
                closure_tolerance: FILE_CONSISTENCY_TOLERANCE,
                closure_hours: closure.hours,
                closure_worst_month: closure
                    .worst_month
                    .and_then(|m| i8::try_from(m).ok())
                    .unwrap_or(-1),
                closure_within: u8::from(closure.within),
                reserved: [0; 5],
                beam_below_horizon: r.beam_below_horizon,
                max_clamped_share: r.max_clamped_share,
                isotropic_intervals: r.isotropic_intervals,
                held_intervals: r.held_intervals,
                sample_count: climate.sample_count(),
                patch_count: climate.patch_count(),
            });
            if o.capacity < climate.sample_count() {
                return Ok(());
            }
            for (target, rows) in [
                (o.direct, &totals.direct),
                (o.diffuse, &totals.diffuse),
                (o.reflected, &totals.reflected),
            ] {
                for (s, row) in rows.iter().enumerate() {
                    std::ptr::copy_nonoverlapping(row.as_ptr(), target.add(s * 12), 12);
                }
            }
            if let Some(lux) = &totals.lux {
                for (target, rows) in [
                    (o.lux_hours, &lux.lux_hours),
                    (o.hours_above, &lux.hours_above),
                ] {
                    if target.is_null() {
                        continue;
                    }
                    for (s, row) in rows.iter().enumerate() {
                        std::ptr::copy_nonoverlapping(row.as_ptr(), target.add(s * 12), 12);
                    }
                }
            }
            let svf = climate.sky_view_factors();
            std::ptr::copy_nonoverlapping(svf.as_ptr(), o.sky_view, svf.len());
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
