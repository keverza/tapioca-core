//! #112 P3 unified terrain build: one immutable baseline, one compiled field
//! stack and one constrained result topology.
//!
//! This module is the first pure-Rust slice of the landscape mesh authority. It
//! assembles every plan domain edge as a constraint of one region rebuild, then
//! evaluates the ordered field stack on that topology. Region entries currently
//! support existing/constant targets with the reconciled absolute Grade, Add and
//! Subtract rules; path bands, grading masks and locked controls are subsequent
//! slices of the same build entry point.
//!
//! Coordinates are world metres in `f64`; heights are z in metres. The baseline
//! and plan are borrowed for the call and are never mutated. A refusal returns no
//! partial surface.

use super::edit::{Effect, smoother_step};
use super::field::{Field, FieldError};
use super::plan;
use super::region::{RegionError, RegionMesh, RegionRequest, retriangulate_region_with_points};
use super::volume::VolumeError;
use super::volume::exact::{ExactReport, ExactRequest, exact_cut_fill};
use crate::identity::{ObjectId, OperationId, SourceId};
use crate::slope::Slope;

/// Immutable terrain baseline consumed by one build.
#[derive(Clone, Copy, Debug)]
pub struct Baseline<'a> {
    /// Flat xyz baseline vertices, world metres.
    pub points: &'a [f64],
    /// Counter-clockwise plan-view baseline triangle triples.
    pub triangles: &'a [u32],
    /// Closed property-line ring. Required when a plan entry has domain parts;
    /// absent is valid only for an empty plan over the whole baseline.
    pub property: Option<&'a [f64]>,
}

/// Immutable compiled field plan consumed by one build.
#[derive(Clone, Copy, Debug)]
pub struct Plan<'a> {
    /// Document revision the plan was compiled from.
    pub revision: u64,
    /// Deterministic plan content hash used by the caller's stale-result gate.
    pub hash: u64,
    /// Ordered stack entries. `Entry::order` must be the contiguous index.
    pub entries: &'a [Entry<'a>],
}

/// One ordered region-field entry in the compiled plan.
#[derive(Clone, Copy, Debug)]
pub struct Entry<'a> {
    /// Contiguous stack position; later entries override earlier entries.
    pub order: u32,
    /// Durable owning object identity carried for diagnostics/provenance.
    pub object: ObjectId,
    /// Durable root-operation identity carried for diagnostics/provenance.
    pub root_operation: OperationId,
    /// Evaluated domain ancestry, in caller order.
    pub sources: &'a [SourceId],
    /// Domain parts owned by this entry.
    pub parts: &'a [Part<'a>],
    /// Authored surface target used inside the domain.
    pub surface: Surface,
    /// Vertical terrain effect and its permitted transition.
    pub terrain: Terrain,
}

/// One evaluated domain part: one outer ring and zero or more holes.
#[derive(Clone, Copy, Debug)]
pub struct Part<'a> {
    /// Implicitly closed outer xyz ring, at least three vertices.
    pub outer: &'a [f64],
    /// Implicitly closed hole xyz rings.
    pub holes: &'a [&'a [f64]],
}

/// Surface target supported by this build slice.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Surface {
    /// The current composed surface; a Grade resets to the baseline inside the
    /// domain, while Add and Subtract are no-ops against the current height.
    Existing,
    /// Absolute constant target height, metres.
    Constant {
        /// Target height inside the domain, metres.
        level: f64,
    },
}

/// Vertical effect applied to the entry's transition surface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TerrainEffect {
    /// Set the composed height to the target.
    Grade,
    /// Raise-only: keep the greater of the current and transition heights.
    Add,
    /// Lower-only: keep the lesser of the current and transition heights.
    Subtract,
}

/// Vertical terrain effect and its transition for one entry.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Terrain {
    /// Absolute-target effect applied to the transition surface.
    pub effect: TerrainEffect,
    /// How the entry reaches the surrounding terrain outside its domain.
    pub transition: Transition,
}

/// How an entry reaches the surrounding terrain outside its domain.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Transition {
    /// Smooth `6t^5 - 15t^4 + 10t^3` falloff over this plan distance, metres.
    Falloff {
        /// Transition width outside the domain, metres.
        blur: f64,
    },
    /// Cut/fill daylight faces over this reach, metres.
    Daylight {
        /// Grade of the face where the ground is above the target.
        cut: Slope,
        /// Grade of the face where the ground is below the target.
        fill: Slope,
        /// Maximum plan reach outside the domain, metres.
        reach: f64,
    },
}

/// Authored identity of one plan edge supplied as a region breakline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LineOrigin {
    /// Index into `Plan::entries`.
    pub entry: u32,
    /// Index into `Entry::parts`.
    pub part: u32,
    /// Zero for the outer ring; one-based hole index otherwise.
    pub ring: u32,
    /// Zero-based ring segment index.
    pub segment: u32,
}

/// One complete unified terrain build.
#[derive(Clone, Debug, PartialEq)]
pub struct BuiltTerrain {
    /// Flat xyz proposed vertices; xy positions are `mesh.points`' positions.
    pub points: Vec<f64>,
    /// Baseline height sampled for every output vertex, metres.
    pub base: Vec<f64>,
    /// Last ordered entry whose evaluation changed each vertex height.
    pub owners: Vec<Option<u32>>,
    /// Result topology and complete baseline/plan correspondence.
    pub mesh: RegionMesh,
    /// Per-line authored origin, indexed like `RegionMesh::line_edges.line`.
    pub lines: Vec<LineOrigin>,
    /// Exact cut/fill against `base`, split at the property line.
    pub volumes: ExactReport,
    /// Revision carried from `Plan::revision`.
    pub revision: u64,
    /// Content hash carried from `Plan::hash`.
    pub plan_hash: u64,
}

/// Unified-build refusals. Nothing partial is returned.
#[derive(Clone, Debug, PartialEq)]
pub enum BuildError {
    /// The baseline is not a valid upward terrain field.
    Baseline(FieldError),
    /// The one-region constraint assembly or baseline topology refused.
    Region(RegionError),
    /// Exact volume reconciliation refused.
    Volume(VolumeError),
    /// A ring or pool does not have whole xyz triples or enough vertices.
    PlanShape {
        /// Index into `Plan::entries`, or `u32::MAX` for the property line.
        entry: u32,
        /// Index into `Entry::parts`, or `u32::MAX` when not applicable.
        part: u32,
        /// Ring index, or `u32::MAX` when not applicable.
        ring: u32,
    },
    /// A plan coordinate or parameter is NaN or infinite.
    NonFinite {
        /// Index into `Plan::entries`, or `u32::MAX` for the property line.
        entry: u32,
        /// Index into `Entry::parts`, or `u32::MAX` when not applicable.
        part: u32,
        /// Ring index, or `u32::MAX` when not applicable.
        ring: u32,
    },
    /// Entry orders are not the contiguous stack indices.
    Order {
        /// Contiguous index expected at this plan position.
        expected: u32,
        /// Authored order found at this plan position.
        current: u32,
    },
    /// A nonempty plan needs an explicit property ring.
    PropertyRequired,
    /// A transition or surface parameter is outside its contract.
    Parameters {
        /// Index into `Plan::entries`.
        entry: u32,
        /// Index into `Entry::parts`, or zero when the parameter is entry-wide.
        part: u32,
        /// Name of the refusing parameter group.
        field: &'static str,
    },
    /// An entry names a zero-length ring segment.
    ZeroLength {
        /// Index into `Plan::entries`.
        entry: u32,
        /// Index into `Entry::parts`.
        part: u32,
        /// Ring index: zero outer, one-based hole.
        ring: u32,
        /// Zero-based segment index in the ring.
        segment: u32,
    },
}

impl BuildError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(&self) -> &'static str {
        match self {
            Self::Baseline(error) => (*error).message(),
            Self::Region(error) => (*error).message(),
            Self::Volume(error) => (*error).message(),
            Self::PlanShape { .. } => {
                "BuildPlanShape: rings need whole xyz triples and three or more vertices"
            }
            Self::NonFinite { .. } => {
                "BuildNonFinite: plan coordinates and parameters must be finite"
            }
            Self::Order { .. } => "BuildOrder: entry.order must be its contiguous plan index",
            Self::PropertyRequired => {
                "BuildProperty: a nonempty plan needs an explicit property line"
            }
            Self::Parameters { .. } => {
                "BuildParameters: falloff, reach and surface levels are outside their contract"
            }
            Self::ZeroLength { .. } => "BuildZeroLength: a plan ring has a zero-length segment",
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct PreparedEntry<'a> {
    entry: &'a Entry<'a>,
    target: Option<f64>,
    effect: Effect,
    transition: PreparedTransition,
}

#[derive(Clone, Copy, Debug)]
enum PreparedTransition {
    Falloff { blur: f64 },
    Daylight { cut: f64, fill: f64, reach: f64 },
}

#[derive(Clone, Debug)]
struct PlannedLine {
    points: Vec<f64>,
    origin: LineOrigin,
}

fn plan_shape(entry: u32, part: u32, ring: u32) -> BuildError {
    BuildError::PlanShape { entry, part, ring }
}

fn non_finite(entry: u32, part: u32, ring: u32) -> BuildError {
    BuildError::NonFinite { entry, part, ring }
}

fn validate_ring(ring: &[f64], entry: u32, part: u32, ring_index: u32) -> Result<(), BuildError> {
    if !ring.len().is_multiple_of(3) || plan::vertices(ring).len() < 3 {
        return Err(plan_shape(entry, part, ring_index));
    }
    if ring.iter().any(|value| !value.is_finite()) {
        return Err(non_finite(entry, part, ring_index));
    }
    Ok(())
}

fn validate_transition(entry: &Entry<'_>, part: u32) -> Result<PreparedTransition, BuildError> {
    match entry.terrain.transition {
        Transition::Falloff { blur } => {
            if !blur.is_finite() {
                return Err(non_finite(entry.order, part, u32::MAX));
            }
            if blur <= 0.0 {
                return Err(BuildError::Parameters {
                    entry: entry.order,
                    part,
                    field: "falloff",
                });
            }
            Ok(PreparedTransition::Falloff { blur })
        }
        Transition::Daylight { cut, fill, reach } => {
            if !reach.is_finite() {
                return Err(non_finite(entry.order, part, u32::MAX));
            }
            if reach < 0.0 {
                return Err(BuildError::Parameters {
                    entry: entry.order,
                    part,
                    field: "reach",
                });
            }
            let cut = cut.gradient().map_err(|_| BuildError::Parameters {
                entry: entry.order,
                part,
                field: "cut",
            })?;
            let fill = fill.gradient().map_err(|_| BuildError::Parameters {
                entry: entry.order,
                part,
                field: "fill",
            })?;
            if !cut.is_finite() || !fill.is_finite() {
                return Err(BuildError::Parameters {
                    entry: entry.order,
                    part,
                    field: "vertical daylight",
                });
            }
            Ok(PreparedTransition::Daylight { cut, fill, reach })
        }
    }
}

fn prepare_entry<'a>(entry: &'a Entry<'a>) -> Result<PreparedEntry<'a>, BuildError> {
    let target = match entry.surface {
        Surface::Existing => None,
        Surface::Constant { level } => {
            if !level.is_finite() {
                return Err(BuildError::NonFinite {
                    entry: entry.order,
                    part: u32::MAX,
                    ring: u32::MAX,
                });
            }
            Some(level)
        }
    };
    let effect = match entry.terrain.effect {
        TerrainEffect::Grade => Effect::Both,
        TerrainEffect::Add => Effect::Fill,
        TerrainEffect::Subtract => Effect::Cut,
    };
    let transition = validate_transition(entry, 0)?;
    Ok(PreparedEntry {
        entry,
        target,
        effect,
        transition,
    })
}

fn validate_plan<'a>(
    plan: &Plan<'a>,
    has_property: bool,
) -> Result<Vec<PreparedEntry<'a>>, BuildError> {
    let mut prepared = Vec::with_capacity(plan.entries.len());
    for (position, entry) in plan.entries.iter().enumerate() {
        let expected = u32::try_from(position).map_err(|_| BuildError::Order {
            expected: u32::MAX,
            current: entry.order,
        })?;
        if entry.order != expected {
            return Err(BuildError::Order {
                expected,
                current: entry.order,
            });
        }
        let mut has_parts = false;
        for (part_index, part) in entry.parts.iter().enumerate() {
            let part_index =
                u32::try_from(part_index).map_err(|_| plan_shape(entry.order, u32::MAX, 0))?;
            validate_ring(part.outer, entry.order, part_index, 0)?;
            has_parts = true;
            for (hole_index, hole) in part.holes.iter().enumerate() {
                let ring = u32::try_from(hole_index + 1)
                    .map_err(|_| plan_shape(entry.order, part_index, u32::MAX))?;
                validate_ring(hole, entry.order, part_index, ring)?;
            }
        }
        if has_parts && !has_property {
            return Err(BuildError::PropertyRequired);
        }
        prepared.push(prepare_entry(entry)?);
    }
    Ok(prepared)
}

fn push_ring_lines(
    lines: &mut Vec<PlannedLine>,
    ring: &[f64],
    entry: u32,
    part: u32,
    ring_index: u32,
) -> Result<(), BuildError> {
    let vertices = plan::vertices(ring);
    let count = vertices.len();
    for segment in 0..count {
        let next = (segment + 1) % count;
        let (Some(a), Some(b)) = (vertices.get(segment), vertices.get(next)) else {
            return Err(plan_shape(entry, part, ring_index));
        };
        if a[0].to_bits() == b[0].to_bits() && a[1].to_bits() == b[1].to_bits() {
            return Err(BuildError::ZeroLength {
                entry,
                part,
                ring: ring_index,
                segment: u32::try_from(segment).unwrap_or(u32::MAX),
            });
        }
        let mut points = Vec::with_capacity(6);
        points.extend_from_slice(a);
        points.extend_from_slice(b);
        lines.push(PlannedLine {
            points,
            origin: LineOrigin {
                entry,
                part,
                ring: ring_index,
                segment: u32::try_from(segment).unwrap_or(u32::MAX),
            },
        });
    }
    Ok(())
}

fn member_distance(part: &Part<'_>, x: f64, y: f64) -> Option<f64> {
    let outer = plan::vertices(part.outer);
    if plan::inside(outer, x, y) == Some(false) {
        return plan::nearest(outer, true, x, y).map(|near| near.distance);
    }
    for hole in part.holes {
        if plan::inside(plan::vertices(hole), x, y) == Some(true) {
            return None;
        }
    }
    Some(0.0)
}

fn distance_to_parts(parts: &[Part<'_>], x: f64, y: f64) -> Option<f64> {
    parts
        .iter()
        .filter_map(|part| member_distance(part, x, y))
        .min_by(f64::total_cmp)
}

fn candidate(prepared: &PreparedEntry<'_>, x: f64, y: f64, current: f64) -> Option<f64> {
    if prepared.entry.parts.is_empty() {
        return None;
    }
    let distance = distance_to_parts(prepared.entry.parts, x, y)?;
    let target = prepared.target.unwrap_or(current);
    let raw = match prepared.transition {
        PreparedTransition::Falloff { blur } => {
            if distance >= blur {
                return None;
            }
            let influence = if distance <= 0.0 {
                1.0
            } else {
                1.0 - smoother_step(distance / blur)
            };
            current + influence * (target - current)
        }
        PreparedTransition::Daylight { cut, fill, reach } => {
            if distance > reach {
                return None;
            }
            if distance <= 0.0 {
                target
            } else if current > target {
                current.min(target + distance * cut)
            } else {
                current.max(target - distance * fill)
            }
        }
    };
    let next = match prepared.effect {
        Effect::Cut => current.min(raw),
        Effect::Fill => current.max(raw),
        Effect::Both => raw,
    };
    Some(next)
}

fn apply_entry(
    prepared: &PreparedEntry<'_>,
    mesh_points: &[f64],
    heights: &mut [f64],
    owners: &mut [Option<u32>],
) -> Result<(), BuildError> {
    for (vertex, &[x, y, _]) in (0_u32..).zip(plan::vertices(mesh_points)) {
        let Some(current) = heights.get(vertex as usize).copied() else {
            return Err(BuildError::Region(RegionError::Topology));
        };
        let Some(next) = candidate(prepared, x, y, current) else {
            continue;
        };
        if let Some(slot) = heights.get_mut(vertex as usize) {
            *slot = next;
        }
        if next.to_bits() != current.to_bits()
            && let Some(owner) = owners.get_mut(vertex as usize)
        {
            *owner = Some(prepared.entry.order);
        }
    }
    Ok(())
}

fn baseline_mesh(points: &[f64], triangles: &[u32]) -> Result<RegionMesh, BuildError> {
    let count =
        u32::try_from(triangles.len() / 3).map_err(|_| BuildError::Baseline(FieldError::Shape))?;
    let parents = (0..count).collect();
    Ok(RegionMesh {
        points: points.to_vec(),
        triangles: triangles.to_vec(),
        parents,
        kept: vec![true; count as usize],
        inside: vec![true; count as usize],
        input_vertices: u32::try_from(points.len() / 3)
            .map_err(|_| BuildError::Baseline(FieldError::Shape))?,
        split_vertices: Vec::new(),
        new_vertices: Vec::new(),
        breakline_vertices: Vec::new(),
        point_vertices: Vec::new(),
        line_edges: Vec::new(),
        ring: Vec::new(),
    })
}

/// Build one terrain from an immutable baseline and ordered field plan.
///
/// All domain edges are assembled as breaklines of one region rebuild, together
/// with the property line and every baseline interior edge. Heights are then
/// composed in plan order on the unified topology and volumes are integrated
/// exactly against the sampled baseline heights. O(plan edges x output
/// vertices) plus the region triangulation; worker-classified.
///
/// # Errors
/// See [`BuildError`]. Inputs are immutable and no partial result is returned.
pub fn build_terrain(baseline: &Baseline<'_>, plan: &Plan<'_>) -> Result<BuiltTerrain, BuildError> {
    Field::new(baseline.points, baseline.triangles).map_err(BuildError::Baseline)?;
    if let Some(property) = baseline.property {
        if !property.len().is_multiple_of(3) || plan::vertices(property).len() < 3 {
            return Err(BuildError::PlanShape {
                entry: u32::MAX,
                part: u32::MAX,
                ring: u32::MAX,
            });
        }
        if property.iter().any(|value| !value.is_finite()) {
            return Err(BuildError::NonFinite {
                entry: u32::MAX,
                part: u32::MAX,
                ring: u32::MAX,
            });
        }
    }
    let prepared = validate_plan(plan, baseline.property.is_some())?;
    let mut lines = Vec::new();
    for entry in plan.entries {
        for (part_index, part) in entry.parts.iter().enumerate() {
            let part_index =
                u32::try_from(part_index).map_err(|_| plan_shape(entry.order, u32::MAX, 0))?;
            push_ring_lines(&mut lines, part.outer, entry.order, part_index, 0)?;
            for (hole_index, hole) in part.holes.iter().enumerate() {
                let ring = u32::try_from(hole_index + 1)
                    .map_err(|_| plan_shape(entry.order, part_index, u32::MAX))?;
                push_ring_lines(&mut lines, hole, entry.order, part_index, ring)?;
            }
        }
    }
    let mesh = if let Some(property) = baseline.property {
        let breaklines: Vec<&[f64]> = lines.iter().map(|line| line.points.as_slice()).collect();
        retriangulate_region_with_points(
            &RegionRequest {
                points: baseline.points,
                triangles: baseline.triangles,
                region: property,
                breaklines: &breaklines,
            },
            &[],
        )
        .map_err(BuildError::Region)?
    } else {
        baseline_mesh(baseline.points, baseline.triangles)?
    };
    let base: Vec<f64> = plan::vertices(&mesh.points)
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let mut heights = base.clone();
    let mut owners = vec![None; base.len()];
    for entry in &prepared {
        apply_entry(entry, &mesh.points, &mut heights, &mut owners)?;
    }
    let mut points = mesh.points.clone();
    let chunks = points.as_chunks_mut::<3>();
    for (height, xyz) in heights.iter().zip(chunks.0) {
        if let Some(slot) = xyz.get_mut(2) {
            *slot = *height;
        }
    }
    let volumes = exact_cut_fill(&ExactRequest {
        points: &points,
        base: &base,
        triangles: &mesh.triangles,
        inside: Some(&mesh.inside),
    })
    .map_err(BuildError::Volume)?;
    Ok(BuiltTerrain {
        points,
        base,
        owners,
        lines: lines.iter().map(|line| line.origin).collect(),
        mesh,
        volumes,
        revision: plan.revision,
        plan_hash: plan.hash,
    })
}

#[cfg(test)]
mod tests;
