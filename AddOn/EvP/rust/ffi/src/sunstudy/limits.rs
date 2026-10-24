//! `tapioca_sunstudy_limits`: the machine's study ceiling.

use super::{Regions, flag};
use crate::boundary::call;
use tapioca_sunstudy::limits;

/// What the machine offers and what the study asks for.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LimitsRequest {
    /// Dedicated video memory, bytes (ignored unless `gpu_known`).
    pub gpu_memory_bytes: u64,
    /// Free physical memory, bytes; 0 = unknown.
    pub available_ram_bytes: u64,
    /// Timesteps per study.
    pub steps: usize,
    /// Atlas occupancy, clamped to 0.01..=1.
    pub occupancy: f64,
    /// Area to analyse, square metres, for the finest-grid answer.
    pub analysed_area: f64,
    /// Device 2D texture limit (ignored unless `gpu_known`).
    pub max_texture_dimension: u32,
    /// 1 when the viewer has published GPU facts, else 0.
    pub gpu_known: u8,
}

/// The binding ceiling, the breakdown and the byte costs behind it.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LimitsOutput {
    /// Largest allowed sample count.
    pub max_samples: usize,
    /// Sampler, system-memory, GPU-memory and texture ceilings (0 = unknown).
    pub caps: [usize; 4],
    /// Samples x steps at the ceiling.
    pub max_rays: u64,
    /// System bytes per sample at this step count and occupancy.
    pub system_bytes_per_sample: f64,
    /// GPU bytes per atlas texel at this step count.
    pub gpu_bytes_per_texel: f64,
    /// Finest spacing (metres) that keeps `analysed_area` within the ceiling.
    pub finest_grid_spacing: f64,
    /// Atlas packer ceiling for this machine, texels per side.
    pub max_atlas_dimension: u32,
    /// 0 sampler, 1 system memory, 2 GPU memory, 3 texture size.
    pub binding: u8,
}

/// Compute the study ceiling for one machine.
///
/// # Safety
/// `request` must be live initialized immutable storage; `output` must be live,
/// aligned, exclusively writable and disjoint from the request and error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_limits(
    request: *const LimitsRequest,
    output: *mut LimitsOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: the envelope contains unwinding; regions are checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(output, 1);
            regions.check()?;
            // SAFETY: shape and disjointness checked; the record is copied.
            let request = regions.context().record(request)?;
            let machine = limits::MachineResources {
                gpu_known: flag(request.gpu_known, "LimitsGpuKnown: expected 0 or 1")?,
                gpu_memory_bytes: request.gpu_memory_bytes,
                max_texture_dimension: request.max_texture_dimension,
                available_ram_bytes: request.available_ram_bytes,
            };
            let result =
                limits::compute_analysis_limits(&machine, request.steps, request.occupancy);
            let occupancy = request.occupancy.clamp(0.01, 1.0);
            let out = LimitsOutput {
                max_samples: result.max_samples,
                caps: [
                    result.sampler_cap,
                    result.ram_cap,
                    result.gpu_cap,
                    result.texture_cap,
                ],
                max_rays: result.max_rays,
                system_bytes_per_sample: limits::system_bytes_per_sample(request.steps, occupancy),
                gpu_bytes_per_texel: limits::gpu_bytes_per_texel(request.steps),
                finest_grid_spacing: limits::finest_grid_spacing(
                    request.analysed_area,
                    result.max_samples,
                ),
                max_atlas_dimension: result.max_atlas_dimension,
                binding: result.binding as u8,
            };
            // SAFETY: checked aligned exclusive output.
            output.write(out);
            Ok(())
        })
    }
}
