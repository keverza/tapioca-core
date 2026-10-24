//! Patch-atlas fit and the stable triangle atlas, with the atlas state passed
//! by value: allocations and the shelf cursor in, the updated ones out, element
//! GUIDs as text in and as offsets into one deduplicated buffer out.

use super::atlas::{
    AtlasOutput, AtlasSamplesView, publish, read_samples, register_atlas_output, register_samples,
    write_atlas,
};
use super::{Regions, flag};
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use std::collections::BTreeMap;
use tapioca_sunstudy::atlas::{AtlasOptions, AtlasSamples};
use tapioca_sunstudy::patch_atlas::{
    AtlasMesh, PatchAtlas, PatchAtlasAllocation, PatchAtlasCursor, PatchAtlasUpdate, PatchShape,
    build_stable_triangle_atlas,
};
use tapioca_sunstudy::patch_sampler::PatchKey;

/// A patch key with its element as text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchKeyView {
    /// Element GUID, UTF-8.
    pub element: SliceView<u8>,
    /// Plane hash (local triangle index for stable triangle atlases).
    pub plane: u64,
    /// Occurrence.
    pub occurrence: u32,
}

/// One input allocation.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchAllocationView {
    /// Identity.
    pub key: PatchKeyView,
    /// Left, top, width, height, generation.
    pub rect: [u32; 5],
}

/// One rectangle to fit.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchShapeView {
    /// Identity.
    pub key: PatchKeyView,
    /// Lattice columns.
    pub columns: u32,
    /// Lattice rows.
    pub rows: u32,
}

/// The allocator's scalar state.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchAtlasCursorRecord {
    /// Width, height, pen x, pen y, shelf height, next generation.
    pub fields: [u32; 6],
}

impl From<PatchAtlasCursor> for PatchAtlasCursorRecord {
    fn from(c: PatchAtlasCursor) -> Self {
        Self {
            fields: [
                c.width,
                c.height,
                c.pen_x,
                c.pen_y,
                c.shelf_height,
                c.next_generation,
            ],
        }
    }
}

impl From<PatchAtlasCursorRecord> for PatchAtlasCursor {
    fn from(r: PatchAtlasCursorRecord) -> Self {
        let [width, height, pen_x, pen_y, shelf_height, next_generation] = r.fields;
        Self {
            width,
            height,
            pen_x,
            pen_y,
            shelf_height,
            next_generation,
        }
    }
}

/// A whole patch atlas, by value.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchAtlasStateView {
    /// Scalar state.
    pub cursor: PatchAtlasCursorRecord,
    /// Allocations (any order; keys must be unique).
    pub allocations: SliceView<PatchAllocationView>,
}

/// One output allocation; its element is `text[offset..offset + length]`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchAllocationRecord {
    /// Byte offset of the element GUID in the output text.
    pub element_offset: usize,
    /// Byte length of the element GUID.
    pub element_length: usize,
    /// Plane hash.
    pub plane: u64,
    /// Occurrence.
    pub occurrence: u32,
    /// Left, top, width, height, generation.
    pub rect: [u32; 5],
}

/// Caller-owned destination for an atlas state.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchAtlasStateOutput {
    /// Scalar state out.
    pub cursor: *mut PatchAtlasCursorRecord,
    /// Allocations out, in key order.
    pub allocations: *mut PatchAllocationRecord,
    /// Capacity in allocations (the shape or triangle count suffices).
    pub allocation_capacity: usize,
    /// Allocations written.
    pub allocation_count: *mut usize,
    /// Deduplicated element GUID bytes.
    pub text: *mut u8,
    /// Capacity in bytes (the sum of distinct input element lengths suffices).
    pub text_capacity: usize,
    /// Bytes written.
    pub text_length: *mut usize,
}

/// Counts of one fit.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchAtlasUpdateRecord {
    /// Retained, reused, moved, added, removed.
    pub counts: [usize; 5],
    /// 1 when the atlas grew and everything moved.
    pub resized: u8,
}

impl From<PatchAtlasUpdate> for PatchAtlasUpdateRecord {
    fn from(u: PatchAtlasUpdate) -> Self {
        Self {
            counts: [u.retained, u.reused, u.moved, u.added, u.removed],
            resized: u8::from(u.resized),
        }
    }
}

// SAFETY: the key's text was registered and checked through `Regions`.
unsafe fn key_of(context: &BorrowContext, view: &PatchKeyView) -> Result<PatchKey, Failure> {
    Ok(PatchKey {
        // SAFETY: forwarded from the caller's checked registration.
        element: unsafe { context.text(view.element)? }.to_owned(),
        plane: view.plane,
        occurrence: view.occurrence,
    })
}

/// Register a state's allocation list and each allocation's text.
///
/// SAFETY: the state's allocation list must be live and immutable.
unsafe fn register_state(
    regions: &mut Regions,
    state: &PatchAtlasStateView,
) -> Result<Vec<PatchAllocationView>, Failure> {
    regions.input(state.allocations.data, state.allocations.count);
    regions.check()?;
    let context = regions.context();
    // SAFETY: shape and disjointness checked just above; copied by value.
    let list = unsafe { context.slice(state.allocations)? }.to_vec();
    for allocation in &list {
        regions.input(allocation.key.element.data, allocation.key.element.count);
    }
    Ok(list)
}

// SAFETY: every allocation's text was registered and checked.
unsafe fn read_state(
    context: &BorrowContext,
    cursor: PatchAtlasCursorRecord,
    list: &[PatchAllocationView],
) -> Result<PatchAtlas, Failure> {
    let mut allocations = Vec::with_capacity(list.len());
    let mut seen = std::collections::BTreeSet::new();
    for view in list {
        // SAFETY: forwarded from the caller's checked registration.
        let key = unsafe { key_of(context, &view.key)? };
        if !seen.insert(key.clone()) {
            return Err(Failure::Argument("PatchAtlasDuplicateKey"));
        }
        let [x, y, width, height, generation] = view.rect;
        allocations.push(PatchAtlasAllocation {
            key,
            x,
            y,
            width,
            height,
            generation,
        });
    }
    Ok(PatchAtlas::from_state(cursor.into(), allocations))
}

fn register_state_output(regions: &mut Regions, target: &PatchAtlasStateOutput) {
    regions.output(target.cursor, 1);
    regions.output(target.allocations, target.allocation_capacity);
    regions.output(target.allocation_count, 1);
    regions.output(target.text, target.text_capacity);
    regions.output(target.text_length, 1);
}

// SAFETY: the output's regions were registered and checked disjoint.
unsafe fn write_state(atlas: &PatchAtlas, target: &PatchAtlasStateOutput) -> Result<(), Failure> {
    let mut text: Vec<u8> = Vec::new();
    let mut offsets: BTreeMap<&str, usize> = BTreeMap::new();
    let mut records = Vec::new();
    for allocation in atlas.allocations() {
        let element = allocation.key.element.as_str();
        let offset = *offsets.entry(element).or_insert_with(|| {
            let at = text.len();
            text.extend_from_slice(element.as_bytes());
            at
        });
        records.push(PatchAllocationRecord {
            element_offset: offset,
            element_length: element.len(),
            plane: allocation.key.plane,
            occurrence: allocation.key.occurrence,
            rect: [
                allocation.x,
                allocation.y,
                allocation.width,
                allocation.height,
                allocation.generation,
            ],
        });
    }
    if target.allocation_capacity < records.len() || target.text_capacity < text.len() {
        return Err(Failure::Argument("PatchAtlasStateOutputTooSmall"));
    }
    // SAFETY: lengths checked against the disjoint capacities above.
    unsafe {
        publish(&records, target.allocations);
        publish(&text, target.text);
        target.cursor.write(atlas.cursor().into());
        target.allocation_count.write(records.len());
        target.text_length.write(text.len());
    }
    Ok(())
}

/// Patch-atlas fit inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchAtlasFitRequest {
    /// The atlas before the fit.
    pub state: PatchAtlasStateView,
    /// Rectangles to fit.
    pub shapes: SliceView<PatchShapeView>,
    /// Gutter texels (raised to 1).
    pub gutter: u32,
    /// Ceiling per axis.
    pub max_dimension: u32,
}

/// Fit outputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PatchAtlasFitOutput {
    /// The atlas after the fit.
    pub state: PatchAtlasStateOutput,
    /// Required update counts.
    pub update: *mut PatchAtlasUpdateRecord,
}

/// `SunStudyPatchAtlas::Fit` on a state passed by value.
///
/// # Safety
/// The request, its lists and every text must be live, initialized and
/// immutable; every output region at its capacity must be live, aligned,
/// exclusively writable and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_patch_atlas_fit(
    request: *const PatchAtlasFitRequest,
    output: *const PatchAtlasFitOutput,
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
            let list = register_state(&mut regions, &request.state)?;
            regions.input(request.shapes.data, request.shapes.count);
            regions.check()?;
            let shapes_in = {
                let context = regions.context();
                context.slice(request.shapes)?.to_vec()
            };
            for shape in &shapes_in {
                regions.input(shape.key.element.data, shape.key.element.count);
            }
            register_state_output(&mut regions, &target.state);
            regions.output(target.update, 1);
            regions.check()?;
            let context = regions.context();
            let mut atlas = read_state(&context, request.state.cursor, &list)?;
            let shapes = shapes_in
                .iter()
                .map(|s| {
                    Ok(PatchShape {
                        key: key_of(&context, &s.key)?,
                        columns: s.columns,
                        rows: s.rows,
                    })
                })
                .collect::<Result<Vec<_>, Failure>>()?;
            let update = atlas.fit(&shapes, request.gutter, request.max_dimension);
            write_state(&atlas, &target.state)?;
            target.update.write(update.into());
            Ok(())
        })
    }
}

/// One snapshot mesh.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AtlasMeshView {
    /// Element GUID, any spelling.
    pub guid: SliceView<u8>,
    /// Triangles in the mesh.
    pub triangle_count: usize,
}

/// Stable-triangle-atlas inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StableAtlasRequest {
    /// The sample grid's layouts and per-sample cells.
    pub samples: AtlasSamplesView,
    /// Snapshot meshes in face order.
    pub meshes: SliceView<AtlasMeshView>,
    /// The caller's allocation state (used unless `previous` replaces it).
    pub allocations: PatchAtlasStateView,
    /// A previous study's state.
    pub previous: PatchAtlasStateView,
    /// 1 when `previous` is supplied, else 0.
    pub has_previous: u8,
    /// Gutter texels.
    pub gutter: u32,
    /// Ceiling per axis.
    pub max_dimension: u32,
}

/// Stable-triangle-atlas outputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StableAtlasOutput {
    /// The atlas.
    pub atlas: AtlasOutput,
    /// The allocation state after the build.
    pub state: PatchAtlasStateOutput,
}

/// `BuildStableTriangleAtlas` on states passed by value.
///
/// # Safety
/// As [`tapioca_sunstudy_patch_atlas_fit`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_stable_triangle_atlas(
    request: *const StableAtlasRequest,
    output: *const StableAtlasOutput,
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
            register_samples(&mut regions, &request.samples);
            let current = register_state(&mut regions, &request.allocations)?;
            let has_previous = flag(
                request.has_previous,
                "StableAtlasHasPrevious: expected 0 or 1",
            )?;
            let previous = if has_previous {
                register_state(&mut regions, &request.previous)?
            } else {
                Vec::new()
            };
            regions.input(request.meshes.data, request.meshes.count);
            regions.check()?;
            let meshes_in = {
                let context = regions.context();
                context.slice(request.meshes)?.to_vec()
            };
            for mesh in &meshes_in {
                regions.input(mesh.guid.data, mesh.guid.count);
            }
            register_atlas_output(&mut regions, &target.atlas);
            register_state_output(&mut regions, &target.state);
            regions.check()?;
            let context = regions.context();
            let (owned, [faces, cell_columns, cell_rows]) =
                read_samples(&context, &request.samples)?;
            let samples = AtlasSamples {
                layouts: &owned.layouts,
                faces,
                cell_columns,
                cell_rows,
            };
            let meshes = meshes_in
                .iter()
                .map(|m| {
                    Ok(AtlasMesh {
                        guid: context.text(m.guid)?,
                        triangle_count: m.triangle_count,
                    })
                })
                .collect::<Result<Vec<_>, Failure>>()?;
            let mut allocations = read_state(&context, request.allocations.cursor, &current)?;
            let previous = if has_previous {
                Some(read_state(&context, request.previous.cursor, &previous)?)
            } else {
                None
            };
            let atlas = build_stable_triangle_atlas(
                &samples,
                &meshes,
                &mut allocations,
                previous.as_ref(),
                &AtlasOptions {
                    gutter: request.gutter,
                    max_dimension: request.max_dimension,
                },
            );
            write_atlas(&atlas, &target.atlas)?;
            write_state(&allocations, &target.state)
        })
    }
}
