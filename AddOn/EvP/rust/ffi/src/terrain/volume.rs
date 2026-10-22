//! Terrain cut/fill ABI v1: whole-terrain grid volumes, site and context, and
//! per-edit attribution for an edited surface on the existing mesh.

use super::{BorrowContext, Failure, SliceView, call, memory_range};
use crate::pools::{self, Range};
use tapioca_geometry::terrain::field::Field;
use tapioca_geometry::terrain::volume as kernel;

/// The existing mesh, the proposed surface and the grid. `proposed`: 1 another
/// mesh (`proposed_points`/`proposed_triangles`), 2 a plane at `plane`, 3 the
/// existing mesh with `heights` and `owners` (`u32::MAX` for no owner).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainVolumeRequest {
    /// Existing flat xyz, world metres.
    pub points: SliceView<f64>,
    /// Existing vertex triples.
    pub triangles: SliceView<u32>,
    /// Proposed flat xyz (kind 1).
    pub proposed_points: SliceView<f64>,
    /// Proposed vertex triples (kind 1).
    pub proposed_triangles: SliceView<u32>,
    /// Edited heights, one per existing vertex (kind 3).
    pub heights: SliceView<f64>,
    /// Owning edit per existing vertex (kind 3).
    pub owners: SliceView<u32>,
    /// Flat xyz property ring; empty for none.
    pub property: SliceView<f64>,
    /// `x_min, y_min, x_max, y_max` when `has_domain` is 1.
    pub domain: [f64; 4],
    /// Plane height (kind 2), metres.
    pub plane: f64,
    /// Cell size, metres.
    pub cell: f64,
    /// Proposed kind.
    pub proposed: u32,
    /// Samples per cell per axis.
    pub subdivisions: u32,
    /// 0 or 1.
    pub has_domain: u32,
    /// Zero.
    pub reserved: u32,
}

/// Volumes over a set of samples.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainVolumes {
    /// Cut, cubic metres.
    pub cut: f64,
    /// Fill, cubic metres.
    pub fill: f64,
    /// `fill - cut`.
    pub net: f64,
    /// Sampled plan area, square metres.
    pub area: f64,
    /// Samples on both surfaces.
    pub samples: u32,
    /// Samples off either surface.
    pub excluded: u32,
}

/// One edit's share.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainEditVolumes {
    /// Cut it caused.
    pub cut: f64,
    /// Fill it caused.
    pub fill: f64,
    /// Its net.
    pub net: f64,
    /// Plan area it changed.
    pub area: f64,
    /// Edit index.
    pub edit: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned outputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainVolumeOutput {
    /// Required.
    pub site: *mut TerrainVolumes,
    /// Required.
    pub context: *mut TerrainVolumes,
    /// Required: two cell counts (x, y).
    pub cells: *mut u32,
    /// Required: edit rows published.
    pub edit_count: *mut usize,
    /// Per-edit rows, ascending edit.
    pub edits: *mut TerrainEditVolumes,
    /// Capacity, at least the distinct owners (kind 3; zero otherwise).
    pub edit_capacity: usize,
}

const fn volumes(v: kernel::Volumes) -> TerrainVolumes {
    TerrainVolumes {
        cut: v.cut,
        fill: v.fill,
        net: v.net,
        area: v.area,
        samples: v.samples,
        excluded: v.excluded,
    }
}

// SAFETY: request pools, outputs and error are preflighted disjoint first.
unsafe fn compute(
    input: *const TerrainVolumeRequest,
    output: *const TerrainVolumeOutput,
    error: Range,
) -> Result<(kernel::VolumeReport, TerrainVolumeOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend([
            memory_range(r.points.data, r.points.count),
            memory_range(r.triangles.data, r.triangles.count),
            memory_range(r.proposed_points.data, r.proposed_points.count),
            memory_range(r.proposed_triangles.data, r.proposed_triangles.count),
            memory_range(r.heights.data, r.heights.count),
            memory_range(r.owners.data, r.owners.count),
            memory_range(r.property.data, r.property.count),
        ]);
    }
    let writable = target.map_or_else(Vec::new, |t| {
        vec![
            memory_range(t.site.cast_const(), 1),
            memory_range(t.context.cast_const(), 1),
            memory_range(t.cells.cast_const(), 2),
            memory_range(t.edit_count.cast_const(), 1),
            memory_range(t.edits.cast_const(), t.edit_capacity),
        ]
    });
    pools::preflight(&inputs, &writable, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    // SAFETY: every pool was preflighted against outputs, descriptors and error.
    let (points, triangles, proposed_points, proposed_triangles, heights, owners, property) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.proposed_points)?,
            context.slice(request.proposed_triangles)?,
            context.slice(request.heights)?,
            context.slice(request.owners)?,
            context.slice(request.property)?,
        )
    };
    let failure = |message: &'static str| Failure::Argument(message);
    let existing = Field::new(points, triangles).map_err(|error| failure(error.message()))?;
    let other;
    let owned: Vec<Option<u32>>;
    let proposed = match request.proposed {
        1 => {
            other = Field::new(proposed_points, proposed_triangles)
                .map_err(|error| failure(error.message()))?;
            kernel::Proposed::Field(&other)
        }
        2 => kernel::Proposed::Plane(request.plane),
        3 => {
            owned = owners
                .iter()
                .map(|&owner| (owner != u32::MAX).then_some(owner))
                .collect();
            kernel::Proposed::Edited {
                heights,
                owners: &owned,
            }
        }
        _ => return Err(failure("TerrainVolumeKind")),
    };
    let domain = match request.has_domain {
        0 => None,
        1 => Some(request.domain),
        _ => return Err(failure("TerrainVolumeFlag")),
    };
    let report = kernel::cut_fill(&kernel::VolumeRequest {
        existing: &existing,
        proposed,
        grid: kernel::Grid {
            cell: request.cell,
            subdivisions: request.subdivisions,
        },
        domain,
        property: (!property.is_empty()).then_some(property),
    })
    .map_err(|error| failure(error.message()))?;
    if target.edit_capacity < report.edits.len() {
        return Err(failure("TerrainOutputTooSmall"));
    }
    Ok((report, target))
}

/// Grid cut/fill volumes between an existing mesh and a proposed surface.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_triangulate`, for the required records and edit rows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_volume(
    input: *const TerrainVolumeRequest,
    output: *const TerrainVolumeOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; preflight first.
    unsafe {
        call(error, capacity, || {
            let (report, target) =
                compute(input, output, memory_range(error.cast_const(), capacity)?)?;
            let rows: Vec<TerrainEditVolumes> = report
                .edits
                .iter()
                .map(|row| TerrainEditVolumes {
                    cut: row.cut,
                    fill: row.fill,
                    net: row.net,
                    area: row.area,
                    edit: row.edit,
                    reserved: 0,
                })
                .collect();
            // SAFETY: capacities checked; ranges disjoint; POD writes cannot unwind.
            if !rows.is_empty() {
                std::ptr::copy_nonoverlapping(rows.as_ptr(), target.edits, rows.len());
            }
            target.site.write(volumes(report.site));
            target.context.write(volumes(report.context));
            target.cells.write(report.cells[0]);
            target.cells.add(1).write(report.cells[1]);
            target.edit_count.write(rows.len());
            Ok(())
        })
    }
}
