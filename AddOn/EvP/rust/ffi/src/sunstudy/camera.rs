//! Shadow-camera arithmetic, accumulator layout and the refinement ladder.

use super::Regions;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::raster;

/// One timestep's camera inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShadowCameraRequest {
    /// Model box minimum corner, metres.
    pub aabb_min: [f64; 3],
    /// Model box maximum corner, metres.
    pub aabb_max: [f64; 3],
    /// Vector pointing TO the sun; need not be unit length.
    pub sun_dir: [f64; 3],
    /// Edge padding, metres (C++ default 1).
    pub margin: f64,
    /// Shadow-map texels per side (C++ default 2048).
    pub resolution: u32,
    /// Bias floor, metres (C++ default 0.005).
    pub min_bias: f64,
}

/// The fitted frustum and everything derived from it. When `valid` is 0 (zero
/// sun or inverted box) every other field is zero.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShadowCamera {
    /// Left, right, bottom, top, near, far (metres; near may be negative).
    pub planes: [f64; 6],
    /// World width, height and depth of the frustum, metres.
    pub extent: [f64; 3],
    /// Unit right axis.
    pub right_axis: [f64; 3],
    /// Unit up axis.
    pub up_axis: [f64; 3],
    /// Unit axis from the sun toward the scene.
    pub light_axis: [f64; 3],
    /// Row-major orthographic projection.
    pub projection: [f64; 16],
    /// Row-major view-projection on unshifted positions.
    pub light_view_projection: [f64; 16],
    /// Relative-to-centre origin of the box, whole metres.
    pub rtc_origin: [f64; 3],
    /// Row-major view-projection on positions with `rtc_origin` subtracted.
    pub rtc_light_view_projection: [f64; 16],
    /// Metres per shadow-map texel.
    pub texel_world_size: f64,
    /// `max(2 x texel, min_bias)`, metres.
    pub bias: f64,
    /// `bias` in depth-buffer units.
    pub depth_bias_ndc: f64,
    /// 1 when a frustum was fitted, else 0.
    pub valid: u8,
}

fn camera(request: &ShadowCameraRequest) -> ShadowCamera {
    let Some(f) = raster::fit_ortho_frustum(
        &request.aabb_min,
        &request.aabb_max,
        &request.sun_dir,
        request.margin,
    ) else {
        return ShadowCamera::default();
    };
    let origin = raster::rtc_origin(&request.aabb_min, &request.aabb_max);
    let bias = raster::derive_bias_from_frustum(&f, request.resolution, request.min_bias);
    ShadowCamera {
        planes: [f.left, f.right, f.bottom, f.top, f.near_plane, f.far_plane],
        extent: [f.world_width, f.world_height, f.world_depth],
        right_axis: f.right_axis,
        up_axis: f.up_axis,
        light_axis: f.light_axis,
        projection: raster::ortho_projection(&f),
        light_view_projection: raster::light_view_projection(&f, None),
        rtc_origin: origin,
        rtc_light_view_projection: raster::light_view_projection(&f, Some(&origin)),
        texel_world_size: raster::texel_world_size(&f, request.resolution),
        bias,
        depth_bias_ndc: raster::depth_bias_ndc(bias, f.near_plane, f.far_plane),
        valid: 1,
    }
}

/// Read one POD request and write one POD result after checking both.
///
/// SAFETY: request must be live initialized immutable storage for one `R`;
/// output must be live aligned exclusively writable storage for one `O`.
unsafe fn single<R: Copy, O>(
    request: *const R,
    output: *mut O,
    error: *mut u8,
    error_capacity: usize,
    compute: impl FnOnce(&R) -> Result<O, Failure>,
) -> Result<(), Failure> {
    let mut regions = Regions::new(error, error_capacity)?;
    regions.input(request, 1);
    regions.output(output, 1);
    regions.check()?;
    // SAFETY: shape and disjointness checked; the record is copied by value.
    let request = unsafe { regions.context().record(request)? };
    let result = compute(&request)?;
    // SAFETY: checked aligned exclusive output for one O.
    unsafe { output.write(result) };
    Ok(())
}

/// Fit the shadow camera for one sun direction.
///
/// # Safety
/// `request` must be live initialized immutable storage; `output` must be live,
/// aligned, exclusively writable and disjoint from the request and error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_shadow_camera(
    request: *const ShadowCameraRequest,
    output: *mut ShadowCamera,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; `single` checks every region.
    unsafe {
        call(error, error_capacity, || {
            single(request, output, error, error_capacity, |r| Ok(camera(r)))
        })
    }
}

/// Accumulator texture layout; `valid` 0 when refused (too many samples or a
/// zero maximum width), with width and height 1.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AccumLayoutOutput {
    /// Texels per row.
    pub width: u32,
    /// Rows.
    pub height: u32,
    /// 1 when the layout covers every sample, else 0.
    pub valid: u8,
}

/// Compute the accumulator layout for `sample_count` samples.
///
/// # Safety
/// `output` must be live, aligned, exclusively writable and disjoint from the
/// error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_accum_layout(
    sample_count: usize,
    max_width: u32,
    output: *mut AccumLayoutOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output region is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(output, 1);
            regions.check()?;
            let result = raster::compute_accum_layout(sample_count, max_width).map_or(
                AccumLayoutOutput {
                    width: 1,
                    height: 1,
                    valid: 0,
                },
                |layout| AccumLayoutOutput {
                    width: layout.width,
                    height: layout.height,
                    valid: 1,
                },
            );
            // SAFETY: checked aligned exclusive output.
            output.write(result);
            Ok(())
        })
    }
}

/// Grid-ladder inputs (C++ defaults: thresholds 20000 and 5000 samples).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GridLadderRequest {
    /// Analysed area, square metres.
    pub area_m2: f64,
    /// Final grid spacing, metres.
    pub target_grid: f64,
    /// Estimated samples above which the 4x rung is added.
    pub coarse_threshold: f64,
    /// Estimated samples above which the 2x rung is added.
    pub mid_threshold: f64,
}

/// Coarsest-first spacings; `grids[..count]` are meaningful, the rest 0.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GridLadderOutput {
    /// Spacings in metres.
    pub grids: [f64; 3],
    /// Rungs written, 1 to 3.
    pub count: u32,
}

/// Coarse-to-fine refinement spacings ending at the target grid.
///
/// # Safety
/// Same contract as [`tapioca_sunstudy_shadow_camera`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_grid_ladder(
    request: *const GridLadderRequest,
    output: *mut GridLadderOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; `single` checks every region.
    unsafe {
        call(error, error_capacity, || {
            single(request, output, error, error_capacity, |r| {
                let ladder = raster::grid_ladder(
                    r.area_m2,
                    r.target_grid,
                    r.coarse_threshold,
                    r.mid_threshold,
                );
                let mut out = GridLadderOutput::default();
                for (slot, grid) in out.grids.iter_mut().zip(&ladder) {
                    *slot = *grid;
                }
                out.count = u32::try_from(ladder.len()).map_err(|_| Failure::Internal)?;
                Ok(out)
            })
        })
    }
}
