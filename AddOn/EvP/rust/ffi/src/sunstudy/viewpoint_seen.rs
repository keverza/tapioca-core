//! A sphere viewpoint's seen surfaces as a Rust-owned handle (#83).
//!
//! Handle contract: `read` only reads it; `destroy` needs exclusive use. The
//! request is the viewpoint's own, so a refusal is the study's (status 2 with
//! its message), and the mesh triangles are read for the first time here.

use super::Regions;
use super::atlas::publish;
use super::views::{
    ViewpointRequest, VtableQueries, meshes_of, register_meshes, texts, viewpoint_options,
};
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::viewpoint::seen::{SeenSurfaces, seen_surfaces};

/// Opaque seen-surface handle.
pub struct SeenHandle {
    surfaces: SeenSurfaces,
}

/// What a handle holds.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SeenSummary {
    /// Triangle corners, three per triangle.
    pub points: usize,
    /// Seen area, m^2.
    pub area: f64,
    /// The cell side the run settled on, metres; 0 off the sphere.
    pub cell_metres: f64,
}

/// SAFETY: `handle` must be null or live, not destroyed during the call.
unsafe fn seen<'a>(handle: *const SeenHandle) -> Result<&'a SeenSurfaces, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }
        .map(|h| &h.surfaces)
        .ok_or(Failure::Argument("SeenHandle: null"))
}

/// The seen surfaces of the viewpoint `request` describes (empty off the
/// sphere). `cell_metres` > 0 fixes the starting cell side.
///
/// # Safety
/// Request, mesh arrays, texts and vtable live for the call; `out` one
/// writable pointer slot and `summary` one writable record, both disjoint
/// from the error buffer. Release with
/// `tapioca_sunstudy_viewpoint_seen_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_viewpoint_seen(
    request: *const ViewpointRequest,
    cell_metres: f64,
    out: *mut *mut SeenHandle,
    summary: *mut SeenSummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(out, 1);
            regions.output(summary, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            let list = register_meshes(&mut regions, request.meshes)?;
            let context_list = regions.text_list(request.context)?;
            regions.input(request.queries, 1);
            regions.check()?;
            let context = regions.context();
            let meshes = meshes_of(&context, &list)?;
            let picked = texts(&context, &context_list)?;
            let queries = VtableQueries {
                vtable: context.record(request.queries)?,
            };
            let options = viewpoint_options(&request)?;
            let surfaces = seen_surfaces(&meshes, &picked, &options, cell_metres, &queries)
                .map_err(|e| Failure::State(e.message()))?;
            summary.write(SeenSummary {
                points: surfaces.triangles.len(),
                area: surfaces.area,
                cell_metres: surfaces.cell_metres,
            });
            out.write(Box::into_raw(Box::new(SeenHandle { surfaces })));
            Ok(())
        })
    }
}

/// Copy the triangle corners; `capacity` must hold the summary's `points`.
///
/// # Safety
/// `handle` live; `points` writable for `capacity` corners and disjoint from
/// the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_viewpoint_seen_read(
    handle: *const SeenHandle,
    points: *mut [f64; 3],
    capacity: usize,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output range is checked first.
    unsafe {
        call(error, error_capacity, || {
            let surfaces = seen(handle)?;
            if capacity < surfaces.triangles.len() {
                return Err(Failure::Argument("SeenOutputTooSmall"));
            }
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(points, surfaces.triangles.len());
            regions.check()?;
            publish(&surfaces.triangles, points);
            Ok(())
        })
    }
}

/// Release a seen-surface handle. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle no thread is using, never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_viewpoint_seen_destroy(
    handle: *mut SeenHandle,
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
