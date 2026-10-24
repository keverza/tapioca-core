//! The triangle-atlas export and the sample/atlas views it shares with the
//! stable triangle atlas.
//!
//! The patch atlas is stateful in C++. Until the store moves to Rust, its
//! state crosses the ABI by value: allocations and the shelf cursor go in, the
//! updated ones come out. Element GUIDs go in as text views and come out as
//! (offset, length) into one deduplicated text buffer.

use super::{Regions, flag};
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use tapioca_sunstudy::atlas::{
    AtlasOptions, AtlasSamples, AtlasTile, SunStudyAtlas, build_sun_study_atlas,
};
use tapioca_sunstudy::sampler::FaceLayout;

use super::sampler::FaceLayoutRecord;

// SAFETY: destination holds at least `source.len()` elements, checked disjoint.
pub(super) unsafe fn publish<T: Copy>(source: &[T], destination: *mut T) {
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

pub(super) struct OwnedSamples {
    pub(super) layouts: Vec<FaceLayout>,
}

// SAFETY: every view was registered and checked through `Regions`.
pub(super) unsafe fn read_samples<'a>(
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

pub(super) fn register_samples(regions: &mut Regions, view: &AtlasSamplesView) {
    regions.input(view.layouts.data, view.layouts.count);
    regions.input(view.faces.data, view.faces.count);
    regions.input(view.cell_columns.data, view.cell_columns.count);
    regions.input(view.cell_rows.data, view.cell_rows.count);
}

pub(super) fn register_atlas_output(regions: &mut Regions, target: &AtlasOutput) {
    regions.output(target.tiles, target.tile_capacity);
    regions.output(target.texels, target.texel_capacity);
    regions.output(target.summary, 1);
}

// SAFETY: the output's regions were registered and checked disjoint.
pub(super) unsafe fn write_atlas(
    atlas: &SunStudyAtlas,
    target: &AtlasOutput,
) -> Result<(), Failure> {
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
