//! Borrow-only legacy mesh ingress. This does not certify authored identity (D14).

use crate::boundary::{Failure, call, memory_range, overlaps};
use tapioca_geometry::mesh::{self, ValidationError};

/// Call-scoped array view. Counts are scalar elements, never bytes or xyz tuples.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SliceView<T> {
    /// Immutable caller-owned data; null is accepted only when count is zero.
    pub data: *const T,
    /// Number of scalar elements available at data.
    pub count: usize,
}

/// Borrowed geometry-only mesh. No field substitutes for authored provenance.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshView {
    /// UTF-8 bytes, not NUL-terminated; synthetic/empty GUIDs are allowed here.
    pub guid: SliceView<u8>,
    /// Existing Modeler element type, uninterpreted by generic validation.
    pub elem_type: i32,
    /// Interleaved xyz f64, world metres, right-handed Z-up.
    pub vertices: SliceView<f64>,
    /// Optional interleaved xyz f32 normals, same scalar count as vertices.
    pub normals: SliceView<f32>,
    /// Triangle triples, each index less than vertices.count / 3.
    pub triangles: SliceView<u32>,
    /// Optional material pool indices, one per triangle.
    pub tri_material: SliceView<i32>,
    /// Optional source boundary bits, one per triangle.
    pub tri_wire_edges: SliceView<u8>,
    /// Optional 1-based Modeler body sidecar, paired with `tri_source_pgon`.
    pub tri_source_body: SliceView<i32>,
    /// Optional 1-based Modeler polygon sidecar, paired with `tri_source_body`.
    pub tri_source_pgon: SliceView<i32>,
    /// Optional original project surface attributes, one per triangle.
    pub tri_surface_attribute: SliceView<i32>,
}

/// Borrowed snapshot geometry subset. Material catalogs remain C++ owned/unchanged.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnapshotView {
    /// Existing snapshot ID; zero remains valid in offline fixtures.
    pub id: u64,
    /// Existing model-watch epoch, not authored identity.
    pub capture_stamp: u64,
    /// UTF-8 scope bytes, not NUL-terminated.
    pub scope: SliceView<u8>,
    /// Explicit byte boolean: only 0 and 1 are valid.
    pub complete_model: u8,
    /// Immutable array of mesh descriptors, borrowed only for this call.
    pub meshes: SliceView<MeshView>,
}

struct BorrowContext {
    error_range: Option<(usize, usize)>,
}

impl BorrowContext {
    // SAFETY: callers guarantee live, initialized immutable foreign memory for
    // the entire call, with no aliases to writable error storage. Checked shape
    // and arithmetic reject malformed representations, not dangling allocations.
    unsafe fn slice<'a, T: Copy>(&self, view: SliceView<T>) -> Result<&'a [T], Failure> {
        let range = memory_range(view.data, view.count)?;
        if overlaps(range, self.error_range) {
            return Err(Failure::Overlap);
        }
        if view.count == 0 {
            return Ok(&[]);
        }
        // SAFETY: range checked null, alignment, byte/address overflow and error
        // overlap; allocation validity, initialization and lifetime are foreign
        // caller obligations. The borrow cannot escape an exported call.
        Ok(unsafe { std::slice::from_raw_parts(view.data, view.count) })
    }

    // SAFETY: same allocation/lifetime contract as slice; UTF-8 is checked here.
    unsafe fn text<'a>(&self, view: SliceView<u8>) -> Result<&'a str, Failure> {
        // SAFETY: caller forwards the immutable foreign view contract.
        let bytes = unsafe { self.slice(view)? };
        std::str::from_utf8(bytes)
            .map_err(|_| Failure::Argument("InvalidUtf8: malformed text view"))
    }

    // SAFETY: descriptor and all referenced arrays obey the foreign borrow contract.
    unsafe fn mesh<'a>(&self, input: MeshView) -> Result<mesh::MeshView<'a>, Failure> {
        // SAFETY: each field is checked independently before forming a slice;
        // immutable inputs may share storage, but none may overlap error output.
        unsafe {
            Ok(mesh::MeshView {
                guid: self.text(input.guid)?,
                elem_type: input.elem_type,
                vertices: self.slice(input.vertices)?,
                normals: self.slice(input.normals)?,
                triangles: self.slice(input.triangles)?,
                tri_material: self.slice(input.tri_material)?,
                tri_wire_edges: self.slice(input.tri_wire_edges)?,
                tri_source_body: self.slice(input.tri_source_body)?,
                tri_source_pgon: self.slice(input.tri_source_pgon)?,
                tri_surface_attribute: self.slice(input.tri_surface_attribute)?,
            })
        }
    }
}

fn validation_failure(error: ValidationError) -> Failure {
    match error {
        ValidationError::SnapshotDuplicateGuid => Failure::State(error.message()),
        _ => Failure::Argument(error.message()),
    }
}

/// Validate immutable legacy mesh geometry without repairing/copying it.
///
/// Empty geometry and optional empty sidecars are accepted. Topology-specific
/// degeneracy/manifold/closure/winding tests belong to their owning algorithms.
/// This geometry-only operation does not establish authored provenance.
///
/// # Safety
/// The descriptor and all nonempty arrays must be live, initialized, aligned and
/// immutable for the call. Error storage must be live/exclusive writable bytes,
/// nonoverlapping with every input allocation. No pointer or slice is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_validate_mesh(
    input: *const MeshView,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: caller guarantees foreign allocation/lifetime validity. call
    // contains unwinding panics and checks error storage before the closure runs.
    unsafe {
        call(error, capacity, || {
            let context = BorrowContext {
                error_range: memory_range(error.cast_const(), capacity)?,
            };
            let descriptor = context.slice(SliceView {
                data: input,
                count: 1,
            })?;
            let input = descriptor
                .first()
                .ok_or(Failure::Argument("MissingMeshView"))?;
            let view = context.mesh(*input)?;
            mesh::validate_mesh(&view).map_err(validation_failure)
        })
    }
}

/// Validate snapshot mesh ingress and duplicate nonempty GUIDs, not material catalogs.
///
/// # Safety
/// All descriptor/array/string storage is immutable and live for the call; error
/// storage is exclusively writable and nonoverlapping with every input. Pointer
/// shape checks cannot prove foreign memory validity. No borrowed input survives.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_validate_snapshot(
    input: *const SnapshotView,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: same call-scoped foreign contract as tapioca_validate_mesh.
    unsafe {
        call(error, capacity, || {
            let context = BorrowContext {
                error_range: memory_range(error.cast_const(), capacity)?,
            };
            let descriptor = context.slice(SliceView {
                data: input,
                count: 1,
            })?;
            let input = descriptor
                .first()
                .ok_or(Failure::Argument("MissingSnapshotView"))?;
            if input.complete_model > 1 {
                return Err(Failure::Argument(
                    "InvalidBoolean: complete_model must be 0 or 1",
                ));
            }
            let scope = context.text(input.scope)?;
            let raw_meshes = context.slice(input.meshes)?;
            // Only small descriptors are owned; coordinate/triangle/sidecar arrays
            // stay borrowed. Drop all views before call writes its diagnostic.
            let meshes = raw_meshes
                .iter()
                .map(|view| context.mesh(*view))
                .collect::<Result<Vec<_>, _>>()?;
            let snapshot = mesh::SnapshotView {
                id: input.id,
                capture_stamp: input.capture_stamp,
                scope,
                complete_model: input.complete_model != 0,
                meshes: &meshes,
            };
            mesh::validate_snapshot(&snapshot).map_err(validation_failure)
        })
    }
}

#[cfg(test)]
mod tests;
