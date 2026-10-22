//! Terrain edit ABI v1: local operators over a mesh's vertex heights, with
//! mandatory schema-v1 input provenance (mesh vertices and faces, domains=5).
//!
//! Edit geometry (curves, paths, rings, the clip and protected rings) is read
//! from one flat xyz pool by scalar range; target surfaces are separate mesh
//! views. Outputs are caller-owned: heights and owners (one per vertex), face
//! owner rows (count published), and one report per edit.

use super::{
    BorrowContext, Failure, ProvenanceRequest, SliceView, attribution, call, memory_range,
};
use crate::pools::{self, Checked, Range};
use tapioca_geometry::identity as id;
use tapioca_geometry::slope::Slope;
use tapioca_geometry::terrain::edit as kernel;
use tapioca_geometry::terrain::field::Field;

/// A slope in its authored unit: kind 1 angle (`a` radians), 2 percent (`a`),
/// 3 rise:run (`a`:`b`), 4 height (`a` metres over `b` metres).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainSlope {
    /// First value.
    pub a: f64,
    /// Second value (kinds 3 and 4).
    pub b: f64,
    /// 1..4.
    pub kind: u32,
    /// Zero.
    pub reserved: u32,
}

/// One daylight side.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainDaylight {
    /// Face grade.
    pub slope: TerrainSlope,
    /// Extension in metres when `has_extension` is 1.
    pub extension: f64,
    /// 0 or 1.
    pub has_extension: u32,
    /// Zero.
    pub reserved: u32,
}

/// One edit. `target`: 1 point, 2 curve, 3 area, 4 path, 5 surface. `mode`: 1
/// absolute, 2 relative. `effect`: 1 cut, 2 fill, 3 both. `transition`: 1
/// falloff, 2 daylight. `level_kind` (area): 1 height, 2 flatten.
/// `crossfall_hand` (path): 0 none, 1 left, 2 right.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainEdit {
    /// Point x, y and height/displacement.
    pub position: [f64; 3],
    /// Point radius, curve half width or path full width, metres.
    pub width: f64,
    /// Area level (height or displacement).
    pub level: f64,
    /// Falloff blur, metres.
    pub blur: f64,
    /// Maximum distance outside the core, metres.
    pub max_distance: f64,
    /// Displacement scale in `(0, 1]`.
    pub strength: f64,
    /// Daylight cut side.
    pub cut: TerrainDaylight,
    /// Daylight fill side.
    pub fill: TerrainDaylight,
    /// Path crossfall slope.
    pub crossfall: TerrainSlope,
    /// Target kind.
    pub target: u32,
    /// Height mode.
    pub mode: u32,
    /// Cut/fill/both.
    pub effect: u32,
    /// Falloff/daylight.
    pub transition: u32,
    /// Scalar offset of the curve/path/ring in the geometry pool.
    pub geometry_offset: u32,
    /// Scalar count (xyz triples).
    pub geometry_count: u32,
    /// Area level kind.
    pub level_kind: u32,
    /// Path crossfall hand.
    pub crossfall_hand: u32,
    /// Surface index (target 5).
    pub surface: u32,
    /// Zero.
    pub reserved: u32,
}

/// A target surface's mesh.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainSurfaceView {
    /// Flat xyz world metres.
    pub points: SliceView<f64>,
    /// Vertex triples.
    pub triangles: SliceView<u32>,
}

/// A scalar range of the geometry pool holding one ring.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainRing {
    /// Scalar offset.
    pub offset: u32,
    /// Scalar count (xyz triples, at least three vertices).
    pub count: u32,
}

/// The mesh, edits, their geometry and the input provenance.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainEditRequest {
    /// Flat xyz mesh vertices, world metres.
    pub points: SliceView<f64>,
    /// Mesh vertex triples.
    pub triangles: SliceView<u32>,
    /// Edits in operation order.
    pub edits: SliceView<TerrainEdit>,
    /// Flat xyz edit geometry pool.
    pub geometry: SliceView<f64>,
    /// Target surfaces.
    pub surfaces: SliceView<TerrainSurfaceView>,
    /// Protected rings in the geometry pool.
    pub protected: SliceView<TerrainRing>,
    /// The property line in the geometry pool, when `has_clip` is 1.
    pub clip: TerrainRing,
    /// 0 or 1.
    pub has_clip: u32,
    /// 0 or 1.
    pub has_smoothing: u32,
    /// Smoothing radius, metres.
    pub smoothing: f64,
    /// Domains exactly vertex+face (5): mesh vertices, mesh triangles.
    pub provenance: ProvenanceRequest,
}

/// A changed face and an edit that moved one of its vertices.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainFaceOwner {
    /// Triangle index.
    pub face: u32,
    /// Edit index.
    pub edit: u32,
}

/// One edit's counts.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainEditReport {
    /// Vertices it moved.
    pub moved: u32,
    /// Vertices beyond a daylight extension it would have moved.
    pub extension_clipped: u32,
    /// Vertices beyond the maximum distance it would have moved.
    pub limit_clipped: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned outputs. `owners` holds `u32::MAX` for an unmoved vertex.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainEditOutput {
    /// Required: face owner rows published.
    pub face_count: *mut usize,
    /// One height per vertex.
    pub heights: *mut f64,
    /// Capacity, at least the vertex count.
    pub height_capacity: usize,
    /// One owner per vertex.
    pub owners: *mut u32,
    /// Capacity, at least the vertex count.
    pub owner_capacity: usize,
    /// Face owner rows; at most three per triangle.
    pub faces: *mut TerrainFaceOwner,
    /// Capacity, at least the rows produced.
    pub face_capacity: usize,
    /// One report per edit.
    pub reports: *mut TerrainEditReport,
    /// Capacity, at least the edit count.
    pub report_capacity: usize,
}

pub(super) fn slope(value: TerrainSlope) -> Result<Slope, Failure> {
    Ok(match value.kind {
        1 => Slope::Angle { radians: value.a },
        2 => Slope::Percent(value.a),
        3 => Slope::RiseRun {
            rise: value.a,
            run: value.b,
        },
        4 => Slope::Height {
            rise_metres: value.a,
            run_metres: value.b,
        },
        _ => return Err(Failure::Argument("TerrainSlopeKind")),
    })
}

fn daylight(side: TerrainDaylight) -> Result<kernel::Daylight, Failure> {
    Ok(kernel::Daylight {
        slope: slope(side.slope)?,
        extension: match side.has_extension {
            0 => None,
            1 => Some(side.extension),
            _ => return Err(Failure::Argument("TerrainEditFlag")),
        },
    })
}

fn range(pool: &[f64], offset: u32, count: u32) -> Result<&[f64], Failure> {
    let start = offset as usize;
    let end = start
        .checked_add(count as usize)
        .ok_or(Failure::Argument("TerrainGeometryRange"))?;
    pool.get(start..end)
        .ok_or(Failure::Argument("TerrainGeometryRange"))
}

fn edit<'a>(
    raw: &TerrainEdit,
    geometry: &'a [f64],
    fields: &'a [Field],
) -> Result<kernel::Edit<'a>, Failure> {
    let pool = || range(geometry, raw.geometry_offset, raw.geometry_count);
    let target = match raw.target {
        1 => kernel::Target::Point {
            position: raw.position,
            radius: raw.width,
        },
        2 => kernel::Target::Curve {
            points: pool()?,
            half_width: raw.width,
        },
        3 => kernel::Target::Area {
            ring: pool()?,
            level: match raw.level_kind {
                1 => kernel::AreaLevel::Height(raw.level),
                2 => kernel::AreaLevel::Flatten,
                _ => return Err(Failure::Argument("TerrainEditKind")),
            },
        },
        4 => kernel::Target::Path {
            centreline: pool()?,
            width: raw.width,
            crossfall: match raw.crossfall_hand {
                0 => None,
                hand @ (1 | 2) => Some(kernel::Crossfall {
                    slope: slope(raw.crossfall)?,
                    falls_to: if hand == 1 {
                        kernel::Hand::Left
                    } else {
                        kernel::Hand::Right
                    },
                }),
                _ => return Err(Failure::Argument("TerrainEditKind")),
            },
        },
        5 => kernel::Target::Surface {
            field: fields
                .get(raw.surface as usize)
                .ok_or(Failure::Argument("TerrainSurfaceIndex"))?,
        },
        _ => return Err(Failure::Argument("TerrainEditKind")),
    };
    Ok(kernel::Edit {
        target,
        mode: match raw.mode {
            1 => kernel::Mode::Absolute,
            2 => kernel::Mode::Relative,
            _ => return Err(Failure::Argument("TerrainEditKind")),
        },
        effect: match raw.effect {
            1 => kernel::Effect::Cut,
            2 => kernel::Effect::Fill,
            3 => kernel::Effect::Both,
            _ => return Err(Failure::Argument("TerrainEditKind")),
        },
        transition: match raw.transition {
            1 => kernel::Transition::Falloff { blur: raw.blur },
            2 => kernel::Transition::Daylight {
                cut: daylight(raw.cut)?,
                fill: daylight(raw.fill)?,
            },
            _ => return Err(Failure::Argument("TerrainEditKind")),
        },
        max_distance: raw.max_distance,
        strength: raw.strength,
    })
}

fn outputs(target: TerrainEditOutput) -> Vec<Checked> {
    vec![
        memory_range(target.face_count.cast_const(), 1),
        memory_range(target.heights.cast_const(), target.height_capacity),
        memory_range(target.owners.cast_const(), target.owner_capacity),
        memory_range(target.faces.cast_const(), target.face_capacity),
        memory_range(target.reports.cast_const(), target.report_capacity),
    ]
}

/// The computed result, owned before publication.
struct Computed {
    edited: kernel::Edited,
    target: TerrainEditOutput,
}

// SAFETY: every pool, including each surface's nested pools, is preflighted
// disjoint from outputs and error before any foreign borrow or write.
pub(super) unsafe fn with_edit<O: Copy>(
    input: *const TerrainEditRequest,
    output: *const O,
    error: Range,
    writable: fn(O) -> Vec<Checked>,
    sized: impl FnOnce(&O, usize, usize) -> Result<(), Failure>,
) -> Result<(kernel::Edited, O), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend(pools::provenance(&r.provenance));
        inputs.extend([
            memory_range(r.points.data, r.points.count),
            memory_range(r.triangles.data, r.triangles.count),
            memory_range(r.edits.data, r.edits.count),
            memory_range(r.geometry.data, r.geometry.count),
            memory_range(r.surfaces.data, r.surfaces.count),
            memory_range(r.protected.data, r.protected.count),
        ]);
    }
    let writable = target.map_or_else(Vec::new, writable);
    pools::preflight(&inputs, &writable, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    // SAFETY: the surface descriptor array itself was preflighted above.
    let surfaces = unsafe { context.slice(request.surfaces)? };
    let nested: Vec<Checked> = surfaces
        .iter()
        .flat_map(|s| {
            [
                memory_range(s.points.data, s.points.count),
                memory_range(s.triangles.data, s.triangles.count),
            ]
        })
        .collect();
    pools::preflight(&nested, &writable, error)?;
    // SAFETY: every top-level and nested pool is now preflighted.
    let (points, triangles, edits, geometry, protected, provenance) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.edits)?,
            context.slice(request.geometry)?,
            context.slice(request.protected)?,
            context.provenance(&request.provenance)?,
        )
    };
    let count = |n: usize| u64::try_from(n).map_err(|_| Failure::Argument("TerrainCountOverflow"));
    attribution(
        &provenance,
        id::EntityCounts {
            vertices: count(points.len() / 3)?,
            edges: 0,
            faces: count(triangles.len() / 3)?,
            patches: 0,
        },
        5,
    )?;
    sized(&target, points.len() / 3, edits.len())?;
    let mut fields = Vec::with_capacity(surfaces.len());
    for surface in surfaces {
        // SAFETY: both nested pools were preflighted in the second pass.
        let (mesh, faces) = unsafe {
            (
                context.slice(surface.points)?,
                context.slice(surface.triangles)?,
            )
        };
        fields.push(Field::new(mesh, faces).map_err(|error| Failure::Argument(error.message()))?);
    }
    let kernel_edits = edits
        .iter()
        .map(|raw| edit(raw, geometry, &fields))
        .collect::<Result<Vec<_>, _>>()?;
    let rings = protected
        .iter()
        .map(|ring| range(geometry, ring.offset, ring.count))
        .collect::<Result<Vec<_>, _>>()?;
    let clip = match request.has_clip {
        0 => None,
        1 => Some(range(geometry, request.clip.offset, request.clip.count)?),
        _ => return Err(Failure::Argument("TerrainEditFlag")),
    };
    let smoothing = match request.has_smoothing {
        0 => None,
        1 => Some(request.smoothing),
        _ => return Err(Failure::Argument("TerrainEditFlag")),
    };
    let edited = kernel::edit_terrain(&kernel::EditRequest {
        points,
        triangles,
        edits: &kernel_edits,
        clip,
        protected: &rings,
        smoothing,
    })
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((edited, target))
}

// SAFETY: as [`with_edit`].
unsafe fn compute(
    input: *const TerrainEditRequest,
    output: *const TerrainEditOutput,
    error: Range,
) -> Result<Computed, Failure> {
    // SAFETY: forwarded unchanged; with_edit preflights everything first.
    let (edited, target) = unsafe {
        with_edit(input, output, error, outputs, |target, vertices, edits| {
            if target.height_capacity < vertices
                || target.owner_capacity < vertices
                || target.report_capacity < edits
            {
                return Err(Failure::Argument("TerrainOutputTooSmall"));
            }
            Ok(())
        })?
    };
    if target.face_capacity < edited.faces.len() {
        return Err(Failure::Argument("TerrainOutputTooSmall"));
    }
    Ok(Computed { edited, target })
}

// SAFETY: target ranges were preflighted disjoint and checked large enough.
unsafe fn publish(result: &Computed) {
    let Computed { edited, target } = result;
    let owners: Vec<u32> = edited
        .owners
        .iter()
        .map(|owner| owner.unwrap_or(u32::MAX))
        .collect();
    let faces: Vec<TerrainFaceOwner> = edited
        .faces
        .iter()
        .map(|row| TerrainFaceOwner {
            face: row.face,
            edit: row.edit,
        })
        .collect();
    let reports: Vec<TerrainEditReport> = edited
        .reports
        .iter()
        .map(|report| TerrainEditReport {
            moved: report.moved,
            extension_clipped: report.extension_clipped,
            limit_clipped: report.limit_clipped,
            reserved: 0,
        })
        .collect();
    // SAFETY: every capacity was verified before this call; POD copies cannot unwind.
    unsafe {
        if !edited.heights.is_empty() {
            std::ptr::copy_nonoverlapping(
                edited.heights.as_ptr(),
                target.heights,
                edited.heights.len(),
            );
            std::ptr::copy_nonoverlapping(owners.as_ptr(), target.owners, owners.len());
        }
        if !faces.is_empty() {
            std::ptr::copy_nonoverlapping(faces.as_ptr(), target.faces, faces.len());
        }
        if !reports.is_empty() {
            std::ptr::copy_nonoverlapping(reports.as_ptr(), target.reports, reports.len());
        }
        target.face_count.write(faces.len());
    }
}

/// Apply local terrain edits to a mesh's vertex heights.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_triangulate`, extended to each surface's nested pools.
/// Attribution refusals return STATE; geometry/parameter refusals ARG.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_edit(
    input: *const TerrainEditRequest,
    output: *const TerrainEditOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; preflight first.
    unsafe {
        call(error, capacity, || {
            let result = compute(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&result);
            Ok(())
        })
    }
}
