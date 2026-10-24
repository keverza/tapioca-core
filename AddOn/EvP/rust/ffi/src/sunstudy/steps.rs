//! Step-mask, texture-delta and step-schedule exports.

use super::Regions;
use super::atlas::publish;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::series::SunStep;
use tapioca_sunstudy::step_atlas;
use tapioca_sunstudy::texture_delta::{self, AtlasRegion};

/// Step-mask inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepMaskRequest {
    /// Sample-major LSB-first lit bits, `step_count` per sample.
    pub step_bits: SliceView<u8>,
    /// Hours-atlas texel per sample, negative for none.
    pub texel_of_sample: SliceView<i64>,
    /// Samples.
    pub sample_count: usize,
    /// Steps.
    pub step_count: usize,
    /// Atlas texels across.
    pub width: u32,
    /// Atlas texels down.
    pub height: u32,
}

/// Step-mask outputs: `words * width * height` slice-major masks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepMaskOutput {
    /// Masks.
    pub masks: *mut u32,
    /// Capacity in words.
    pub capacity: usize,
    /// Slices written (at least 1).
    pub words: *mut u32,
}

/// Pack per-step lit bits texel for texel (`PackStepMasks`).
///
/// # Safety
/// The request and its arrays must be live, initialized and immutable; the
/// outputs must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_step_masks(
    request: *const StepMaskRequest,
    output: *const StepMaskOutput,
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
            regions.input(request.step_bits.data, request.step_bits.count);
            regions.input(request.texel_of_sample.data, request.texel_of_sample.count);
            regions.output(target.masks, target.capacity);
            regions.output(target.words, 1);
            regions.check()?;
            let needed = request
                .sample_count
                .checked_mul(request.step_count)
                .map(|bits| bits.div_ceil(8))
                .ok_or(Failure::Argument("StepMaskTooLarge"))?;
            if request.step_bits.count < needed {
                return Err(Failure::Argument("StepMaskBitsTooShort"));
            }
            let context = regions.context();
            let atlas = step_atlas::pack_step_masks(
                context.slice(request.step_bits)?,
                request.sample_count,
                request.step_count,
                context.slice(request.texel_of_sample)?,
                (request.width, request.height),
            );
            if target.capacity < atlas.masks.len() {
                return Err(Failure::Argument("StepMaskOutputTooSmall"));
            }
            publish(&atlas.masks, target.masks);
            target.words.write(atlas.words);
            Ok(())
        })
    }
}

/// Texture-delta inputs. `kind` 0 compares f32 texels, 1 compares u32.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TextureDeltaRequest {
    /// Image before, as raw 32-bit texels.
    pub before: SliceView<u32>,
    /// Image after.
    pub after: SliceView<u32>,
    /// Texels across, down, and layers.
    pub size: [u32; 3],
    /// Regions per layer before a full-plane collapse (C++ default 512).
    pub max_regions_per_layer: usize,
}

/// Texture-delta outputs. Capacity `layers * max(1, max_regions_per_layer)`
/// always suffices.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TextureDeltaOutput {
    /// Regions.
    pub regions: *mut AtlasRegion,
    /// Capacity in regions.
    pub capacity: usize,
    /// Regions written (0 for "nothing changed" or "full upload required").
    pub count: *mut usize,
}

/// Changed regions between two images (`AtlasChangedRegions`). Both f32 and
/// u32 images compare their bytes, so one 32-bit entry point serves both.
///
/// # Safety
/// The request and its arrays must be live, initialized and immutable; the
/// outputs must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_texture_delta(
    request: *const TextureDeltaRequest,
    output: *const TextureDeltaOutput,
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
            regions.input(request.before.data, request.before.count);
            regions.input(request.after.data, request.after.count);
            regions.output(target.regions, target.capacity);
            regions.output(target.count, 1);
            regions.check()?;
            let context = regions.context();
            let [width, height, layers] = request.size;
            let found = texture_delta::atlas_changed_regions(
                (width, height, layers),
                context.slice(request.before)?,
                context.slice(request.after)?,
                request.max_regions_per_layer,
            );
            if target.capacity < found.len() {
                return Err(Failure::Argument("TextureDeltaOutputTooSmall"));
            }
            publish(&found, target.regions);
            target.count.write(found.len());
            Ok(())
        })
    }
}

/// Step-schedule inputs: a filtered series' steps and the fan interval.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepScheduleRequest {
    /// Kept steps (as `tapioca_sunstudy_series` returns them).
    pub steps: SliceView<SunStep>,
    /// Fan interval, minutes (<= 0 keeps every step).
    pub fan_interval_minutes: f64,
}

/// Step-schedule outputs; both arrays hold one entry per step.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepScheduleOutput {
    /// Minutes of day per step.
    pub minutes: *mut u16,
    /// Fan step indices; `fan_count` written.
    pub fan: *mut u32,
    /// Capacity of both arrays.
    pub capacity: usize,
    /// Fan entries written.
    pub fan_count: *mut usize,
    /// The highest-sun step.
    pub solar_noon: *mut u32,
}

/// Minutes, solar noon and fan steps for a series (`StepMinutes`,
/// `SolarNoonStep`, `FanSteps`).
///
/// # Safety
/// The request and its steps must be live, initialized and immutable; every
/// output must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_step_schedule(
    request: *const StepScheduleRequest,
    output: *const StepScheduleOutput,
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
            regions.input(request.steps.data, request.steps.count);
            regions.output(target.minutes, target.capacity);
            regions.output(target.fan, target.capacity);
            regions.output(target.fan_count, 1);
            regions.output(target.solar_noon, 1);
            regions.check()?;
            let context = regions.context();
            let steps = context.slice(request.steps)?;
            if target.capacity < steps.len() {
                return Err(Failure::Argument("StepScheduleOutputTooSmall"));
            }
            let minutes = step_atlas::step_minutes(steps);
            let fan = step_atlas::fan_steps(&minutes, request.fan_interval_minutes);
            publish(&minutes, target.minutes);
            publish(&fan, target.fan);
            target.fan_count.write(fan.len());
            target.solar_noon.write(step_atlas::solar_noon_step(steps));
            Ok(())
        })
    }
}
