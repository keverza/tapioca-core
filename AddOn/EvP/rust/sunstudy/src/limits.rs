//! How big a study a machine can run and show, as numbers the start command
//! enforces: refused in advance, never truncated.
//!
//! Portable half of `SunStudy/SunStudyLimits.cpp`. The operating-system memory
//! query and the viewer's published GPU facts stay in C++ and arrive here as a
//! [`MachineResources`] value. Four ceilings, and the smallest binds: the
//! sampler's cap, system memory, GPU memory and the largest texture.

use crate::series::count_to_f64;

/// The sampler's fixed ceiling on a study's sample count.
pub const SAMPLER_MAX_SAMPLES: usize = 4_000_000;
/// The atlas packer's texture ceiling.
pub const ATLAS_PACKER_MAX_DIMENSION: u32 = 8192;
/// Measured atlas occupancy for one tile per surface.
pub const PATCH_ATLAS_OCCUPANCY: f64 = 0.70;
/// Measured atlas occupancy for one tile per triangle.
pub const TRIANGLE_ATLAS_OCCUPANCY: f64 = 0.15;
/// Fraction of free system memory a study may take.
pub const RAM_BUDGET_FRACTION: f64 = 0.50;
/// Fraction of dedicated GPU memory a study may take.
pub const GPU_BUDGET_FRACTION: f64 = 0.25;

/// What the machine offers. GPU facts are unknown until the viewer's device
/// exists, and unknown facts do not bind.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MachineResources {
    /// Whether the GPU fields below are known.
    pub gpu_known: bool,
    /// Dedicated video memory, bytes.
    pub gpu_memory_bytes: u64,
    /// The device's 2D texture limit, texels per side.
    pub max_texture_dimension: u32,
    /// Free physical memory right now, bytes; 0 = unknown.
    pub available_ram_bytes: u64,
}

/// Which ceiling bound the study. Discriminants are the C ABI values.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum LimitBinding {
    /// The sampler's fixed cap.
    #[default]
    SamplerCap = 0,
    /// System memory.
    SystemMemory = 1,
    /// GPU memory.
    GpuMemory = 2,
    /// GPU texture size.
    TextureSize = 3,
}

impl LimitBinding {
    /// Human-readable name, as the HUD prints it.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Self::SystemMemory => "system memory",
            Self::GpuMemory => "GPU memory",
            Self::TextureSize => "GPU texture size",
            Self::SamplerCap => "the sampler's cap",
        }
    }
}

/// The binding ceiling and the per-limit breakdown. Caps of 0 are unknown.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AnalysisLimits {
    /// Largest sample count allowed.
    pub max_samples: usize,
    /// The atlas packer's ceiling for this machine: `min(device, 8192)`.
    pub max_atlas_dimension: u32,
    /// Which ceiling bound `max_samples`.
    pub binding: LimitBinding,
    /// Rays one study casts at the ceiling: samples x steps.
    pub max_rays: u64,
    /// Sampler ceiling.
    pub sampler_cap: usize,
    /// System-memory ceiling.
    pub ram_cap: usize,
    /// GPU-memory ceiling.
    pub gpu_cap: usize,
    /// Texture-size ceiling.
    pub texture_cap: usize,
}

fn words32(steps: usize) -> usize {
    steps.div_ceil(32).max(1)
}

/// Bytes one atlas texel costs on the GPU: R32 hours plus one R32 slice per 32
/// steps.
#[must_use]
pub fn gpu_bytes_per_texel(steps: usize) -> f64 {
    4.0 + 4.0 * count_to_f64(words32(steps))
}

/// Bytes one sample costs in system memory at `steps`, display copy included:
/// positions and normals (48), grid copy (68), hours (8), lit bits in 64-bit
/// words, and the atlas images at `occupancy` (floored at 0.01).
#[must_use]
pub fn system_bytes_per_sample(steps: usize, occupancy: f64) -> f64 {
    let bits = 8.0 * count_to_f64(steps.div_ceil(64));
    let texels = 1.0 / crate::raster::cpp_max(occupancy, 0.01);
    48.0 + 68.0 + 8.0 + bits + texels * gpu_bytes_per_texel(steps)
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "mirrors size_t(double): truncation toward zero of a non-negative budget"
)]
fn to_count(value: f64) -> usize {
    value as usize
}

#[expect(
    clippy::cast_precision_loss,
    reason = "mirrors double(uint64_t) for byte budgets"
)]
fn bytes_to_f64(value: u64) -> f64 {
    value as f64
}

/// The binding ceiling for `steps` timesteps at atlas `occupancy` (clamped to
/// `0.01..=1`).
#[must_use]
pub fn compute_analysis_limits(
    machine: &MachineResources,
    steps: usize,
    occupancy: f64,
) -> AnalysisLimits {
    let occ = occupancy.clamp(0.01, 1.0);
    let mut out = AnalysisLimits {
        sampler_cap: SAMPLER_MAX_SAMPLES,
        max_atlas_dimension: ATLAS_PACKER_MAX_DIMENSION,
        ..AnalysisLimits::default()
    };
    if machine.available_ram_bytes > 0 {
        let budget = bytes_to_f64(machine.available_ram_bytes) * RAM_BUDGET_FRACTION;
        out.ram_cap = to_count(budget / system_bytes_per_sample(steps, occ));
    }
    if machine.gpu_known {
        if machine.max_texture_dimension > 0 {
            out.max_atlas_dimension = out.max_atlas_dimension.min(machine.max_texture_dimension);
        }
        if machine.gpu_memory_bytes > 0 {
            let budget = bytes_to_f64(machine.gpu_memory_bytes) * GPU_BUDGET_FRACTION;
            out.gpu_cap = to_count(budget / gpu_bytes_per_texel(steps) * occ);
        }
    }
    let side = f64::from(out.max_atlas_dimension);
    out.texture_cap = to_count(side * side * occ);

    out.max_samples = out.sampler_cap;
    for (cap, why) in [
        (out.ram_cap, LimitBinding::SystemMemory),
        (out.gpu_cap, LimitBinding::GpuMemory),
        (out.texture_cap, LimitBinding::TextureSize),
    ] {
        if cap > 0 && cap < out.max_samples {
            out.max_samples = cap;
            out.binding = why;
        }
    }
    out.max_rays = u64::try_from(out.max_samples)
        .unwrap_or(u64::MAX)
        .wrapping_mul(u64::try_from(steps).unwrap_or(u64::MAX));
    out
}

/// The finest grid spacing (metres) at which `analysed_area` (m2) stays within
/// `max_samples`; 0 when either is non-positive.
#[must_use]
pub fn finest_grid_spacing(analysed_area: f64, max_samples: usize) -> f64 {
    if analysed_area.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) || max_samples == 0 {
        return 0.0;
    }
    (analysed_area / count_to_f64(max_samples)).sqrt()
}

#[cfg(test)]
mod tests;
