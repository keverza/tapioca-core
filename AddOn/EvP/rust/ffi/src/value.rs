//! Complete detached mesh/snapshot value ingress, separate from legacy and identity gates.

use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call, memory_range};
use crate::validation::MeshView;
use tapioca_geometry::value;

/// Complete immutable mesh descriptor, including its retained world-metre bounds.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshValueView {
    /// Existing geometry-only record, unchanged.
    pub mesh: MeshView,
    /// Retained bounds, not recomputed or repaired at ingress.
    pub bounds: value::Aabb,
}

/// Detached material-pool identity including its optional external surface reference.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MaterialIdentity {
    /// Capture-local material-pool key.
    pub index: i32,
    /// Project surface reference, with the existing unknown sentinel semantics.
    pub external_reference: i32,
    /// Immutable length-delimited UTF-8 name.
    pub name: SliceView<u8>,
}

/// Detached project surface record; not an authored editor identity.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SurfaceAttribute {
    /// Project-local surface index.
    pub index: i32,
    /// Immutable UTF-8 surface name.
    pub name: SliceView<u8>,
    /// Immutable UTF-8 native GUID text.
    pub guid: SliceView<u8>,
}

/// Every detached C++ snapshot value, without transferring ownership.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnapshotValueView {
    /// Retained snapshot identifier.
    pub id: u64,
    /// Retained model-watch epoch, not an editor document revision.
    pub capture_stamp: u64,
    /// Capture scope, validated separately from completeness.
    pub scope: SliceView<u8>,
    /// Integer boolean, only zero/one accepted.
    pub complete_model: u32,
    /// Immutable complete mesh descriptors.
    pub meshes: SliceView<MeshValueView>,
    /// Detached transparency fraction catalog.
    pub material_transparency: SliceView<value::MaterialTransparency>,
    /// Detached material identity catalog.
    pub material_identity: SliceView<MaterialIdentity>,
    /// Detached project surface catalog.
    pub surface_attributes: SliceView<SurfaceAttribute>,
}

/// Explicit value-validation policy supplied separately from retained metadata.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnapshotValueRequest {
    /// Complete detached input snapshot.
    pub snapshot: SnapshotValueView,
    /// Zero accepts detached/synthetic data; nonzero requires a matching retained capture stamp.
    pub expected_capture_stamp: u64,
    /// Integer boolean: require all referenced catalogs rather than preserve unknowns.
    pub require_catalogs: u32,
}

impl BorrowContext {
    // SAFETY: all nested geometry arrays are live immutable allocations for the
    // call. Foreign shape/alias checks do not establish allocation validity.
    unsafe fn mesh_value(&self, input: MeshValueView) -> Result<value::MeshValueView<'_>, Failure> {
        // SAFETY: shared checker validates shape/overlap before borrowing.
        let mesh = unsafe { self.mesh(input.mesh)? };
        Ok(value::MeshValueView {
            mesh,
            bounds: input.bounds,
        })
    }
}

fn failure(error: value::Error) -> Failure {
    if error.is_state_error() {
        Failure::State(error.message())
    } else {
        Failure::Argument(error.message())
    }
}

/// Validate all mesh values, including retained bounds and sidecar value semantics.
///
/// # Safety
/// Input and every nested array are initialized live immutable allocations for
/// the call; nonempty error storage is live writable and disjoint from ALL input
/// storage. No borrow is retained. Null/alignment checks do not prove validity.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_validate_mesh_value(
    input: *const MeshValueView,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope validates error shape and contains unwinding panics.
    unsafe {
        call(error, capacity, || {
            let context = BorrowContext {
                error_range: memory_range(error.cast_const(), capacity)?,
                output_range: None,
            };
            let mesh = context.mesh_value(context.record(input)?)?;
            value::validate_mesh_value(&mesh).map_err(failure)
        })
    }
}

/// Validate complete detached snapshot values with explicit catalog/capture requirements.
///
/// Descriptors/catalog text are borrowed or converted into local view metadata,
/// never geometry copies. This is not provenance certification or a host-state
/// read; the C++ owner must still check freshness at publication.
///
/// # Safety
/// Same live/disjoint immutable input and writable error-buffer contract as
/// `tapioca_validate_mesh_value`, including every catalog record and text view.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_validate_snapshot_value(
    input: *const SnapshotValueRequest,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope contains panics and checks error shape; callers
    // retain immutable initialized foreign allocations through completion.
    unsafe {
        call(error, capacity, || {
            let context = BorrowContext {
                error_range: memory_range(error.cast_const(), capacity)?,
                output_range: None,
            };
            let input = context.record(input)?;
            if input.require_catalogs > 1 || input.snapshot.complete_model > 1 {
                return Err(Failure::Argument(
                    "InvalidBoolean: complete_model and require_catalogs must be 0 or 1",
                ));
            }
            let snapshot = input.snapshot;
            let meshes = context
                .slice(snapshot.meshes)?
                .iter()
                .map(|input| context.mesh_value(*input))
                .collect::<Result<Vec<_>, _>>()?;
            let materials = context
                .slice(snapshot.material_identity)?
                .iter()
                .map(|input| {
                    Ok(value::MaterialIdentity {
                        index: input.index,
                        external_reference: input.external_reference,
                        name: context.text(input.name)?,
                    })
                })
                .collect::<Result<Vec<_>, Failure>>()?;
            let surfaces = context
                .slice(snapshot.surface_attributes)?
                .iter()
                .map(|input| {
                    Ok(value::SurfaceAttribute {
                        index: input.index,
                        name: context.text(input.name)?,
                        guid: context.text(input.guid)?,
                    })
                })
                .collect::<Result<Vec<_>, Failure>>()?;
            let request = value::SnapshotValueRequest {
                snapshot: value::SnapshotValueView {
                    id: snapshot.id,
                    capture_stamp: snapshot.capture_stamp,
                    scope: context.text(snapshot.scope)?,
                    complete_model: snapshot.complete_model != 0,
                    meshes: &meshes,
                    material_transparency: context.slice(snapshot.material_transparency)?,
                    material_identity: &materials,
                    surface_attributes: &surfaces,
                },
                expected_capture_stamp: input.expected_capture_stamp,
                require_catalogs: input.require_catalogs != 0,
            };
            value::validate_snapshot_value(&request).map_err(failure)
        })
    }
}

#[cfg(test)]
mod tests;
