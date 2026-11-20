//! Small kernels the host calls on its own (P7b): a day's timesteps for the
//! capture, the fan of steps the HUD draws from the steps' minutes, and
//! per-sample values scattered into an atlas image for a display.

use super::Regions;
use super::atlas::publish;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::atlas::{SunStudyAtlas, scatter_to_atlas};
use tapioca_sunstudy::series::enumerate_timesteps;
use tapioca_sunstudy::step_atlas;

/// `EnumerateTimesteps`: `[hour, minute]` per step of the window.
///
/// # Safety
/// `times` writable for `capacity` pairs and `written` for one count, both
/// disjoint from each other and from the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_enumerate_timesteps(
    timestep_minutes: i32,
    hour_from: i32,
    hour_to: i32,
    times: *mut [i32; 2],
    capacity: usize,
    written: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let found = enumerate_timesteps(timestep_minutes, hour_from, hour_to);
            if capacity < found.len() {
                return Err(Failure::Argument("TimestepsOutputTooSmall"));
            }
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(times, found.len());
            regions.output(written, 1);
            regions.check()?;
            let pairs: Vec<[i32; 2]> = found.iter().map(|t| [t.hour, t.minute]).collect();
            publish(&pairs, times);
            written.write(pairs.len());
            Ok(())
        })
    }
}

/// `FanSteps` over the steps' minutes of day; at most one entry per step.
///
/// # Safety
/// `minutes` readable for `count`; `fan` writable for `capacity` and `written`
/// for one count, all disjoint and apart from the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_fan_steps(
    minutes: *const u16,
    count: usize,
    interval_minutes: f64,
    fan: *mut u32,
    capacity: usize,
    written: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(minutes, count);
            regions.output(fan, capacity);
            regions.output(written, 1);
            regions.check()?;
            let context = regions.context();
            let minutes = context.slice(SliceView {
                data: minutes,
                count,
            })?;
            let steps = step_atlas::fan_steps(minutes, interval_minutes);
            if capacity < steps.len() {
                return Err(Failure::Argument("FanOutputTooSmall"));
            }
            publish(&steps, fan);
            written.write(steps.len());
            Ok(())
        })
    }
}

/// `ScatterToAtlas` inputs: the atlas' texel per sample and its size.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ScatterRequest {
    /// Texel index per sample.
    pub texels: SliceView<u32>,
    /// Value per sample.
    pub values: SliceView<f64>,
    /// Atlas texels across.
    pub width: u32,
    /// Atlas texels down.
    pub height: u32,
    /// What an untouched texel holds.
    pub empty_value: f32,
    /// 0 for a refused atlas (nothing is written).
    pub valid: u8,
}

/// `ScatterToAtlas`: `written` is `width * height`, or 0 for a refused or
/// empty atlas.
///
/// # Safety
/// The request and its arrays readable; `image` writable for `capacity` and
/// `written` for one count, all disjoint and apart from the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_scatter_to_atlas(
    request: *const ScatterRequest,
    image: *mut f32,
    capacity: usize,
    written: *mut usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(written, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.texels.data, request.texels.count);
            regions.input(request.values.data, request.values.count);
            regions.output(image, capacity);
            regions.check()?;
            let context = regions.context();
            let atlas = SunStudyAtlas {
                width: request.width,
                height: request.height,
                tiles: Vec::new(),
                texels: context.slice(request.texels)?.to_vec(),
                placed_faces: 0,
                valid: super::flag(request.valid, "ScatterRequest: valid")?,
            };
            let scattered =
                scatter_to_atlas(&atlas, context.slice(request.values)?, request.empty_value);
            if capacity < scattered.len() {
                return Err(Failure::Argument("ScatterOutputTooSmall"));
            }
            publish(&scattered, image);
            written.write(scattered.len());
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn message(buffer: &[u8]) -> String {
        let end = buffer.iter().position(|b| *b == 0).unwrap_or(buffer.len());
        String::from_utf8_lossy(buffer.get(..end).unwrap_or_default()).into_owned()
    }

    #[test]
    fn timesteps_fan_and_scatter_cross_the_boundary() {
        let mut error = [0_u8; 256];
        let mut times = [[0_i32; 2]; 8];
        let mut written = 0_usize;
        // SAFETY: every buffer is a live local of the stated length.
        let status = unsafe {
            tapioca_sunstudy_enumerate_timesteps(
                60,
                10,
                14,
                times.as_mut_ptr(),
                times.len(),
                &raw mut written,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 0, "{}", message(&error));
        assert_eq!(
            times.get(..written),
            Some(&[[10, 0], [11, 0], [12, 0], [13, 0]][..])
        );

        let minutes = [600_u16, 615, 630, 645, 660];
        let mut fan = [0_u32; 5];
        // SAFETY: as above.
        let status = unsafe {
            tapioca_sunstudy_fan_steps(
                minutes.as_ptr(),
                minutes.len(),
                30.0,
                fan.as_mut_ptr(),
                fan.len(),
                &raw mut written,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 0, "{}", message(&error));
        assert_eq!(
            fan.get(..written),
            Some(step_atlas::fan_steps(&minutes, 30.0).as_slice())
        );

        let texels = [3_u32, 0, 9];
        let values = [1.5_f64, 2.5];
        let request = ScatterRequest {
            texels: SliceView {
                data: texels.as_ptr(),
                count: texels.len(),
            },
            values: SliceView {
                data: values.as_ptr(),
                count: values.len(),
            },
            width: 2,
            height: 2,
            empty_value: -1.0,
            valid: 1,
        };
        let mut image = [0.0_f32; 4];
        // SAFETY: as above.
        let status = unsafe {
            tapioca_sunstudy_scatter_to_atlas(
                &raw const request,
                image.as_mut_ptr(),
                image.len(),
                &raw mut written,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, 0, "{}", message(&error));
        assert_eq!(written, 4);
        assert_eq!(
            image.map(f32::to_bits),
            [2.5_f32, -1.0, -1.0, 1.5].map(f32::to_bits)
        );
    }
}
