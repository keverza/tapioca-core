//! Mirrors `tests/cpp/test_sunstudylimits.cpp`.

use super::*;

fn gpu(memory: u64, texture: u32) -> MachineResources {
    MachineResources {
        gpu_known: true,
        gpu_memory_bytes: memory,
        max_texture_dimension: texture,
        available_ram_bytes: 0,
    }
}

#[test]
fn with_nothing_known_the_samplers_cap_binds() {
    let limits = compute_analysis_limits(&MachineResources::default(), 49, PATCH_ATLAS_OCCUPANCY);
    assert_eq!(limits.binding, LimitBinding::SamplerCap);
    assert_eq!(limits.max_samples, SAMPLER_MAX_SAMPLES);
    assert_eq!(limits.max_rays, 4_000_000 * 49);
    assert_eq!(limits.max_atlas_dimension, ATLAS_PACKER_MAX_DIMENSION);
}

#[test]
fn a_little_gpu_memory_binds() {
    let limits = compute_analysis_limits(&gpu(128 << 20, 16384), 49, PATCH_ATLAS_OCCUPANCY);
    assert_eq!(limits.binding, LimitBinding::GpuMemory);
    let expected = f64::from(32_u32 << 20) / gpu_bytes_per_texel(49) * PATCH_ATLAS_OCCUPANCY;
    assert!((count_to_f64(limits.max_samples) - expected).abs() <= 1.0);
}

#[test]
fn a_little_system_memory_binds() {
    let machine = MachineResources {
        available_ram_bytes: 512 << 20,
        ..MachineResources::default()
    };
    let limits = compute_analysis_limits(&machine, 49, PATCH_ATLAS_OCCUPANCY);
    assert_eq!(limits.binding, LimitBinding::SystemMemory);
    assert!(limits.max_samples < SAMPLER_MAX_SAMPLES);
}

#[test]
fn a_small_texture_ceiling_binds_and_caps_the_atlas() {
    let limits = compute_analysis_limits(&gpu(24 << 30, 2048), 49, PATCH_ATLAS_OCCUPANCY);
    assert_eq!(limits.binding, LimitBinding::TextureSize);
    assert_eq!(limits.max_atlas_dimension, 2048);
}

#[test]
fn the_triangle_domain_affords_fewer_samples_than_the_patch_domain() {
    let machine = gpu(256 << 20, 16384);
    assert!(
        compute_analysis_limits(&machine, 49, TRIANGLE_ATLAS_OCCUPANCY).max_samples
            < compute_analysis_limits(&machine, 49, PATCH_ATLAS_OCCUPANCY).max_samples
    );
}

#[test]
fn the_finest_grid_is_one_sample_per_spacing_squared() {
    assert!((finest_grid_spacing(10000.0, 1_000_000) - 0.1).abs() < 1e-12);
    assert!(finest_grid_spacing(0.0, 1000).abs() < f64::EPSILON);
    assert!(finest_grid_spacing(100.0, 0).abs() < f64::EPSILON);
}

#[test]
fn binding_names_match_the_hud() {
    assert_eq!(LimitBinding::SamplerCap.name(), "the sampler's cap");
    assert_eq!(LimitBinding::SystemMemory.name(), "system memory");
    assert_eq!(LimitBinding::GpuMemory.name(), "GPU memory");
    assert_eq!(LimitBinding::TextureSize.name(), "GPU texture size");
}

#[test]
fn byte_costs_follow_the_held_buffers() {
    // 49 steps: two 32-bit slices, one 64-bit word.
    assert!((gpu_bytes_per_texel(49) - 12.0).abs() < 1e-12);
    assert!((gpu_bytes_per_texel(0) - 8.0).abs() < 1e-12);
    let expected = 48.0 + 68.0 + 8.0 + 8.0 + (1.0 / 0.5) * 12.0;
    assert!((system_bytes_per_sample(49, 0.5) - expected).abs() < 1e-12);
}
