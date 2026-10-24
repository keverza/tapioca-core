//! `tapioca_sunstudy_ground_grid`: shape query, then fill.

use super::Regions;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::raster;

/// The padded ground grid's inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GroundGridRequest {
    /// Model box minimum corner, metres.
    pub aabb_min: [f64; 3],
    /// Model box maximum corner, metres.
    pub aabb_max: [f64; 3],
    /// Sample spacing, metres.
    pub spacing: f64,
    /// Pad beyond the box, metres; negative selects the automatic pad.
    pub pad: f64,
    /// Lift off the ground, metres.
    pub z_offset: f64,
    /// Refusal ceiling on the sample count.
    pub max_samples: usize,
}

/// The grid's shape. When `valid` is 0 (refused) every other field is zero.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GroundGridSummary {
    /// Pad applied, metres.
    pub pad: f64,
    /// Sample height, metres.
    pub ground_z: f64,
    /// Samples along x.
    pub columns: usize,
    /// Samples along y.
    pub rows: usize,
    /// `columns * rows`; the position array needs three times this.
    pub sample_count: usize,
    /// 1 when the grid was built, else 0.
    pub valid: u8,
}

/// Caller-owned outputs, unchanged on refusal.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GroundGridOutput {
    /// Row-major xyz positions. Null with zero capacity asks for the shape only.
    pub positions: *mut f64,
    /// Capacity in scalars; when nonzero it must hold `3 * sample_count`.
    pub scalar_capacity: usize,
    /// Required summary record. Normals are all +Z and are not transported.
    pub summary: *mut GroundGridSummary,
}

/// Describe and optionally fill the regular ground grid around the model.
///
/// # Safety
/// The request and output descriptor must be live, initialized and immutable.
/// The position capacity and summary must be live, aligned, exclusively writable
/// and disjoint from each other, the descriptors and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_ground_grid(
    request: *const GroundGridRequest,
    output: *const GroundGridOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: the envelope contains unwinding; every region is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let context = regions.context();
            // SAFETY: descriptor shapes and error disjointness checked; POD copies.
            let (request, target) = (context.record(request)?, context.record(output)?);
            regions.output(target.positions, target.scalar_capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let grid = raster::make_ground_sample_grid(&raster::GroundGridRequest {
                aabb_min: request.aabb_min,
                aabb_max: request.aabb_max,
                spacing: request.spacing,
                pad: request.pad,
                z_offset: request.z_offset,
                max_samples: request.max_samples,
            });
            let Some(grid) = grid else {
                target.summary.write(GroundGridSummary::default());
                return Ok(());
            };
            let scalars = grid
                .sample_count()
                .checked_mul(3)
                .ok_or(Failure::Argument("GroundGridTooLarge"))?;
            if target.scalar_capacity != 0 {
                if target.scalar_capacity < scalars {
                    return Err(Failure::Argument("GroundGridOutputTooSmall"));
                }
                let positions = grid.positions();
                // SAFETY: scalars <= checked disjoint capacity.
                std::ptr::copy_nonoverlapping(positions.as_ptr(), target.positions, scalars);
            }
            target.summary.write(GroundGridSummary {
                pad: grid.pad,
                ground_z: grid.ground_z,
                columns: grid.columns,
                rows: grid.rows,
                sample_count: grid.sample_count(),
                valid: 1,
            });
            Ok(())
        })
    }
}
