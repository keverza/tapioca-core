//! The ray-tracing scene as a Rust-owned handle and its directional
//! occlusion batch (raycast plan E0). See `Geometry/RaycastRustFfi.h`.
//!
//! Handle contract: created by `tapioca_raycast_scene_build`, released by
//! `tapioca_raycast_scene_destroy`. A scene is immutable, so any number of
//! threads may query one handle at the same time; destroy must not overlap
//! any query. Rust never retains a handle after destroy.

use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use crate::sunstudy::{CancelCallback, Regions};
use tapioca_geometry::ray::{DirectionalQuery, occlude_directional};
use tapioca_raycast::{Scene, SceneInput};

/// Opaque scene handle.
pub struct RaycastSceneHandle {
    scene: Scene,
}

/// Scene inputs: the C++ engine's concatenated arrays.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RaycastSceneRequest {
    /// `3 * vertex_count` world-metre coordinates.
    pub vertices: SliceView<f64>,
    /// `3 * triangle_count` vertex indices.
    pub triangles: SliceView<u32>,
    /// Geometry identity (the snapshot id).
    pub version: u64,
}

/// One directional batch.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RaycastDirectionalRequest {
    /// `3 * count` origin coordinates.
    pub origins: SliceView<f64>,
    /// Shared direction (normalised per ray).
    pub direction: [f64; 3],
    /// Near bound; `<= 0` starts at the origin.
    pub tmin: f64,
    /// Far bound; `<= 0` is unbounded.
    pub tmax: f64,
    /// 0 decides, 1 inline, n at most n threads.
    pub max_parallel: usize,
    /// Optional cancellation, polled only on the calling thread.
    pub cancel: CancelCallback,
}

const _: () = {
    assert!(size_of::<RaycastSceneRequest>() == 40);
    assert!(size_of::<RaycastDirectionalRequest>() == 80);
};

/// Validate the arrays and build the scene and its hierarchy.
///
/// Returns 0 on success, 1 for malformed arguments or a refused scene (the
/// error names why), or 3 for a contained panic. On failure `out` is unchanged.
///
/// # Safety
/// `request` and its arrays must be live and immutable for the call; `out`
/// must be one writable pointer slot disjoint from every other region. The new
/// handle must be released with `tapioca_raycast_scene_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_raycast_scene_build(
    request: *const RaycastSceneRequest,
    out: *mut *mut RaycastSceneHandle,
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
            regions.input(request.vertices.data, request.vertices.count);
            regions.input(request.triangles.data, request.triangles.count);
            regions.check()?;
            let context = regions.context();
            let input = SceneInput {
                vertices: context.slice(request.vertices)?,
                triangles: context.slice(request.triangles)?,
                version: request.version,
            };
            let scene = Scene::build(&input).map_err(|e| Failure::Argument(e.message()))?;
            out.write(Box::into_raw(Box::new(RaycastSceneHandle { scene })));
            Ok(())
        })
    }
}

/// Release a scene. Null is accepted and ignored.
///
/// # Safety
/// `handle` must be null or a live handle from `build` that no thread is
/// using and that is never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_raycast_scene_destroy(
    handle: *mut RaycastSceneHandle,
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

/// `CpuTraversal::OccludeDirectionalCancellable` over the Rust scene: write
/// 1 (occluded) or 0 per origin into `out`; `completed` is 0 when the batch
/// was cancelled (then `out` is partially written).
///
/// # Safety
/// `handle` must be a live scene (other threads may query it concurrently);
/// `request` and its origins must be live and immutable; `out` (`out_count`
/// bytes) and `completed` must be writable and disjoint from every other
/// region. The cancel callback must not unwind and its context must outlive
/// the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_raycast_occlude_directional(
    handle: *const RaycastSceneHandle,
    request: *const RaycastDirectionalRequest,
    out: *mut u8,
    out_count: usize,
    completed: *mut u8,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            memory_range(handle, 1)?;
            // SAFETY: non-null and aligned (checked); liveness is the caller's.
            let scene = &handle
                .as_ref()
                .ok_or(Failure::Argument("RaycastSceneHandle: null"))?
                .scene;
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, out_count);
            regions.output(completed, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            regions.input(request.origins.data, request.origins.count);
            regions.check()?;
            let context = regions.context();
            let query = DirectionalQuery {
                origins: context.slice(request.origins)?,
                direction: request.direction,
                tmin: request.tmin,
                tmax: request.tmax,
                max_parallel: request.max_parallel,
            };
            let cancel = request.cancel;
            // The caller guarantees the poll and its context for the call; the
            // call runs inside this export's outer unsafe block.
            let poll = || cancel.is_cancelled.is_some_and(|f| f(cancel.context) != 0);
            let out = if out_count == 0 {
                &mut []
            } else {
                // SAFETY: checked shape and disjointness above; writable for the call.
                std::slice::from_raw_parts_mut(out, out_count)
            };
            let done = occlude_directional(scene, &query, out, Some(&poll))
                .map_err(|_| Failure::Argument("origins must be three coordinates per output"))?;
            completed.write(u8::from(done));
            Ok(())
        })
    }
}
