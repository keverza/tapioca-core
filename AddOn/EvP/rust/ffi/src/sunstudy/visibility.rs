//! The visibility study (P4) as an immutable Rust-owned result handle:
//! run, summary, read (values and both atlases) and the grids as a surface
//! result. Any thread may read a live handle; destroy it exactly once.

use super::Regions;
use super::atlas::{AtlasOutput, register_atlas_output, write_atlas};
use super::patch_atlas::{PatchAtlasStateOutput, register_state_output, write_state};
use super::reuse::ReuseMeshView;
use super::surface::SurfaceResultHandle;
use super::views::{RayQueryVtable, VtableQueries, meshes_of, register_meshes, texts};
use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::surface_sampling::{SamplingDomain, SurfaceSamplingResult};
use tapioca_sunstudy::visibility::{
    VisibilityOptions, VisibilityOrigin, VisibilityRequest, VisibilityResult, run_visibility,
};

/// Visibility settings (`VisibilityStudyOptions`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VisibilityOptionsRecord {
    /// Spacing, normal offset, tmin, cone degrees.
    pub reals: [f64; 4],
    /// Max samples, max aim points, max rays, max parallel.
    pub limits: [usize; 4],
    /// The eye.
    pub point: [f64; 3],
    /// View direction.
    pub direction: [f64; 3],
    /// 0 surfaces, 1 point.
    pub origin: u8,
    /// 0 triangle, 1 patch.
    pub domain: u8,
    /// 1 when Context is explicit.
    pub explicit_context: u8,
}

/// Visibility inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VisibilityRunRequest {
    /// Snapshot meshes.
    pub meshes: SliceView<ReuseMeshView>,
    /// FROM, TO and Context element GUIDs.
    pub roles: [SliceView<SliceView<u8>>; 3],
    /// Settings.
    pub options: VisibilityOptionsRecord,
    /// The scene index.
    pub queries: *const RayQueryVtable,
}

/// Opaque visibility result.
pub struct VisibilityHandle {
    result: VisibilityResult,
    domain: SamplingDomain,
}

fn options_of(
    r: &VisibilityOptionsRecord,
    context: Vec<String>,
) -> Result<VisibilityOptions, Failure> {
    let [spacing, normal_offset, tmin, cone_degrees] = r.reals;
    let [max_samples, max_aim_points, max_rays, max_parallel] = r.limits;
    // An unknown origin or domain is the C++ "invalid ... origin mode" refusal.
    let invalid =
        || Failure::State("visibility options contain invalid offsets, limits or origin mode");
    Ok(VisibilityOptions {
        origin: match r.origin {
            0 => VisibilityOrigin::Surfaces,
            1 => VisibilityOrigin::Point,
            _ => return Err(invalid()),
        },
        domain: match r.domain {
            0 => SamplingDomain::TriangleLegacy,
            1 => SamplingDomain::SurfacePatch,
            _ => return Err(invalid()),
        },
        explicit_context: super::flag(
            r.explicit_context,
            "VisibilityOptionsRecord: explicit_context",
        )?,
        context_elements: context,
        spacing,
        normal_offset,
        tmin,
        max_samples,
        max_aim_points,
        max_rays,
        max_parallel,
        point: r.point,
        direction: r.direction,
        cone_degrees,
    })
}

/// `RunVisibilityStudy`; a refusal fails with its message. Release the
/// handle with `tapioca_sunstudy_visibility_destroy`.
///
/// # Safety
/// Request, arrays, texts and vtable live for the call; `out` one writable
/// pointer slot, disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_visibility_run(
    request: *const VisibilityRunRequest,
    out: *mut *mut VisibilityHandle,
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
            let list = register_meshes(&mut regions, request.meshes)?;
            let [from_view, to_view, context_view] = request.roles;
            let from_list = regions.text_list(from_view)?;
            let to_list = regions.text_list(to_view)?;
            let context_list = regions.text_list(context_view)?;
            regions.input(request.queries, 1);
            regions.check()?;
            let context = regions.context();
            let meshes = meshes_of(&context, &list)?;
            let from = texts(&context, &from_list)?;
            let to = texts(&context, &to_list)?;
            let picked = texts(&context, &context_list)?;
            let options = options_of(
                &request.options,
                picked.iter().map(|s| (*s).to_owned()).collect(),
            )?;
            let queries = VtableQueries {
                vtable: context.record(request.queries)?,
            };
            let result = run_visibility(&VisibilityRequest {
                meshes: &meshes,
                from: &from,
                to: &to,
                options: &options,
                queries: &queries,
            })
            .map_err(|e| Failure::State(e.0))?;
            out.write(Box::into_raw(Box::new(VisibilityHandle {
                result,
                domain: options.domain,
            })));
            Ok(())
        })
    }
}

/// Release a visibility result. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle no thread is using, never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_visibility_destroy(
    handle: *mut VisibilityHandle,
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

// SAFETY: `handle` must be null or a live handle not being destroyed.
unsafe fn visibility<'a>(handle: *const VisibilityHandle) -> Result<&'a VisibilityHandle, Failure> {
    memory_range(handle, 1)?;
    // SAFETY: non-null and aligned (checked); liveness is the caller's contract.
    unsafe { handle.as_ref() }.ok_or(Failure::Argument("VisibilityHandle: null"))
}

/// Scalars and array sizes of a visibility result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct VisibilitySummary {
    /// Values, aim points, rays, visible samples, triangle tiles, triangle
    /// texels, patch allocations, patch allocation text bytes.
    pub sizes: [usize; 8],
    /// Mean visibility, analysed area.
    pub reals: [f64; 2],
    /// 0 triangle, 1 patch.
    pub domain: u8,
}

/// Read a result's scalars and sizes.
///
/// # Safety
/// The handle must be live; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_visibility_summary(
    handle: *const VisibilityHandle,
    out: *mut VisibilitySummary,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let h = visibility(handle)?;
            let r = &h.result;
            let text: usize = {
                let mut seen = std::collections::BTreeSet::new();
                r.patch_atlas
                    .allocations()
                    .filter(|a| seen.insert(a.key.element.as_str()))
                    .map(|a| a.key.element.len())
                    .sum()
            };
            out.write(VisibilitySummary {
                sizes: [
                    r.values.len(),
                    r.aim_point_count,
                    r.ray_count,
                    r.visible_samples,
                    r.atlas.tiles.len(),
                    r.atlas.texels.len(),
                    r.patch_atlas.allocations().count(),
                    text,
                ],
                reals: [r.mean_visibility, r.analysed_area],
                domain: h.domain as u8,
            });
            Ok(())
        })
    }
}

/// Destinations for a visibility result's arrays and atlases.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VisibilityReadOutput {
    /// Values out.
    pub values: *mut f64,
    /// Capacity in values.
    pub value_capacity: usize,
    /// The triangle atlas.
    pub atlas: AtlasOutput,
    /// The patch atlas.
    pub patch_atlas: PatchAtlasStateOutput,
}

/// Copy values and both atlases out (capacities from the summary).
///
/// # Safety
/// The handle must be live; the descriptor live; every destination writable
/// for the size the summary reported and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_visibility_read(
    handle: *const VisibilityHandle,
    output: *const VisibilityReadOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.check()?;
            let t = regions.context().record(output)?;
            regions.output(t.values, t.value_capacity);
            register_atlas_output(&mut regions, &t.atlas);
            register_state_output(&mut regions, &t.patch_atlas);
            regions.check()?;
            let r = &visibility(handle)?.result;
            if t.value_capacity < r.values.len() {
                return Err(Failure::Argument("VisibilityReadOutputTooSmall"));
            }
            super::atlas::publish(&r.values, t.values);
            write_atlas(&r.atlas, &t.atlas)?;
            write_state(&r.patch_atlas, &t.patch_atlas)
        })
    }
}

/// The result's grids as a surface-result handle (read with
/// `tapioca_sunstudy_surface_summary`/`_read`, release with `_destroy`).
///
/// # Safety
/// The handle must be live; `out` one writable pointer slot, disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_visibility_grids(
    handle: *const VisibilityHandle,
    out: *mut *mut SurfaceResultHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output slot is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            let r = &visibility(handle)?.result;
            let result = SurfaceSamplingResult {
                triangles: r.grid.clone(),
                patches: r.patch_grid.clone(),
                valid: true,
                ..SurfaceSamplingResult::default()
            };
            out.write(Box::into_raw(Box::new(SurfaceResultHandle { result })));
            Ok(())
        })
    }
}
