//! Atlas exports: triangle atlas, stable triangle atlas, patch-atlas fit, step
//! masks, texture deltas and the step schedule.
//!
//! The patch atlas is stateful in C++. Until the store moves to Rust, its
//! state crosses the ABI by value: allocations and the shelf cursor go in, the
//! updated ones come out. Element GUIDs go in as text views and come out as
//! (offset, length) into one deduplicated text buffer.

use super::{Regions, flag};
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use std::collections::BTreeMap;
use tapioca_sunstudy::atlas::{
    AtlasOptions, AtlasSamples, AtlasTile, SunStudyAtlas, build_sun_study_atlas,
};
use tapioca_sunstudy::patch_atlas::{
    AtlasMesh, PatchAtlas, PatchAtlasAllocation, PatchAtlasCursor, PatchAtlasUpdate, PatchShape,
    build_stable_triangle_atlas,
};
use tapioca_sunstudy::patch_sampler::PatchKey;
use tapioca_sunstudy::sampler::FaceLayout;
use tapioca_sunstudy::series::SunStep;
use tapioca_sunstudy::step_atlas;
use tapioca_sunstudy::texture_delta::{self, AtlasRegion};

use super::sampler::FaceLayoutRecord;

// SAFETY: destination holds at least `source.len()` elements, checked disjoint.
unsafe fn publish<T: Copy>(source: &[T], destination: *mut T) {
    if !source.is_empty() {
        // SAFETY: forwarded from the caller.
        unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), destination, source.len()) };
    }
}

fn layout(record: &FaceLayoutRecord) -> Result<FaceLayout, Failure> {
    Ok(FaceLayout {
        origin: record.origin,
        u_axis: record.u_axis,
        v_axis: record.v_axis,
        u_start: record.u_start,
        v_start: record.v_start,
        columns: record.columns,
        rows: record.rows,
        gridded: flag(record.gridded, "FaceLayoutGridded: expected 0 or 1")?,
    })
}

/// The sample-grid arrays an atlas reads.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AtlasSamplesView {
    /// One per source face.
    pub layouts: SliceView<FaceLayoutRecord>,
    /// Source face per sample.
    pub faces: SliceView<u32>,
    /// Cell column per sample.
    pub cell_columns: SliceView<u32>,
    /// Cell row per sample.
    pub cell_rows: SliceView<u32>,
}

/// One atlas tile in C layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AtlasTileRecord {
    /// Left texel.
    pub x: u32,
    /// Top texel.
    pub y: u32,
    /// Texels across (0 = not placed).
    pub width: u32,
    /// Texels down.
    pub height: u32,
}

impl From<&AtlasTile> for AtlasTileRecord {
    fn from(t: &AtlasTile) -> Self {
        Self {
            x: t.x,
            y: t.y,
            width: t.width,
            height: t.height,
        }
    }
}

/// Atlas outputs. Capacities: one tile per layout, one texel per sample.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AtlasOutput {
    /// Tiles; `tile_count` written.
    pub tiles: *mut AtlasTileRecord,
    /// Capacity in tiles.
    pub tile_capacity: usize,
    /// Texel per sample; `texel_count` written.
    pub texels: *mut u32,
    /// Capacity in texels.
    pub texel_capacity: usize,
    /// Required summary.
    pub summary: *mut AtlasSummary,
}

/// What an atlas build produced.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AtlasSummary {
    /// Tiles written (0 when no layouts or a hard refusal).
    pub tile_count: usize,
    /// Texels written.
    pub texel_count: usize,
    /// Faces with a rectangle.
    pub placed_faces: usize,
    /// Atlas texels across.
    pub width: u32,
    /// Atlas texels down.
    pub height: u32,
    /// 1 when the atlas is complete.
    pub valid: u8,
}

struct OwnedSamples {
    layouts: Vec<FaceLayout>,
}

// SAFETY: every view was registered and checked through `Regions`.
unsafe fn read_samples<'a>(
    context: &'a BorrowContext,
    view: &AtlasSamplesView,
) -> Result<(OwnedSamples, [&'a [u32]; 3]), Failure> {
    // SAFETY: forwarded from the caller's checked registration.
    unsafe {
        let layouts = context
            .slice(view.layouts)?
            .iter()
            .map(layout)
            .collect::<Result<Vec<_>, _>>()?;
        Ok((
            OwnedSamples { layouts },
            [
                context.slice(view.faces)?,
                context.slice(view.cell_columns)?,
                context.slice(view.cell_rows)?,
            ],
        ))
    }
}

fn register_samples(regions: &mut Regions, view: &AtlasSamplesView) {
    regions.input(view.layouts.data, view.layouts.count);
    regions.input(view.faces.data, view.faces.count);
    regions.input(view.cell_columns.data, view.cell_columns.count);
    regions.input(view.cell_rows.data, view.cell_rows.count);
}

fn register_atlas_output(regions: &mut Regions, target: &AtlasOutput) {
    regions.output(target.tiles, target.tile_capacity);
    regions.output(target.texels, target.texel_capacity);
    regions.output(target.summary, 1);
}

// SAFETY: the output's regions were registered and checked disjoint.
unsafe fn write_atlas(atlas: &SunStudyAtlas, target: &AtlasOutput) -> Result<(), Failure> {
    if target.tile_capacity < atlas.tiles.len() || target.texel_capacity < atlas.texels.len() {
        return Err(Failure::Argument("AtlasOutputTooSmall"));
    }
    let tiles: Vec<AtlasTileRecord> = atlas.tiles.iter().map(AtlasTileRecord::from).collect();
    // SAFETY: lengths checked against the disjoint capacities above.
    unsafe {
        publish(&tiles, target.tiles);
        publish(&atlas.texels, target.texels);
        target.summary.write(AtlasSummary {
            tile_count: atlas.tiles.len(),
            texel_count: atlas.texels.len(),
            placed_faces: atlas.placed_faces,
            width: atlas.width,
            height: atlas.height,
            valid: u8::from(atlas.valid),
        });
    }
    Ok(())
}

/// Triangle-atlas inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TriangleAtlasRequest {
    /// The sample grid's layouts and per-sample cells.
    pub samples: AtlasSamplesView,
    /// Gutter texels (raised to 1).
    pub gutter: u32,
    /// Ceiling per axis.
    pub max_dimension: u32,
}

/// Pack a triangle atlas (`BuildSunStudyAtlas`).
///
/// # Safety
/// The request and its arrays must be live, initialized and immutable; the
/// output arrays at their capacities and the summary must be live, aligned,
/// exclusively writable and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_triangle_atlas(
    request: *const TriangleAtlasRequest,
    output: *const AtlasOutput,
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
            register_atlas_output(&mut regions, &target);
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
            let atlas = build_sun_study_atlas(
                &samples,
                &AtlasOptions {
                    gutter: request.gutter,
                    max_dimension: request.max_dimension,
                },
            );
            write_atlas(&atlas, &target)
        })
    }
}

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

/// Step-mask inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepMaskRequest {
    /// Sample-major LSB-first lit bits, `step_count` per sample.
    pub step_bits: SliceView<u8>,
    /// Hours-atlas texel per sample, negative for none.
    pub texel_of_sample: SliceView<i64>,
    /// Samples.
    pub sample_count: usize,
    /// Steps.
    pub step_count: usize,
    /// Atlas texels across.
    pub width: u32,
    /// Atlas texels down.
    pub height: u32,
}

/// Step-mask outputs: `words * width * height` slice-major masks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepMaskOutput {
    /// Masks.
    pub masks: *mut u32,
    /// Capacity in words.
    pub capacity: usize,
    /// Slices written (at least 1).
    pub words: *mut u32,
}

/// Pack per-step lit bits texel for texel (`PackStepMasks`).
///
/// # Safety
/// The request and its arrays must be live, initialized and immutable; the
/// outputs must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_step_masks(
    request: *const StepMaskRequest,
    output: *const StepMaskOutput,
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
            regions.input(request.step_bits.data, request.step_bits.count);
            regions.input(request.texel_of_sample.data, request.texel_of_sample.count);
            regions.output(target.masks, target.capacity);
            regions.output(target.words, 1);
            regions.check()?;
            let needed = request
                .sample_count
                .checked_mul(request.step_count)
                .map(|bits| bits.div_ceil(8))
                .ok_or(Failure::Argument("StepMaskTooLarge"))?;
            if request.step_bits.count < needed {
                return Err(Failure::Argument("StepMaskBitsTooShort"));
            }
            let context = regions.context();
            let atlas = step_atlas::pack_step_masks(
                context.slice(request.step_bits)?,
                request.sample_count,
                request.step_count,
                context.slice(request.texel_of_sample)?,
                (request.width, request.height),
            );
            if target.capacity < atlas.masks.len() {
                return Err(Failure::Argument("StepMaskOutputTooSmall"));
            }
            publish(&atlas.masks, target.masks);
            target.words.write(atlas.words);
            Ok(())
        })
    }
}

/// Texture-delta inputs. `kind` 0 compares f32 texels, 1 compares u32.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TextureDeltaRequest {
    /// Image before, as raw 32-bit texels.
    pub before: SliceView<u32>,
    /// Image after.
    pub after: SliceView<u32>,
    /// Texels across, down, and layers.
    pub size: [u32; 3],
    /// Regions per layer before a full-plane collapse (C++ default 512).
    pub max_regions_per_layer: usize,
}

/// Texture-delta outputs. Capacity `layers * max(1, max_regions_per_layer)`
/// always suffices.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TextureDeltaOutput {
    /// Regions.
    pub regions: *mut AtlasRegion,
    /// Capacity in regions.
    pub capacity: usize,
    /// Regions written (0 for "nothing changed" or "full upload required").
    pub count: *mut usize,
}

/// Changed regions between two images (`AtlasChangedRegions`). Both f32 and
/// u32 images compare their bytes, so one 32-bit entry point serves both.
///
/// # Safety
/// The request and its arrays must be live, initialized and immutable; the
/// outputs must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_texture_delta(
    request: *const TextureDeltaRequest,
    output: *const TextureDeltaOutput,
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
            regions.input(request.before.data, request.before.count);
            regions.input(request.after.data, request.after.count);
            regions.output(target.regions, target.capacity);
            regions.output(target.count, 1);
            regions.check()?;
            let context = regions.context();
            let [width, height, layers] = request.size;
            let found = texture_delta::atlas_changed_regions(
                (width, height, layers),
                context.slice(request.before)?,
                context.slice(request.after)?,
                request.max_regions_per_layer,
            );
            if target.capacity < found.len() {
                return Err(Failure::Argument("TextureDeltaOutputTooSmall"));
            }
            publish(&found, target.regions);
            target.count.write(found.len());
            Ok(())
        })
    }
}

/// Step-schedule inputs: a filtered series' steps and the fan interval.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepScheduleRequest {
    /// Kept steps (as `tapioca_sunstudy_series` returns them).
    pub steps: SliceView<SunStep>,
    /// Fan interval, minutes (<= 0 keeps every step).
    pub fan_interval_minutes: f64,
}

/// Step-schedule outputs; both arrays hold one entry per step.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StepScheduleOutput {
    /// Minutes of day per step.
    pub minutes: *mut u16,
    /// Fan step indices; `fan_count` written.
    pub fan: *mut u32,
    /// Capacity of both arrays.
    pub capacity: usize,
    /// Fan entries written.
    pub fan_count: *mut usize,
    /// The highest-sun step.
    pub solar_noon: *mut u32,
}

/// Minutes, solar noon and fan steps for a series (`StepMinutes`,
/// `SolarNoonStep`, `FanSteps`).
///
/// # Safety
/// The request and its steps must be live, initialized and immutable; every
/// output must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_step_schedule(
    request: *const StepScheduleRequest,
    output: *const StepScheduleOutput,
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
            regions.input(request.steps.data, request.steps.count);
            regions.output(target.minutes, target.capacity);
            regions.output(target.fan, target.capacity);
            regions.output(target.fan_count, 1);
            regions.output(target.solar_noon, 1);
            regions.check()?;
            let context = regions.context();
            let steps = context.slice(request.steps)?;
            if target.capacity < steps.len() {
                return Err(Failure::Argument("StepScheduleOutputTooSmall"));
            }
            let minutes = step_atlas::step_minutes(steps);
            let fan = step_atlas::fan_steps(&minutes, request.fan_interval_minutes);
            publish(&minutes, target.minutes);
            publish(&fan, target.fan);
            target.fan_count.write(fan.len());
            target.solar_noon.write(step_atlas::solar_noon_step(steps));
            Ok(())
        })
    }
}
