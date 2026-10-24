//! The host scene index as a ray-query vtable (its C++ shim catches every
//! exception) and the viewpoint study (P4). Refusals fail with the C++
//! study's message (status 2).

use super::Regions;
use super::reuse::ReuseMeshView;
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use tapioca_sunstudy::rays::{FirstHitRay, OcclusionRay, RayError, RayHit, RayQueries};
use tapioca_sunstudy::surface_sampling::SamplingMesh;
use tapioca_sunstudy::viewpoint::{ViewpointOptions, run_viewpoint};

/// One first-hit ray.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FirstHitRayRecord {
    /// Start.
    pub origin: [f64; 3],
    /// Unit direction.
    pub dir: [f64; 3],
    /// Farthest distance.
    pub length: f64,
}

/// One first-hit answer.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RayHitRecord {
    /// Distance.
    pub t: f64,
    /// Snapshot mesh index.
    pub mesh: u32,
    /// 1 when hit.
    pub hit: u8,
}

/// One bounded any-hit ray.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OcclusionRayRecord {
    /// Start.
    pub origin: [f64; 3],
    /// Unit direction.
    pub dir: [f64; 3],
    /// Near distance.
    pub tmin: f64,
    /// Far distance.
    pub tmax: f64,
}

/// The host's scene index as C callbacks. Every callback returns 1 done,
/// 0 cancelled, 2 failed, and must not unwind.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RayQueryVtable {
    /// Passed back to every callback.
    pub context: *mut core::ffi::c_void,
    /// Acquire the index.
    pub prepare: Option<unsafe extern "C" fn(context: *mut core::ffi::c_void) -> u8>,
    /// First hits; a null mask is the whole model.
    pub first_hits: Option<
        unsafe extern "C" fn(
            context: *mut core::ffi::c_void,
            rays: *const FirstHitRayRecord,
            count: usize,
            mask: *const u8,
            mask_count: usize,
            out: *mut RayHitRecord,
            max_parallel: usize,
        ) -> u8,
    >,
    /// Any-hit occlusion over the whole model: 1 blocked.
    pub occlude_rays: Option<
        unsafe extern "C" fn(
            context: *mut core::ffi::c_void,
            rays: *const OcclusionRayRecord,
            count: usize,
            out: *mut u8,
            max_parallel: usize,
        ) -> u8,
    >,
    /// Optional cancellation poll: 1 cancelled.
    pub is_cancelled: Option<unsafe extern "C" fn(context: *mut core::ffi::c_void) -> u8>,
}

pub(super) struct VtableQueries {
    pub(super) vtable: RayQueryVtable,
}

fn status(code: u8) -> Result<(), RayError> {
    match code {
        1 => Ok(()),
        0 => Err(RayError::Cancelled),
        _ => Err(RayError::Failed),
    }
}

impl RayQueries for VtableQueries {
    fn prepare(&self) -> Result<(), RayError> {
        let Some(prepare) = self.vtable.prepare else {
            return Ok(());
        };
        // SAFETY: the caller guarantees the callback and context for the call
        // and that the callback does not unwind.
        status(unsafe { prepare(self.vtable.context) })
    }

    fn first_hits(
        &self,
        rays: &[FirstHitRay],
        mask: Option<&[u8]>,
        out: &mut [RayHit],
        max_parallel: usize,
    ) -> Result<(), RayError> {
        let Some(cast) = self.vtable.first_hits else {
            return Err(RayError::Failed);
        };
        let records: Vec<FirstHitRayRecord> = rays
            .iter()
            .map(|r| FirstHitRayRecord {
                origin: r.origin,
                dir: r.dir,
                length: r.length,
            })
            .collect();
        let mut hits = vec![RayHitRecord::default(); records.len()];
        let (mask_ptr, mask_len) = mask.map_or((std::ptr::null(), 0), |m| (m.as_ptr(), m.len()));
        // SAFETY: Rust-owned buffers of the stated sizes live for the call;
        // the callback does not unwind or retain the pointers.
        let code = unsafe {
            cast(
                self.vtable.context,
                records.as_ptr(),
                records.len(),
                mask_ptr,
                mask_len,
                hits.as_mut_ptr(),
                max_parallel,
            )
        };
        status(code)?;
        for (slot, h) in out.iter_mut().zip(&hits) {
            *slot = RayHit {
                hit: h.hit != 0,
                t: h.t,
                mesh: usize::try_from(h.mesh).unwrap_or(usize::MAX),
            };
        }
        Ok(())
    }

    fn occlude_rays(
        &self,
        rays: &[OcclusionRay],
        out: &mut [u8],
        max_parallel: usize,
    ) -> Result<(), RayError> {
        let Some(occlude) = self.vtable.occlude_rays else {
            return Err(RayError::Failed);
        };
        if out.len() != rays.len() {
            return Err(RayError::Failed);
        }
        let records: Vec<OcclusionRayRecord> = rays
            .iter()
            .map(|r| OcclusionRayRecord {
                origin: r.origin,
                dir: r.dir,
                tmin: r.tmin,
                tmax: r.tmax,
            })
            .collect();
        // SAFETY: as `first_hits`.
        status(unsafe {
            occlude(
                self.vtable.context,
                records.as_ptr(),
                records.len(),
                out.as_mut_ptr(),
                max_parallel,
            )
        })
    }

    fn cancelled(&self) -> bool {
        // SAFETY: as `prepare`.
        self.vtable
            .is_cancelled
            .is_some_and(|poll| unsafe { poll(self.vtable.context) } != 0)
    }
}

/// Register a mesh list, then each mesh's arrays.
///
/// SAFETY: the list must be live and immutable for the call.
pub(super) unsafe fn register_meshes(
    regions: &mut Regions,
    view: SliceView<ReuseMeshView>,
) -> Result<Vec<ReuseMeshView>, Failure> {
    regions.input(view.data, view.count);
    regions.check()?;
    let context = regions.context();
    // SAFETY: shape and disjointness checked just above; copied by value.
    let list = unsafe { context.slice(view)? }.to_vec();
    for m in &list {
        regions.input(m.guid.data, m.guid.count);
        regions.input(m.vertices.data, m.vertices.count);
        regions.input(m.triangles.data, m.triangles.count);
    }
    Ok(list)
}

// SAFETY: every mesh array was registered and checked.
pub(super) unsafe fn meshes_of<'a>(
    context: &'a BorrowContext,
    list: &[ReuseMeshView],
) -> Result<Vec<SamplingMesh<'a>>, Failure> {
    list.iter()
        .map(|m| {
            // SAFETY: forwarded from the caller's checked registration.
            unsafe {
                Ok(SamplingMesh {
                    guid: context.text(m.guid)?,
                    vertices: context.slice(m.vertices)?,
                    triangles: context.slice(m.triangles)?,
                })
            }
        })
        .collect()
}

// SAFETY: every text of the list was registered and checked.
pub(super) unsafe fn texts<'a>(
    context: &'a BorrowContext,
    list: &[SliceView<u8>],
) -> Result<Vec<&'a str>, Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    list.iter().map(|t| unsafe { context.text(*t) }).collect()
}

/// Viewpoint inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ViewpointRequest {
    /// Snapshot meshes (only the GUIDs are read).
    pub meshes: SliceView<ReuseMeshView>,
    /// Context element GUIDs.
    pub context: SliceView<SliceView<u8>>,
    /// The eye.
    pub point: [f64; 3],
    /// Reach, metres.
    pub radius: f64,
    /// Rays.
    pub rays: usize,
    /// The scene index.
    pub queries: *const RayQueryVtable,
}

/// Viewpoint outputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ViewpointOutput {
    /// Perimeter points (`rays` of them).
    pub perimeter: *mut [f64; 3],
    /// Capacity in points.
    pub capacity: usize,
    /// Points written, collisions.
    pub counts: *mut [usize; 2],
    /// Enclosed area.
    pub area: *mut f64,
}

/// `RunViewpointStudy`; a refusal fails with its message.
///
/// # Safety
/// Request, arrays, texts and vtable live for the call; outputs writable for
/// their capacities and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_viewpoint(
    request: *const ViewpointRequest,
    output: *const ViewpointOutput,
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
            let list = register_meshes(&mut regions, request.meshes)?;
            let context_list = regions.text_list(request.context)?;
            regions.input(request.queries, 1);
            regions.output(target.perimeter, target.capacity);
            regions.output(target.counts, 1);
            regions.output(target.area, 1);
            regions.check()?;
            let context = regions.context();
            let meshes = meshes_of(&context, &list)?;
            let guids: Vec<&str> = meshes.iter().map(|m| m.guid).collect();
            let picked = texts(&context, &context_list)?;
            let queries = VtableQueries {
                vtable: context.record(request.queries)?,
            };
            let options = ViewpointOptions {
                point: request.point,
                radius: request.radius,
                rays: request.rays,
            };
            let result = run_viewpoint(&guids, &picked, &options, &queries)
                .map_err(|e| Failure::State(e.message()))?;
            if target.capacity < result.perimeter.len() {
                return Err(Failure::Argument("ViewpointOutputTooSmall"));
            }
            super::atlas::publish(&result.perimeter, target.perimeter);
            target
                .counts
                .write([result.perimeter.len(), result.collision_count]);
            target.area.write(result.area);
            Ok(())
        })
    }
}
