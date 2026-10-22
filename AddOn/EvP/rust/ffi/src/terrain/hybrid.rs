//! Terrain hybrid ABI v1: one path band built on a base mesh inside a region
//! ring as a constrained TIN, with exact cut/fill and lossless support rows.
//!
//! Input provenance is the split's (domains=7): mesh vertices then region ring
//! vertices, ring segments as edges, base triangles as faces. Stations are
//! authored parameters and carry no entity rows. The output reuses the
//! [`TerrainMapping`] row record with kinds 9 and 12..17 (see the C header);
//! it never extends the pinned [`super::TerrainOutput`].

use super::edit::{TerrainSlope, slope};
use super::{
    BorrowContext, Failure, ProvenanceRequest, SliceView, TerrainMapping, attribution, call,
    memory_range,
};
use crate::pools::{self, Checked, Range};
use tapioca_geometry::identity as id;
use tapioca_geometry::terrain::daylight::Locus;
use tapioca_geometry::terrain::design::{Band, Camber, Station};
use tapioca_geometry::terrain::edit::{Crossfall, Effect, Hand};
use tapioca_geometry::terrain::hybrid::{self as kernel, Built, LineKind};
use tapioca_geometry::terrain::region::{RegionVertex, Support};
use tapioca_geometry::terrain::volume::exact::TriangleVolumes;
use tapioca_geometry::terrain::{Host, Origin};

/// One band station. `crossfall_hand`: 0 level, 1 falls to the left, 2 to the
/// right, 3 crown (both halves fall away from the centreline at `crossfall`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainStation {
    /// Centreline x, y and design height, world metres.
    pub position: [f64; 3],
    /// Left half width, metres.
    pub half_left: f64,
    /// Right half width, metres.
    pub half_right: f64,
    /// Level shoulder beyond the left edge, metres.
    pub shoulder_left: f64,
    /// Level shoulder beyond the right edge, metres.
    pub shoulder_right: f64,
    /// Largest cut daylight distance, metres.
    pub cut_limit: f64,
    /// Largest fill daylight distance, metres.
    pub fill_limit: f64,
    /// Cut face grade.
    pub cut: TerrainSlope,
    /// Fill face grade.
    pub fill: TerrainSlope,
    /// Crossfall or crown grade when `crossfall_hand` is nonzero.
    pub crossfall: TerrainSlope,
    /// Crossfall hand.
    pub crossfall_hand: u32,
    /// Zero.
    pub reserved: u32,
}

/// The base mesh, the region ring, the band and the input provenance.
/// `effect`: 1 cut, 2 fill, 3 both.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainHybridRequest {
    /// Flat xyz base vertices, world metres.
    pub points: SliceView<f64>,
    /// Counter-clockwise base vertex triples.
    pub triangles: SliceView<u32>,
    /// Implicitly closed flat xyz region ring (z ignored).
    pub region: SliceView<f64>,
    /// Stations in path order.
    pub stations: SliceView<TerrainStation>,
    /// At-grade height band, metres.
    pub tolerance: f64,
    /// Band effect.
    pub effect: u32,
    /// Zero.
    pub reserved: u32,
    /// Domains exactly vertex+edge+face (7): mesh then ring vertices, ring
    /// segments, base triangles; no patches.
    pub provenance: ProvenanceRequest,
}

/// Scalar element counts of a complete hybrid result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainHybridCounts {
    /// xyz scalar count.
    pub points: usize,
    /// Triangle index scalar count.
    pub triangles: usize,
    /// Support row count.
    pub mappings: usize,
    /// Design line count.
    pub lines: usize,
}

/// Exact volumes over a set of triangles.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainHybridVolumes {
    /// Cut, cubic metres.
    pub cut: f64,
    /// Fill, cubic metres.
    pub fill: f64,
    /// `fill - cut`.
    pub net: f64,
    /// Plan area, square metres.
    pub area: f64,
    /// Triangles summed.
    pub triangles: u32,
    /// Zero.
    pub reserved: u32,
}

/// What a design line is. `kind`: 1 band edge, 2 shoulder end, 3 daylight, 4
/// end section, 5 crowned centreline. `side`: 1 left, 2 right (0 for an end). `root`: the daylight
/// root counted outward, or 0 first / 1 last for an end (0 for a centreline).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TerrainHybridLine {
    /// Line kind.
    pub kind: u32,
    /// Side.
    pub side: u32,
    /// Root or end.
    pub root: u32,
    /// Zero.
    pub reserved: u32,
}

/// One station side's daylight. `roots` 0 means the side is at grade.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainHybridSide {
    /// First root, metres outward from the band edge.
    pub first: f64,
    /// Last root, metres outward from the band edge.
    pub last: f64,
    /// Root count.
    pub roots: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned disjoint result storage. `counts`, `site` and `context` are
/// required; query mode publishes only `counts` and needs zero capacities.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TerrainHybridOutput {
    /// Required counts record.
    pub counts: *mut TerrainHybridCounts,
    /// Required site (inside the region) volumes.
    pub site: *mut TerrainHybridVolumes,
    /// Required context (outside) volumes.
    pub context: *mut TerrainHybridVolumes,
    /// Proposed xyz.
    pub points: *mut f64,
    /// Scalar capacity.
    pub point_capacity: usize,
    /// Base height per output vertex.
    pub base: *mut f64,
    /// Capacity, at least the vertex count.
    pub base_capacity: usize,
    /// Per output vertex 0 when the band changed its height, else `u32::MAX`.
    pub owners: *mut u32,
    /// Capacity, at least the vertex count.
    pub owner_capacity: usize,
    /// Output vertex triples.
    pub triangles: *mut u32,
    /// Scalar capacity.
    pub triangle_capacity: usize,
    /// Support rows.
    pub mappings: *mut TerrainMapping,
    /// Row capacity.
    pub mapping_capacity: usize,
    /// Design lines, in mapping line order.
    pub lines: *mut TerrainHybridLine,
    /// Capacity.
    pub line_capacity: usize,
    /// `[left, right]` per station.
    pub sides: *mut TerrainHybridSide,
    /// Capacity, at least twice the station count.
    pub side_capacity: usize,
    /// 1 publishes only counts; 0 publishes the complete result.
    pub query_only: u32,
    /// Zero.
    pub reserved: u32,
}

fn writable(target: TerrainHybridOutput) -> Vec<Checked> {
    vec![
        memory_range(target.counts.cast_const(), 1),
        memory_range(target.site.cast_const(), 1),
        memory_range(target.context.cast_const(), 1),
        memory_range(target.points.cast_const(), target.point_capacity),
        memory_range(target.base.cast_const(), target.base_capacity),
        memory_range(target.owners.cast_const(), target.owner_capacity),
        memory_range(target.triangles.cast_const(), target.triangle_capacity),
        memory_range(target.mappings.cast_const(), target.mapping_capacity),
        memory_range(target.lines.cast_const(), target.line_capacity),
        memory_range(target.sides.cast_const(), target.side_capacity),
    ]
}

fn index(value: usize) -> Result<u32, Failure> {
    u32::try_from(value).map_err(|_| Failure::Argument("TerrainCountOverflow"))
}

fn count(value: usize) -> Result<u64, Failure> {
    u64::try_from(value).map_err(|_| Failure::Argument("TerrainCountOverflow"))
}

fn station(raw: &TerrainStation) -> Result<Station, Failure> {
    if raw.reserved != 0 {
        return Err(Failure::Argument("TerrainHybridReserved"));
    }
    Ok(Station {
        position: raw.position,
        half_left: raw.half_left,
        half_right: raw.half_right,
        crossfall: match raw.crossfall_hand {
            0 => None,
            hand @ (1 | 2) => Some(Camber::OneWay(Crossfall {
                slope: slope(raw.crossfall)?,
                falls_to: if hand == 1 { Hand::Left } else { Hand::Right },
            })),
            3 => Some(Camber::Crown(slope(raw.crossfall)?)),
            _ => return Err(Failure::Argument("TerrainHybridHand")),
        },
        shoulder_left: raw.shoulder_left,
        shoulder_right: raw.shoulder_right,
        cut: slope(raw.cut)?,
        fill: slope(raw.fill)?,
        cut_limit: raw.cut_limit,
        fill_limit: raw.fill_limit,
    })
}

const fn hand(side: Hand) -> u32 {
    match side {
        Hand::Left => 1,
        Hand::Right => 2,
    }
}

fn row(kind: u32, index: u32, values: [u32; 6]) -> TerrainMapping {
    TerrainMapping {
        kind,
        index,
        values,
    }
}

const fn support(value: Support) -> [u32; 3] {
    match value {
        Support::BaseEdge([a, b]) => [1, a, b],
        Support::Breakline { line, segment } => [2, line, segment],
    }
}

fn volumes(value: TriangleVolumes) -> TerrainHybridVolumes {
    TerrainHybridVolumes {
        cut: value.cut,
        fill: value.fill,
        net: value.net,
        area: value.area,
        triangles: value.triangles,
        reserved: 0,
    }
}

/// The owned result, encoded before publication.
pub(super) struct Encoded {
    pub(super) points: Vec<f64>,
    pub(super) base: Vec<f64>,
    pub(super) owners: Vec<u32>,
    pub(super) triangles: Vec<u32>,
    pub(super) mappings: Vec<TerrainMapping>,
    pub(super) lines: Vec<TerrainHybridLine>,
    pub(super) sides: Vec<TerrainHybridSide>,
    pub(super) site: TerrainHybridVolumes,
    pub(super) context: TerrainHybridVolumes,
}

/// Encode a build losslessly: faces (12), split vertices (9), region vertices
/// (13, 14), design line vertices (15) and edges (16), the ring (17).
pub(super) fn encode(built: Built) -> Result<Encoded, Failure> {
    let Built {
        points,
        mesh,
        lines,
        owners,
        daylight,
        volumes: report,
    } = built;
    let mut mappings = Vec::new();
    for (face, ((&parent, &inside), &kept)) in mesh
        .parents
        .iter()
        .zip(&mesh.inside)
        .zip(&mesh.kept)
        .enumerate()
    {
        mappings.push(row(
            12,
            index(face)?,
            [parent, u32::from(inside), u32::from(kept), 0, 0, 0],
        ));
    }
    let input = mesh.input_vertices as usize;
    for (i, vertex) in mesh.split_vertices.iter().enumerate() {
        let (origin, source) = match vertex.origin {
            Origin::LineVertex(n) => (1, n),
            Origin::Crossing(n) => (2, n),
        };
        let (host, a, b) = match vertex.host {
            Host::Edge([a, b]) => (1, a, b),
            Host::Face(n) => (2, n, 0),
        };
        let at = input
            .checked_add(i)
            .ok_or(Failure::Argument("TerrainCountOverflow"))?;
        mappings.push(row(9, index(at)?, [origin, source, host, a, b, 0]));
    }
    let appended = input
        .checked_add(mesh.split_vertices.len())
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    for (i, vertex) in mesh.new_vertices.iter().enumerate() {
        let at = index(
            appended
                .checked_add(i)
                .ok_or(Failure::Argument("TerrainCountOverflow"))?,
        )?;
        match *vertex {
            RegionVertex::Breakline { line, vertex } => {
                mappings.push(row(13, at, [line, vertex, 0, 0, 0, 0]));
            }
            RegionVertex::Crossing(pair) => {
                for value in pair {
                    let [kind, a, b] = support(value);
                    mappings.push(row(14, at, [kind, a, b, 0, 0, 0]));
                }
            }
        }
    }
    for (line, vertices) in mesh.breakline_vertices.iter().enumerate() {
        for (ordinal, &vertex) in vertices.iter().enumerate() {
            mappings.push(row(15, index(line)?, [index(ordinal)?, vertex, 0, 0, 0, 0]));
        }
    }
    for edge in &mesh.line_edges {
        mappings.push(row(
            16,
            edge.line,
            [edge.segment, edge.vertices[0], edge.vertices[1], 0, 0, 0],
        ));
    }
    for (position, &vertex) in mesh.ring.iter().enumerate() {
        mappings.push(row(17, index(position)?, [vertex, 0, 0, 0, 0, 0]));
    }
    let lines = lines
        .iter()
        .map(|kind| {
            let (kind, side, root) = match *kind {
                LineKind::Edge(side) => (1, hand(side), 0),
                LineKind::Shoulder(side) => (2, hand(side), 0),
                LineKind::Daylight { side, root } => (3, hand(side), root),
                LineKind::End(last) => (4, 0, u32::from(last)),
                LineKind::Crown => (5, 0, 0),
            };
            TerrainHybridLine {
                kind,
                side,
                root,
                reserved: 0,
            }
        })
        .collect();
    let mut sides = Vec::with_capacity(daylight.len() * 2);
    for pair in &daylight {
        for side in pair {
            // The build refuses every locus but Met, so anything else is at grade.
            let roots: &[f64] = match side {
                Some(Locus::Met { roots }) => roots,
                _ => &[],
            };
            sides.push(TerrainHybridSide {
                first: roots.first().copied().unwrap_or(0.0),
                last: roots.last().copied().unwrap_or(0.0),
                roots: index(roots.len())?,
                reserved: 0,
            });
        }
    }
    Ok(Encoded {
        base: plan_heights(&mesh.points),
        owners: owners
            .iter()
            .map(|owner| owner.unwrap_or(u32::MAX))
            .collect(),
        points,
        triangles: mesh.triangles,
        mappings,
        lines,
        sides,
        site: volumes(report.site),
        context: volumes(report.context),
    })
}

fn plan_heights(points: &[f64]) -> Vec<f64> {
    points
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[_, _, z]| z)
        .collect()
}

// SAFETY: every top-level pool is preflighted disjoint from all outputs and
// the error buffer before any foreign borrow.
unsafe fn compute(
    input: *const TerrainHybridRequest,
    output: *const TerrainHybridOutput,
    error: Range,
) -> Result<(Encoded, TerrainHybridOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(r) = &request {
        inputs.extend(pools::provenance(&r.provenance));
        inputs.extend([
            memory_range(r.points.data, r.points.count),
            memory_range(r.triangles.data, r.triangles.count),
            memory_range(r.region.data, r.region.count),
            memory_range(r.stations.data, r.stations.count),
        ]);
    }
    let outputs = target.map_or_else(Vec::new, writable);
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    if target.query_only > 1
        || target.reserved != 0
        || (target.query_only == 1
            && (target.point_capacity
                | target.base_capacity
                | target.owner_capacity
                | target.triangle_capacity
                | target.mapping_capacity
                | target.line_capacity
                | target.side_capacity)
                != 0)
    {
        return Err(Failure::Argument("TerrainOutputMode"));
    }
    // SAFETY: every pool was preflighted above; no borrow escapes this call.
    let (points, triangles, region, raw_stations, provenance) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.region)?,
            context.slice(request.stations)?,
            context.provenance(&request.provenance)?,
        )
    };
    let ring = region.len() / 3;
    attribution(
        &provenance,
        id::EntityCounts {
            vertices: count(points.len() / 3)?
                .checked_add(count(ring)?)
                .ok_or(Failure::Argument("TerrainCountOverflow"))?,
            edges: count(ring)?,
            faces: count(triangles.len() / 3)?,
            patches: 0,
        },
        7,
    )?;
    if request.reserved != 0 {
        return Err(Failure::Argument("TerrainHybridReserved"));
    }
    let effect = match request.effect {
        1 => Effect::Cut,
        2 => Effect::Fill,
        3 => Effect::Both,
        _ => return Err(Failure::Argument("TerrainHybridEffect")),
    };
    let stations = raw_stations
        .iter()
        .map(station)
        .collect::<Result<Vec<_>, _>>()?;
    let built = kernel::build(&kernel::BuildRequest {
        points,
        triangles,
        region,
        band: Band {
            stations: &stations,
            effect,
        },
        tolerance: request.tolerance,
    })
    .map_err(|error| Failure::Argument(error.message()))?;
    Ok((encode(built)?, target))
}

/// Copy a nonempty POD prefix.
///
/// # Safety
/// `to` holds at least `from.len()` exclusively writable elements.
unsafe fn copy<T: Copy>(from: &[T], to: *mut T) {
    if !from.is_empty() {
        // SAFETY: capacity and disjointness were verified by the caller.
        unsafe { std::ptr::copy_nonoverlapping(from.as_ptr(), to, from.len()) };
    }
}

// SAFETY: target ranges were preflighted disjoint from every input, the error
// buffer and each other.
unsafe fn publish(data: &Encoded, target: TerrainHybridOutput) -> Result<(), Failure> {
    let counts = TerrainHybridCounts {
        points: data.points.len(),
        triangles: data.triangles.len(),
        mappings: data.mappings.len(),
        lines: data.lines.len(),
    };
    if target.query_only == 0
        && (target.point_capacity < counts.points
            || target.base_capacity < data.base.len()
            || target.owner_capacity < data.owners.len()
            || target.triangle_capacity < counts.triangles
            || target.mapping_capacity < counts.mappings
            || target.line_capacity < counts.lines
            || target.side_capacity < data.sides.len())
    {
        return Err(Failure::Argument("TerrainOutputTooSmall"));
    }
    // SAFETY: every capacity was verified before ANY write; POD copies cannot unwind.
    unsafe {
        if target.query_only == 0 {
            copy(&data.points, target.points);
            copy(&data.base, target.base);
            copy(&data.owners, target.owners);
            copy(&data.triangles, target.triangles);
            copy(&data.mappings, target.mappings);
            copy(&data.lines, target.lines);
            copy(&data.sides, target.sides);
            target.site.write(data.site);
            target.context.write(data.context);
        }
        target.counts.write(counts);
    }
    Ok(())
}

/// Build one path band on a base mesh inside a region ring.
/// Query mode computes the same result and publishes only counts.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_triangulate`. Attribution refusals return STATE;
/// geometry/parameter refusals ARG with the kernel's stable code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_hybrid_build(
    input: *const TerrainHybridRequest,
    output: *const TerrainHybridOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; full preflight
    // precedes computation and capacity checks precede every write.
    unsafe {
        call(error, capacity, || {
            let (data, target) =
                compute(input, output, memory_range(error.cast_const(), capacity)?)?;
            publish(&data, target)
        })
    }
}
