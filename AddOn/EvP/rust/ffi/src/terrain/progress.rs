//! How far the running terrain build has got (geometry `terrain::progress`): read at any time, from
//! any thread, while `tapioca_terrain_unified_build` runs on another.
use super::call;
use crate::sunstudy::Regions;
use tapioca_geometry::terrain::progress as kernel;

/// One reading of the running build.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainProgress {
    /// 0 idle, 1 preparing, 2 refining, 3 finishing.
    pub stage: u32,
    /// The refinement round, from 1.
    pub round: u32,
    /// Vertices the refinement has added.
    pub added: u32,
    /// Most vertices it may add.
    pub budget: u32,
    /// Triangles the current round is fixing.
    pub pending: u32,
    /// Zero.
    pub reserved: u32,
    /// Largest height error at the current round's start, metres.
    pub residual: f64,
    /// That error at the first round, metres.
    pub first: f64,
    /// The error refined to, metres.
    pub tolerance: f64,
}

/// The running build's progress.
///
/// # Safety
/// `out` writable and disjoint from the error buffer, for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_progress(
    out: *mut TerrainProgress,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: the envelope contains unwinding; the output region is checked before the write.
    unsafe {
        call(error, capacity, || {
            let mut regions = Regions::new(error, capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let p = kernel::now();
            out.write(TerrainProgress {
                stage: p.stage,
                round: p.round,
                added: p.added,
                budget: p.budget,
                pending: p.pending,
                reserved: 0,
                residual: p.residual,
                first: p.first,
                tolerance: p.tolerance,
            });
            Ok(())
        })
    }
}
