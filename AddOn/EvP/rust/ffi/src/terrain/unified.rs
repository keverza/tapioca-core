//! Unified landscape-build ABI v1: one immutable baseline, one ordered field
//! plan and one constrained result topology (issue #112 P3).
//!
//! Ingress is fixed-layout POD with offset pools only (no nested pointers):
//! the baseline mesh, the property ring, one shared xyz geometry pool with a
//! ring pool (entry/mask domain parts), shared station/source/part-index
//! pools, ordered entries, path bands, masks and controls. Input provenance
//! is the split/hybrid catalog (domains=7): mesh vertices, then property,
//! entry-ring, mask-ring and control-line vertices, then point controls;
//! edges are the matching segments; faces are baseline triangles. Stations,
//! entry surface/terrain parameters and pool indices carry no entity rows.
//! The output reuses [`TerrainMapping`] kinds 9, 12..17 and 20 for the region
//! correspondence and publishes per-breakline origins, resolved locked
//! vertices, site/context volumes and the caller's revision/plan hash.
//! Control spots publish kind 20 (new spot vertex) or 21 (spot remapped to an
//! output vertex) rows. Entry ring segments coinciding with the property ring
//! are dropped before the rebuild (the ring is already constrained there).
//! Query/fill recomputes; publication is atomic.

use super::edit::{TerrainRing, TerrainSlope, slope};
use super::hybrid::{TerrainHybridVolumes, copy, station, support};
use super::{
    BorrowContext, Failure, ProvenanceRequest, SliceView, TerrainMapping, attribution, call,
    memory_range,
};
use crate::pools::{self, Checked, Range};
use tapioca_geometry::identity as id;
use tapioca_geometry::terrain::design::Band;
use tapioca_geometry::terrain::edit::{Effect, Hand};
use tapioca_geometry::terrain::hybrid::LineKind;
use tapioca_geometry::terrain::region::RegionVertex;
use tapioca_geometry::terrain::volume::exact::TriangleVolumes;
use tapioca_geometry::terrain::{self as kernel};

/// One domain part: ring `first_ring` is the outer, the next `ring_count - 1`
/// rings are holes, all indexing the shared rings pool.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UnifiedPart {
    /// First ring index; always at least one ring (the outer).
    pub first_ring: u32,
    /// Ring count, one plus the hole count.
    pub ring_count: u32,
}

/// One ordered region-field entry. `surface`: 1 Existing, 2 Constant at
/// `level`, 3 Triangulated using `surface_points` in the shared xyz pool and
/// `surface_triangles` in the surface-index pool. `effect`: 1 Grade, 2 Add,
/// 3 Subtract. `transition`: 1 Falloff over `falloff` metres, 2 Daylight with
/// `cut`/`fill` grades over `reach` metres. Parts and sources are index ranges
/// into the shared part-index/source pools; target meshes carry no entity rows.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedEntry {
    /// Durable owning object identity.
    pub object: u64,
    /// Durable root-operation identity.
    pub root_operation: u64,
    /// Constant target height, metres; used when `surface` is 2.
    pub level: f64,
    /// Falloff blur outside the domain, metres; used when `transition` is 1.
    pub falloff: f64,
    /// Daylight cut grade; used when `transition` is 2.
    pub cut: TerrainSlope,
    /// Daylight fill grade; used when `transition` is 2.
    pub fill: TerrainSlope,
    /// Maximum daylight reach outside the domain, metres; transition 2.
    pub reach: f64,
    /// Target xyz range in the shared geometry pool when `surface` is 3.
    pub surface_points: TerrainRing,
    /// Target triangle range in the shared surface-index pool when `surface` is 3.
    pub surface_triangles: TerrainRing,
    /// Contiguous stack index; later entries override earlier entries.
    pub order: u32,
    /// Surface target kind, 1 or 2.
    pub surface: u32,
    /// Vertical effect, 1..3.
    pub effect: u32,
    /// Transition kind, 1 or 2.
    pub transition: u32,
    /// First part index into the shared part-index pool.
    pub first_part: u32,
    /// Number of part indices.
    pub part_count: u32,
    /// First source index into the shared source pool.
    pub first_source: u32,
    /// Number of source identities.
    pub source_count: u32,
}

/// One ordered path band. `effect`: 1 cut, 2 fill, 3 both. Stations are an
/// index range into the shared stations pool and carry no entity rows; path
/// lines are derived in Rust like the hybrid build's.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedPath {
    /// Stack position after every region entry.
    pub order: u32,
    /// Band effect, 1..3.
    pub effect: u32,
    /// Zero.
    pub reserved: u32,
    /// First station index into the shared stations pool, at least two.
    pub first_station: u32,
    /// Number of sampled stations in travel order.
    pub station_count: u32,
    /// Authored path-centerline source identity.
    pub source: u64,
    /// Durable path-operation identity.
    pub operation: u64,
    /// One terrain-owning path object.
    pub object: u64,
    /// At-grade height band for the design classification, metres.
    pub tolerance: f64,
}

/// One grading limit or protected zone. `kind`: 1 `GradingLimit`, 2
/// `ProtectedZone`. Parts and sources are index ranges into the shared
/// part-index/source pools.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedMask {
    /// Mask kind, 1 or 2.
    pub kind: u32,
    /// Zero.
    pub reserved: u32,
    /// Durable mask-operation identity.
    pub operation: u64,
    /// First part index into the shared part-index pool.
    pub first_part: u32,
    /// Number of part indices.
    pub part_count: u32,
    /// First source index into the shared source pool.
    pub first_source: u32,
    /// Number of source identities.
    pub source_count: u32,
}

/// One fixed-height point or open straight line control. When `point` is 1,
/// `position` is the fixed point; otherwise `line` selects an open polyline
/// from the shared geometry pool.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedControl {
    /// 0 for a fixed open line, 1 for a fixed point.
    pub point: u32,
    /// Zero.
    pub reserved: u32,
    /// Authored source identity.
    pub source: u64,
    /// Authored vertex identity of a point control; zero for a line control.
    pub vertex: u64,
    /// Fixed xyz position of a point control, world metres.
    pub position: [f64; 3],
    /// Fixed open polyline of a line control, in the shared geometry pool.
    pub line: TerrainRing,
}

/// One paved area graded from its outline (#149 T1). `effect`: 1 cut, 2 fill,
/// 3 both. The paved surface and spots are ranges in the shared geometry pool,
/// its triangles a range in the surface-index pool, its per-vertex grades a
/// range of (cut, fill, rounding) triples in `outline_grades` (empty: every
/// face takes `cut`/`fill`/`rounding`), and its face lines `line_count` ranges
/// from `first_line` in `outline_lines`. None of them carries entity rows.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedOutline {
    /// Face grade where the outline is below the ground.
    pub cut: TerrainSlope,
    /// Face grade where the outline is above the ground.
    pub fill: TerrainSlope,
    /// Plan reach of a face from the outline, metres.
    pub limit: f64,
    /// Plan length each face rounds over at its top and toe, metres.
    pub rounding: f64,
    /// At-grade height band, metres.
    pub tolerance: f64,
    /// Paved xyz vertices in the shared geometry pool.
    pub surface_points: TerrainRing,
    /// Paved triangle indices in the shared surface-index pool.
    pub surface_triangles: TerrainRing,
    /// Per paved vertex grade triples in `outline_grades`; empty for uniform.
    pub grades: TerrainRing,
    /// Isolated plan points in the shared geometry pool.
    pub spots: TerrainRing,
    /// Stack position, after every region entry.
    pub order: u32,
    /// Grading effect, 1..3.
    pub effect: u32,
    /// First face line in `outline_lines`.
    pub first_line: u32,
    /// Number of face lines.
    pub line_count: u32,
}

/// The immutable baseline, the ordered plan, shared geometry pools and the
/// input provenance. All plan records are POD with offset ranges into the
/// shared pools; empty property/entries/paths/masks/controls are valid, and a
/// nonempty plan needs a property ring.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedRequest {
    /// Flat xyz baseline vertices, world metres.
    pub points: SliceView<f64>,
    /// Counter-clockwise baseline triangle triples.
    pub triangles: SliceView<u32>,
    /// Closed property ring xyz; empty selects the whole baseline.
    pub property: SliceView<f64>,
    /// Shared xyz pool of every domain/mask ring and control line.
    pub geometry: SliceView<f64>,
    /// Ring ranges into `geometry`; ring zero of each part is its outer.
    pub rings: SliceView<TerrainRing>,
    /// Domain part records indexing the rings pool.
    pub parts: SliceView<UnifiedPart>,
    /// Part indices of every entry then every mask, in plan order.
    pub part_indices: SliceView<u32>,
    /// Stations of every path concatenated, in plan order.
    pub stations: SliceView<super::TerrainStation>,
    /// Target-surface triangle indices of every triangulated entry, in plan order.
    pub surface_indices: SliceView<u32>,
    /// Source identities of every entry then every mask, in plan order.
    pub sources: SliceView<u64>,
    /// Ordered region-field entries.
    pub entries: SliceView<UnifiedEntry>,
    /// Ordered path bands.
    pub paths: SliceView<UnifiedPath>,
    /// Grading limits and protected zones.
    pub masks: SliceView<UnifiedMask>,
    /// Fixed-height point/line controls.
    pub controls: SliceView<UnifiedControl>,
    /// Absolute grading/fixed-height compatibility band, metres.
    pub control_tolerance: f64,
    /// Document revision the plan was compiled from.
    pub revision: u64,
    /// Deterministic plan content hash for the caller's stale-result gate.
    pub plan_hash: u64,
    /// Domains exactly vertex+edge+face (7) in the documented input order.
    pub provenance: ProvenanceRequest,
    /// At most one paved outline (#149 T1); empty for none.
    pub outlines: SliceView<UnifiedOutline>,
    /// Outline face-line ranges into `geometry`.
    pub outline_lines: SliceView<TerrainRing>,
    /// Outline per-vertex (cut, fill, rounding) gradient triples.
    pub outline_grades: SliceView<f64>,
}

/// Scalar element counts of a complete unified result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UnifiedCounts {
    /// xyz scalar count.
    pub points: usize,
    /// Triangle index scalar count.
    pub triangles: usize,
    /// Support row count.
    pub mappings: usize,
    /// Per-breakline origin count.
    pub lines: usize,
    /// Resolved locked-vertex count.
    pub locked: usize,
}

/// The caller's revision and plan hash, carried through unchanged, and the
/// outline's located shortfalls (#149 T1).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UnifiedMeta {
    /// Document revision the plan was compiled from.
    pub revision: u64,
    /// Deterministic plan content hash.
    pub plan_hash: u64,
    /// Outline vertices whose face ran out at its limit before the ground.
    pub outline_unreached: u32,
    /// Rebuild-ring samples where an outline face had not met the ground.
    pub outline_clipped: u32,
}

/// Authored origin of one constrained breakline, in breakline order: every
/// entry ring segment, every derived path design line, every mask ring
/// segment, every control line, then every outline line. `domain`: 1 entry,
/// 2 path, 3 mask, 4 control, 5 outline (#149 T1: `part` 0 for a caller face
/// line, 1 for a private paved support whose paved vertex pair is
/// `ring`/`segment`, 2 for a traced toe line, #149 T2). For paths, `kind`/`side`/`root` reuse the hybrid line encoding
/// (kinds 1..8); for a control line, `segment` is its authored segment count.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct UnifiedLine {
    /// Origin domain, 1..4.
    pub domain: u32,
    /// Entry/path/mask/control index.
    pub index: u32,
    /// Entry/mask part index.
    pub part: u32,
    /// Entry/mask ring: zero outer, one-based hole index.
    pub ring: u32,
    /// Entry/mask: zero-based ring segment; control: authored segment count.
    pub segment: u32,
    /// Path design-line kind, hybrid encoding 1..8; zero elsewhere.
    pub kind: u32,
    /// Path design-line side, 1 left 2 right; zero elsewhere.
    pub side: u32,
    /// Path design-line root; zero elsewhere.
    pub root: u32,
}

/// One resolved fixed-height output vertex.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct UnifiedLocked {
    /// Index into the plan's controls.
    pub control: u32,
    /// Output vertex owned by the control.
    pub vertex: u32,
    /// Fixed height, metres.
    pub height: f64,
}

/// Caller-owned disjoint result storage. `counts`, `meta`, `site` and
/// `context` are required; query mode publishes only counts and needs zero
/// capacities. All outputs stay unchanged on any refusal.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UnifiedOutput {
    /// Required counts record.
    pub counts: *mut UnifiedCounts,
    /// Required revision/hash record.
    pub meta: *mut UnifiedMeta,
    /// Required site (inside the property) volumes.
    pub site: *mut TerrainHybridVolumes,
    /// Required context (outside) volumes.
    pub context: *mut TerrainHybridVolumes,
    /// Proposed xyz.
    pub points: *mut f64,
    /// Scalar capacity.
    pub point_capacity: usize,
    /// Baseline height per output vertex.
    pub base: *mut f64,
    /// Capacity, at least the vertex count.
    pub base_capacity: usize,
    /// Last ordered stack position that moved each vertex, `u32::MAX` if none.
    pub owners: *mut u32,
    /// Capacity, at least the vertex count.
    pub owner_capacity: usize,
    /// Output vertex triples.
    pub triangles: *mut u32,
    /// Scalar capacity.
    pub triangle_capacity: usize,
    /// Region correspondence and support rows.
    pub mappings: *mut TerrainMapping,
    /// Row capacity.
    pub mapping_capacity: usize,
    /// Per-breakline origins, in breakline order.
    pub lines: *mut UnifiedLine,
    /// Capacity.
    pub line_capacity: usize,
    /// Resolved locked vertices.
    pub locked: *mut UnifiedLocked,
    /// Capacity.
    pub locked_capacity: usize,
    /// 1 publishes only counts; 0 publishes the complete result.
    pub query_only: u32,
    /// Zero.
    pub reserved: u32,
}

fn writable(target: UnifiedOutput) -> Vec<Checked> {
    vec![
        memory_range(target.counts.cast_const(), 1),
        memory_range(target.meta.cast_const(), 1),
        memory_range(target.site.cast_const(), 1),
        memory_range(target.context.cast_const(), 1),
        memory_range(target.points.cast_const(), target.point_capacity),
        memory_range(target.base.cast_const(), target.base_capacity),
        memory_range(target.owners.cast_const(), target.owner_capacity),
        memory_range(target.triangles.cast_const(), target.triangle_capacity),
        memory_range(target.mappings.cast_const(), target.mapping_capacity),
        memory_range(target.lines.cast_const(), target.line_capacity),
        memory_range(target.locked.cast_const(), target.locked_capacity),
    ]
}

fn mode(target: &UnifiedOutput) -> Result<(), Failure> {
    if target.query_only > 1
        || target.reserved != 0
        || (target.query_only == 1
            && (target.point_capacity
                | target.base_capacity
                | target.owner_capacity
                | target.triangle_capacity
                | target.mapping_capacity
                | target.line_capacity
                | target.locked_capacity)
                != 0)
    {
        return Err(Failure::Argument("TerrainOutputMode"));
    }
    Ok(())
}

fn index(value: usize) -> Result<u32, Failure> {
    u32::try_from(value).map_err(|_| Failure::Argument("TerrainCountOverflow"))
}

fn count(value: usize) -> Result<u64, Failure> {
    u64::try_from(value).map_err(|_| Failure::Argument("TerrainCountOverflow"))
}

fn row(kind: u32, index: u32, values: [u32; 6]) -> TerrainMapping {
    TerrainMapping {
        kind,
        index,
        values,
    }
}

fn ring_slice<T: Copy>(geometry: &[T], ring: TerrainRing) -> Result<&[T], Failure> {
    let start = ring.offset as usize;
    let end = start
        .checked_add(ring.count as usize)
        .ok_or(Failure::Argument("TerrainUnifiedRingRange"))?;
    let slice = geometry
        .get(start..end)
        .ok_or(Failure::Argument("TerrainUnifiedRingRange"))?;
    if !slice.len().is_multiple_of(3) {
        return Err(Failure::Argument("TerrainUnifiedRingShape"));
    }
    Ok(slice)
}

/// Convert the shared parts pool into per-part hole lists borrowed from the
/// geometry pool.
fn holes<'a>(
    pool: &[UnifiedPart],
    rings: &[TerrainRing],
    geometry: &'a [f64],
) -> Result<Vec<Vec<&'a [f64]>>, Failure> {
    if rings.iter().any(|ring| ring.count % 3 != 0) {
        return Err(Failure::Argument("TerrainUnifiedRingShape"));
    }
    let mut holes = Vec::with_capacity(pool.len());
    for part in pool {
        if part.ring_count == 0 {
            return Err(Failure::Argument("TerrainUnifiedPartShape"));
        }
        let first = part.first_ring as usize;
        let end = first
            .checked_add(part.ring_count as usize)
            .ok_or(Failure::Argument("TerrainUnifiedPartRange"))?;
        let range = rings
            .get(first..end)
            .ok_or(Failure::Argument("TerrainUnifiedPartRange"))?;
        let mut list = Vec::with_capacity(range.len().saturating_sub(1));
        for ring in range.iter().skip(1) {
            list.push(ring_slice(geometry, *ring)?);
        }
        holes.push(list);
    }
    Ok(holes)
}

/// Borrow the shared parts pool as kernel parts; the hole lists are borrowed
/// from `holes`, which must outlive the returned parts.
fn parts<'h, 'a: 'h>(
    pool: &[UnifiedPart],
    rings: &[TerrainRing],
    holes: &'h [Vec<&'a [f64]>],
    geometry: &'a [f64],
) -> Result<Vec<kernel::Part<'h>>, Failure> {
    let mut converted = Vec::with_capacity(pool.len());
    for (part, list) in pool.iter().zip(holes) {
        let outer = rings
            .get(part.first_ring as usize)
            .ok_or(Failure::Argument("TerrainUnifiedPartRange"))?;
        converted.push(kernel::Part {
            outer: ring_slice(geometry, *outer)?,
            holes: list,
        });
    }
    Ok(converted)
}

fn select_parts<'a>(
    pool: &[kernel::Part<'a>],
    indices: &[u32],
) -> Result<Vec<kernel::Part<'a>>, Failure> {
    indices
        .iter()
        .map(|&at| {
            pool.get(at as usize)
                .copied()
                .ok_or(Failure::Argument("TerrainUnifiedPartIndex"))
        })
        .collect()
}

fn range(
    pool_len: usize,
    first: u32,
    count: u32,
    code: &'static str,
) -> Result<(usize, usize), Failure> {
    let start = first as usize;
    let end = start
        .checked_add(count as usize)
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    if end > pool_len {
        return Err(Failure::Argument(code));
    }
    Ok((start, end))
}

/// Checked shared-pool slice selected by a POD index range.
fn take<'a, T: Copy>(
    pool: &'a [T],
    first: u32,
    count: u32,
    code: &'static str,
) -> Result<&'a [T], Failure> {
    let (start, end) = range(pool.len(), first, count, code)?;
    pool.get(start..end).ok_or(Failure::Argument(code))
}

fn surface<'a>(
    raw: &UnifiedEntry,
    geometry: &'a [f64],
    indices: &'a [u32],
) -> Result<kernel::Surface<'a>, Failure> {
    match raw.surface {
        1 => Ok(kernel::Surface::Existing),
        2 => Ok(kernel::Surface::Constant { level: raw.level }),
        3 => Ok(kernel::Surface::Triangulated {
            points: ring_slice(geometry, raw.surface_points)?,
            triangles: ring_slice(indices, raw.surface_triangles)?,
        }),
        _ => Err(Failure::Argument("TerrainUnifiedSurface")),
    }
}

fn effect(raw: &UnifiedEntry) -> Result<kernel::TerrainEffect, Failure> {
    match raw.effect {
        1 => Ok(kernel::TerrainEffect::Grade),
        2 => Ok(kernel::TerrainEffect::Add),
        3 => Ok(kernel::TerrainEffect::Subtract),
        _ => Err(Failure::Argument("TerrainUnifiedEffect")),
    }
}

fn transition(raw: &UnifiedEntry) -> Result<kernel::Transition, Failure> {
    match raw.transition {
        1 => Ok(kernel::Transition::Falloff { blur: raw.falloff }),
        2 => Ok(kernel::Transition::Daylight {
            cut: slope(raw.cut)?,
            fill: slope(raw.fill)?,
            reach: raw.reach,
        }),
        _ => Err(Failure::Argument("TerrainUnifiedTransition")),
    }
}

fn band_effect(raw: u32) -> Result<Effect, Failure> {
    match raw {
        1 => Ok(Effect::Cut),
        2 => Ok(Effect::Fill),
        3 => Ok(Effect::Both),
        _ => Err(Failure::Argument("TerrainUnifiedBandEffect")),
    }
}

fn mask_kind(raw: u32) -> Result<kernel::MaskKind, Failure> {
    match raw {
        1 => Ok(kernel::MaskKind::GradingLimit),
        2 => Ok(kernel::MaskKind::ProtectedZone),
        _ => Err(Failure::Argument("TerrainUnifiedMaskKind")),
    }
}

fn hand(side: Hand) -> u32 {
    match side {
        Hand::Left => 1,
        Hand::Right => 2,
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
    points: Vec<f64>,
    base: Vec<f64>,
    owners: Vec<u32>,
    triangles: Vec<u32>,
    mappings: Vec<TerrainMapping>,
    lines: Vec<UnifiedLine>,
    locked: Vec<UnifiedLocked>,
    site: TerrainHybridVolumes,
    context: TerrainHybridVolumes,
    meta: UnifiedMeta,
}

fn path_line(kind: LineKind) -> Result<(u32, u32, u32), Failure> {
    let encoded = match kind {
        LineKind::Edge(side) => (1, hand(side), 0),
        LineKind::Shoulder(side) => (2, hand(side), 0),
        LineKind::Daylight { side, root } => (3, hand(side), root),
        LineKind::End(last) => (4, 0, u32::from(last)),
        LineKind::Crown => (5, 0, 0),
        LineKind::Section(station) => (7, 0, station),
        LineKind::Face { side, step } => (8, hand(side), step),
        LineKind::Locked(_) => return Err(Failure::Argument("TerrainUnifiedLockedLine")),
    };
    Ok(encoded)
}

/// Encode a build losslessly: region faces (12), split vertices (9), region
/// vertices (13, 14), breakline vertices (15) and edges (16), the property
/// ring (17) and control spots (20).
pub(super) fn encode(built: kernel::BuiltTerrain) -> Result<Encoded, Failure> {
    let kernel::BuiltTerrain {
        points,
        base,
        owners,
        mesh,
        lines,
        path_lines,
        mask_lines,
        control_lines,
        outline_lines,
        outline_unreached,
        outline_clipped,
        locked,
        volumes: report,
        revision,
        plan_hash,
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
    for (at, vertex) in mesh.split_vertices.iter().enumerate() {
        let (origin, source) = match vertex.origin {
            kernel::Origin::LineVertex(n) => (1, n),
            kernel::Origin::Crossing(n) => (2, n),
        };
        let (host, a, b) = match vertex.host {
            kernel::Host::Edge([a, b]) => (1, a, b),
            kernel::Host::Face(n) => (2, n, 0),
        };
        let at = input
            .checked_add(at)
            .ok_or(Failure::Argument("TerrainCountOverflow"))?;
        mappings.push(row(9, index(at)?, [origin, source, host, a, b, 0]));
    }
    let appended = input
        .checked_add(mesh.split_vertices.len())
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    for (at, vertex) in mesh.new_vertices.iter().enumerate() {
        let at = index(
            appended
                .checked_add(at)
                .ok_or(Failure::Argument("TerrainCountOverflow"))?,
        )?;
        match *vertex {
            RegionVertex::Point(point) => {
                mappings.push(row(20, at, [point, 0, 0, 0, 0, 0]));
            }
            RegionVertex::Breakline { line, vertex } => {
                mappings.push(row(13, at, [line, vertex, 0, 0, 0, 0]));
            }
            RegionVertex::Crossing(pair) => {
                for value in pair {
                    let [kind, a, b] = support(value);
                    mappings.push(row(14, at, [kind, a, b, 0, 0, 0]));
                }
            }
            RegionVertex::Refined => {
                // #149 T3: a vertex the outline mesher added, inside a triangle or on a line or
                // base edge it split. No caller input names it; its row says only that.
                mappings.push(row(23, at, [0, 0, 0, 0, 0, 0]));
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
    for (point, &vertex) in mesh.point_vertices.iter().enumerate() {
        mappings.push(row(21, index(point)?, [vertex, 0, 0, 0, 0, 0]));
    }
    let mut rows = Vec::with_capacity(
        lines
            .len()
            .checked_add(path_lines.len())
            .and_then(|n| n.checked_add(mask_lines.len()))
            .and_then(|n| n.checked_add(control_lines.len()))
            .and_then(|n| n.checked_add(outline_lines.len()))
            .ok_or(Failure::Argument("TerrainCountOverflow"))?,
    );
    for origin in &lines {
        rows.push(UnifiedLine {
            domain: 1,
            index: origin.entry,
            part: origin.part,
            ring: origin.ring,
            segment: origin.segment,
            kind: 0,
            side: 0,
            root: 0,
        });
    }
    for origin in &path_lines {
        let (kind, side, root) = path_line(origin.kind)?;
        rows.push(UnifiedLine {
            domain: 2,
            index: origin.path,
            part: 0,
            ring: 0,
            segment: 0,
            kind,
            side,
            root,
        });
    }
    for origin in &mask_lines {
        rows.push(UnifiedLine {
            domain: 3,
            index: origin.mask,
            part: origin.part,
            ring: origin.ring,
            segment: origin.segment,
            kind: 0,
            side: 0,
            root: 0,
        });
    }
    for origin in &control_lines {
        rows.push(UnifiedLine {
            domain: 4,
            index: origin.control,
            part: 0,
            ring: 0,
            segment: origin.segments,
            kind: 0,
            side: 0,
            root: 0,
        });
    }
    for origin in &outline_lines {
        let [a, b] = origin.support.unwrap_or([0, 0]);
        rows.push(UnifiedLine {
            domain: 5,
            index: origin.outline,
            part: if origin.toe {
                2
            } else {
                u32::from(origin.support.is_some())
            },
            ring: a,
            segment: b,
            kind: 0,
            side: 0,
            root: 0,
        });
    }
    let locked = locked
        .into_iter()
        .map(|vertex| UnifiedLocked {
            control: vertex.control,
            vertex: vertex.vertex,
            height: vertex.height,
        })
        .collect();
    Ok(Encoded {
        site: volumes(report.site),
        context: volumes(report.context),
        meta: UnifiedMeta {
            revision,
            plan_hash,
            outline_unreached,
            outline_clipped,
        },
        points,
        base,
        owners: owners
            .iter()
            .map(|owner| owner.unwrap_or(u32::MAX))
            .collect(),
        triangles: mesh.triangles,
        mappings,
        lines: rows,
        locked,
    })
}

/// Input provenance dimensions for the documented vertex/edge order.
fn attribution_counts(
    mesh_vertices: usize,
    mesh_triangles: usize,
    property: usize,
    rings: &[TerrainRing],
    controls: &[UnifiedControl],
) -> Result<id::EntityCounts, Failure> {
    let ring_vertices = rings.iter().try_fold(0_u64, |total, ring| {
        total
            .checked_add(count(ring.count as usize / 3)?)
            .ok_or(Failure::Argument("TerrainCountOverflow"))
    })?;
    // Entry/mask rings are closed and contribute one segment per vertex.
    // Control lines are already present in `rings`; subtract them here, then
    // add each open line once with one fewer segment. Target-surface pools are
    // parameters and deliberately have no entity rows.
    let mut line_vertices = 0_u64;
    let mut line_segments = 0_u64;
    let mut points = 0_u64;
    for control in controls {
        if control.point == 1 {
            points = points
                .checked_add(1)
                .ok_or(Failure::Argument("TerrainCountOverflow"))?;
        } else {
            if control.line.count % 3 != 0 {
                return Err(Failure::Argument("TerrainUnifiedRingShape"));
            }
            let vertices = count(control.line.count as usize / 3)?;
            line_vertices = line_vertices
                .checked_add(vertices)
                .ok_or(Failure::Argument("TerrainCountOverflow"))?;
            line_segments = line_segments
                .checked_add(vertices.saturating_sub(1))
                .ok_or(Failure::Argument("TerrainCountOverflow"))?;
        }
    }
    let closed_ring_vertices = ring_vertices
        .checked_sub(line_vertices)
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    let vertices = count(mesh_vertices)?
        .checked_add(count(property / 3)?)
        .and_then(|n| n.checked_add(closed_ring_vertices))
        .and_then(|n| n.checked_add(line_vertices))
        .and_then(|n| n.checked_add(points))
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    let edges = count(property / 3)?
        .checked_add(closed_ring_vertices)
        .and_then(|n| n.checked_add(line_segments))
        .ok_or(Failure::Argument("TerrainCountOverflow"))?;
    Ok(id::EntityCounts {
        vertices,
        edges,
        faces: count(mesh_triangles / 3)?,
        patches: 0,
    })
}

// SAFETY: every top-level pool is preflighted disjoint from all of `output`'s
// writable ranges and the error buffer before any foreign borrow; all records
// are POD with offset ranges, so there are no nested pointer pools.
/// Every pool the request names, for the preflight against the outputs: shared by the build
/// and the field preview (#175).
pub(super) fn request_pools(request: &UnifiedRequest) -> Vec<Checked> {
    let mut inputs = Vec::new();
    inputs.extend(pools::provenance(&request.provenance));
    inputs.extend([
        memory_range(request.points.data, request.points.count),
        memory_range(request.triangles.data, request.triangles.count),
        memory_range(request.property.data, request.property.count),
        memory_range(request.geometry.data, request.geometry.count),
        memory_range(request.rings.data, request.rings.count),
        memory_range(request.parts.data, request.parts.count),
        memory_range(request.part_indices.data, request.part_indices.count),
        memory_range(request.stations.data, request.stations.count),
        memory_range(request.surface_indices.data, request.surface_indices.count),
        memory_range(request.sources.data, request.sources.count),
        memory_range(request.entries.data, request.entries.count),
        memory_range(request.paths.data, request.paths.count),
        memory_range(request.masks.data, request.masks.count),
        memory_range(request.controls.data, request.controls.count),
        memory_range(request.outlines.data, request.outlines.count),
        memory_range(request.outline_lines.data, request.outline_lines.count),
        memory_range(request.outline_grades.data, request.outline_grades.count),
    ]);
    inputs
}

unsafe fn compute(
    input: *const UnifiedRequest,
    output: *const UnifiedOutput,
    error: Range,
) -> Result<(Encoded, UnifiedOutput), Failure> {
    // SAFETY: descriptors validates both POD shapes before copying them.
    let (request, target, context): (_, _, BorrowContext) =
        unsafe { pools::descriptors(input, output, error)? };
    let mut inputs = vec![memory_range(input, 1), memory_range(output, 1)];
    if let Some(request) = &request {
        inputs.extend(request_pools(request));
    }
    let outputs = target.map_or_else(Vec::new, writable);
    pools::preflight(&inputs, &outputs, error)?;
    let request = request.ok_or(Failure::Argument("TerrainInputDescriptor"))?;
    let target = target.ok_or(Failure::Argument("TerrainOutputDescriptor"))?;
    mode(&target)?;
    // SAFETY: all pools were preflighted above; no borrow escapes this call.
    let built = unsafe { evaluate(&context, &request, kernel::build_terrain) }?;
    Ok((encode(built)?, target))
}

// SAFETY: all request pools were preflighted against outputs and the error
// buffer; every returned borrow lives only for this call. `run` is given the
// decoded baseline and plan: the build, or the field preview (#175).
pub(super) unsafe fn evaluate<R>(
    context: &BorrowContext,
    request: &UnifiedRequest,
    run: impl FnOnce(&kernel::Baseline<'_>, &kernel::Plan<'_>) -> Result<R, kernel::BuildError>,
) -> Result<R, Failure> {
    // SAFETY: no borrow escapes; computation owns all output before publication.
    let (
        points,
        triangles,
        property,
        geometry,
        rings,
        parts_pool,
        part_indices,
        stations_pool,
        surface_indices,
        sources_pool,
        raw_entries,
        raw_paths,
        raw_masks,
        raw_controls,
        provenance,
        raw_outlines,
        outline_line_pool,
        outline_grade_pool,
    ) = unsafe {
        (
            context.slice(request.points)?,
            context.slice(request.triangles)?,
            context.slice(request.property)?,
            context.slice(request.geometry)?,
            context.slice(request.rings)?,
            context.slice(request.parts)?,
            context.slice(request.part_indices)?,
            context.slice(request.stations)?,
            context.slice(request.surface_indices)?,
            context.slice(request.sources)?,
            context.slice(request.entries)?,
            context.slice(request.paths)?,
            context.slice(request.masks)?,
            context.slice(request.controls)?,
            context.provenance(&request.provenance)?,
            context.slice(request.outlines)?,
            context.slice(request.outline_lines)?,
            context.slice(request.outline_grades)?,
        )
    };
    let expected = attribution_counts(
        points.len() / 3,
        triangles.len(),
        property.len(),
        rings,
        raw_controls,
    )?;
    attribution(&provenance, expected, 7)?;
    for control in raw_controls {
        if control.reserved != 0 || control.point > 1 {
            return Err(Failure::Argument("TerrainUnifiedControl"));
        }
        if control.point == 0 {
            ring_slice(geometry, control.line)?;
        }
    }
    let hole_lists = holes(parts_pool, rings, geometry)?;
    let kernel_parts = parts(parts_pool, rings, &hole_lists, geometry)?;
    // Stage owned per-member storage first; plan records borrow it by index.
    let mut entry_parts = Vec::with_capacity(raw_entries.len());
    let mut entry_sources = Vec::with_capacity(raw_entries.len());
    for raw in raw_entries {
        let parts = select_parts(
            &kernel_parts,
            take(
                part_indices,
                raw.first_part,
                raw.part_count,
                "TerrainUnifiedPartIndex",
            )?,
        )?;
        let sources = take(
            sources_pool,
            raw.first_source,
            raw.source_count,
            "TerrainUnifiedSourceIndex",
        )?;
        entry_parts.push(parts);
        entry_sources.push(
            sources
                .iter()
                .map(|&source| id::SourceId(source))
                .collect::<Vec<_>>(),
        );
    }
    let mut entries = Vec::with_capacity(raw_entries.len());
    for (at, raw) in raw_entries.iter().enumerate() {
        let parts = entry_parts
            .get(at)
            .ok_or(Failure::Argument("TerrainUnifiedPartIndex"))?;
        let sources = entry_sources
            .get(at)
            .ok_or(Failure::Argument("TerrainUnifiedPartIndex"))?;
        entries.push(kernel::Entry {
            order: raw.order,
            object: id::ObjectId(raw.object),
            root_operation: id::OperationId(raw.root_operation),
            sources,
            parts,
            surface: surface(raw, geometry, surface_indices)?,
            terrain: kernel::Terrain {
                effect: effect(raw)?,
                transition: transition(raw)?,
            },
        });
    }
    let mut stations = Vec::with_capacity(raw_paths.len());
    for raw in raw_paths {
        if raw.reserved != 0 {
            return Err(Failure::Argument("TerrainUnifiedPath"));
        }
        let band = take(
            stations_pool,
            raw.first_station,
            raw.station_count,
            "TerrainUnifiedStationIndex",
        )?;
        stations.push(band.iter().map(station).collect::<Result<Vec<_>, _>>()?);
    }
    let mut paths = Vec::with_capacity(raw_paths.len());
    for (at, raw) in raw_paths.iter().enumerate() {
        let band = stations
            .get(at)
            .ok_or(Failure::Argument("TerrainUnifiedPath"))?;
        paths.push(kernel::Path {
            order: raw.order,
            source: id::SourceId(raw.source),
            operation: id::OperationId(raw.operation),
            object: id::ObjectId(raw.object),
            band: Band {
                stations: band,
                effect: band_effect(raw.effect)?,
            },
            tolerance: raw.tolerance,
        });
    }
    let mut mask_parts = Vec::with_capacity(raw_masks.len());
    let mut mask_sources = Vec::with_capacity(raw_masks.len());
    for raw in raw_masks {
        if raw.reserved != 0 {
            return Err(Failure::Argument("TerrainUnifiedMask"));
        }
        let parts = select_parts(
            &kernel_parts,
            take(
                part_indices,
                raw.first_part,
                raw.part_count,
                "TerrainUnifiedPartIndex",
            )?,
        )?;
        let sources = take(
            sources_pool,
            raw.first_source,
            raw.source_count,
            "TerrainUnifiedSourceIndex",
        )?;
        mask_parts.push(parts);
        mask_sources.push(
            sources
                .iter()
                .map(|&source| id::SourceId(source))
                .collect::<Vec<_>>(),
        );
    }
    let mut masks = Vec::with_capacity(raw_masks.len());
    for (at, raw) in raw_masks.iter().enumerate() {
        let parts = mask_parts
            .get(at)
            .ok_or(Failure::Argument("TerrainUnifiedMask"))?;
        let sources = mask_sources
            .get(at)
            .ok_or(Failure::Argument("TerrainUnifiedMask"))?;
        masks.push(kernel::Mask {
            operation: id::OperationId(raw.operation),
            kind: mask_kind(raw.kind)?,
            sources,
            parts,
        });
    }
    let mut controls = Vec::with_capacity(raw_controls.len());
    for raw in raw_controls {
        let line = if raw.point == 1 {
            &[][..]
        } else {
            ring_slice(geometry, raw.line)?
        };
        controls.push(kernel::Control {
            source: id::SourceId(raw.source),
            vertex: raw.vertex,
            point: raw.point == 1,
            point_position: raw.position,
            line,
        });
    }
    let baseline = kernel::Baseline {
        points,
        triangles,
        property: if property.is_empty() {
            None
        } else {
            Some(property)
        },
    };
    // #149 T1: the paved outline's pools; none carries entity rows.
    let mut outline_grades = Vec::with_capacity(raw_outlines.len());
    let mut outline_lines = Vec::with_capacity(raw_outlines.len());
    for raw in raw_outlines {
        let triples = ring_slice(outline_grade_pool, raw.grades)?;
        if !triples.len().is_multiple_of(3) {
            return Err(Failure::Argument("TerrainUnifiedOutline"));
        }
        outline_grades.push(
            triples
                .as_chunks::<3>()
                .0
                .iter()
                .map(|&[cut, fill, rounding]| kernel::outline::SurfaceGrade {
                    cut,
                    fill,
                    rounding,
                })
                .collect::<Vec<_>>(),
        );
        let ranges = take(
            outline_line_pool,
            raw.first_line,
            raw.line_count,
            "TerrainUnifiedOutlineLine",
        )?;
        outline_lines.push(
            ranges
                .iter()
                .map(|&range| ring_slice(geometry, range))
                .collect::<Result<Vec<_>, _>>()?,
        );
    }
    let mut outlines = Vec::with_capacity(raw_outlines.len());
    for (at, raw) in raw_outlines.iter().enumerate() {
        let (Some(grades), Some(lines)) = (outline_grades.get(at), outline_lines.get(at)) else {
            return Err(Failure::Argument("TerrainUnifiedOutline"));
        };
        outlines.push(kernel::Outline {
            order: raw.order,
            surface_points: ring_slice(geometry, raw.surface_points)?,
            surface_triangles: ring_slice(surface_indices, raw.surface_triangles)?,
            surface_grades: grades,
            grading: kernel::outline::Grading {
                cut: slope(raw.cut)?,
                fill: slope(raw.fill)?,
                limit: raw.limit,
                rounding: raw.rounding,
                effect: band_effect(raw.effect)?,
                tolerance: raw.tolerance,
            },
            lines,
            spots: ring_slice(geometry, raw.spots)?,
        });
    }
    let plan = kernel::Plan {
        revision: request.revision,
        hash: request.plan_hash,
        entries: &entries,
        paths: &paths,
        masks: &masks,
        controls: &controls,
        control_tolerance: request.control_tolerance,
        outlines: &outlines,
    };
    run(&baseline, &plan).map_err(|error| Failure::Argument(error.message()))
}

// SAFETY: target ranges were preflighted disjoint from every input, the error
// buffer and each other; every capacity is checked before ANY write.
pub(super) unsafe fn publish(data: &Encoded, target: UnifiedOutput) -> Result<(), Failure> {
    let counts = UnifiedCounts {
        points: data.points.len(),
        triangles: data.triangles.len(),
        mappings: data.mappings.len(),
        lines: data.lines.len(),
        locked: data.locked.len(),
    };
    if target.query_only == 0
        && (target.point_capacity < counts.points
            || target.base_capacity < data.base.len()
            || target.owner_capacity < data.owners.len()
            || target.triangle_capacity < counts.triangles
            || target.mapping_capacity < counts.mappings
            || target.line_capacity < counts.lines
            || target.locked_capacity < counts.locked)
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
            copy(&data.locked, target.locked);
            target.site.write(data.site);
            target.context.write(data.context);
            target.meta.write(data.meta);
        }
        target.counts.write(counts);
    }
    Ok(())
}

/// Build one terrain from an immutable baseline and one ordered field plan.
/// Query mode computes the same result and publishes only counts.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_hybrid_build`. Attribution refusals return STATE;
/// geometry/parameter refusals ARG with the kernel's stable code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_unified_build(
    input: *const UnifiedRequest,
    output: *const UnifiedOutput,
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
