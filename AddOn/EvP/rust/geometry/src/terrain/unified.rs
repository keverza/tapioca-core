//! #112 P3 unified terrain build: one immutable baseline, one compiled field
//! stack and one constrained result topology.
//!
//! This module is the pure-Rust landscape mesh authority. It assembles every
//! plan domain edge, hybrid-derived path line, mask boundary and fixed control
//! as constraints of one region rebuild, then evaluates the ordered field stack
//! on that topology. Region entries currently support existing/constant targets
//! with the reconciled absolute Grade, Add and Subtract rules, sampled path
//! bands, grading/protection masks and fixed-height point/line controls.
//! Additional surface modes and host integration
//! are later slices. The hard-step policy refuses side-by-side sharp
//! absolute-target domains that touch with no overlap: a shared boundary
//! segment would need two heights at one plan position. Ordered pairs carry
//! the change deliberately â€” an overlap blends it, and a strictly nested
//! override constrains it exactly through its inner rings, with the stack
//! order deciding which target wins inside.
//!
//! Cohesion review: this file intentionally keeps plan validation, one-graph
//! constraint assembly, ordered height composition, fixed-control resolution and
//! the public immutable result in one pipeline module. Splitting those steps would
//! make the single-build ordering contract harder to audit; advisory size reviews
//! are expected while the remaining path/surface slices are still added here.
//!
//! Coordinates are world metres in `f64`; heights are z in metres. The baseline
//! and plan are borrowed for the call and are never mutated. A refusal returns no
//! partial surface.

use std::collections::BTreeMap;

use super::design::{self, Design};
use super::edit::{Effect, smoother_step};
use super::field::{Field, FieldError};
use super::hybrid::{self, LineKind};
use super::plan;
use super::region::{RegionError, RegionMesh, RegionRequest, retriangulate_region_with_points};
use super::volume::VolumeError;
use super::volume::exact::{ExactReport, ExactRequest, exact_cut_fill};
use crate::identity::{ObjectId, OperationId, SourceId};
use crate::slope::Slope;

/// One authored constant-height claim an entry makes at an output vertex.
#[derive(Clone, Copy, Debug)]
struct TargetClaim<'a> {
    entry: u32,
    height: f64,
    boundary: bool,
    sharp: bool,
    parts: &'a [Part<'a>],
}

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
    /// Ordered path bands. Their `Path::order` follows every region entry.
    pub paths: &'a [Path<'a>],
    /// Grading limits and protected zones in caller order.
    pub masks: &'a [Mask<'a>],
    /// Fixed-height point/line controls in caller order.
    pub controls: &'a [Control<'a>],
    /// Absolute compatibility band for grading versus a fixed height, metres.
    pub control_tolerance: f64,
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

/// One ordered path band in the compiled plan.
#[derive(Clone, Copy, Debug)]
pub struct Path<'a> {
    /// Stack position after every region entry.
    pub order: u32,
    /// Authored path-centerline source identity.
    pub source: SourceId,
    /// Durable path-operation identity.
    pub operation: OperationId,
    /// One terrain-owning path object.
    pub object: ObjectId,
    /// Sampled design band; it contributes constraints and height targets only.
    pub band: design::Band<'a>,
    /// At-grade height band used by the design classification, metres.
    pub tolerance: f64,
}

/// Authored identity of one path line supplied as a region breakline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PathLineOrigin {
    /// Index into `Plan::paths`.
    pub path: u32,
    /// What the constrained path polyline is.
    pub kind: LineKind,
}

/// What a plan mask does to the ordered grading stack.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MaskKind {
    /// Only vertices inside/on at least one grading-limit mask may be graded.
    GradingLimit,
    /// Vertices inside/on the mask keep the baseline and are never graded.
    ProtectedZone,
}

/// One evaluated grading/protection mask with full domain extents.
#[derive(Clone, Copy, Debug)]
pub struct Mask<'a> {
    /// Durable mask-operation identity carried for diagnostics/provenance.
    pub operation: OperationId,
    /// Whether the mask limits grading or protects the baseline.
    pub kind: MaskKind,
    /// Evaluated mask ancestry, in caller order.
    pub sources: &'a [SourceId],
    /// Mask domain parts; membership is evaluated from these rings.
    pub parts: &'a [Part<'a>],
}

/// Authored identity of one mask edge supplied as a region breakline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaskLineOrigin {
    /// Index into `Plan::masks`.
    pub mask: u32,
    /// Index into `Mask::parts`.
    pub part: u32,
    /// Zero for the outer ring; one-based hole index otherwise.
    pub ring: u32,
    /// Zero-based ring segment index.
    pub segment: u32,
}

/// One immutable fixed-height point or open straight line control.
#[derive(Clone, Copy, Debug)]
pub struct Control<'a> {
    /// Authored source identity carried for diagnostics/provenance.
    pub source: SourceId,
    /// Authored vertex identity for a point control; zero for a line control.
    pub vertex: u64,
    /// True for one fixed point; false for one fixed open polyline.
    pub point: bool,
    /// Fixed xyz position of a point control, world metres.
    pub point_position: [f64; 3],
    /// Fixed open xyz polyline of a line control, world metres.
    pub line: &'a [f64],
}

/// Authored identity of one control line supplied as a region breakline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ControlLineOrigin {
    /// Index into `Plan::controls`.
    pub control: u32,
    /// Number of authored segments in the line.
    pub segments: u32,
}

/// A resolved fixed-height output vertex.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LockedVertex {
    /// Index into `Plan::controls`.
    pub control: u32,
    /// Output vertex whose height is owned by the control.
    pub vertex: u32,
    /// Fixed height, metres.
    pub height: f64,
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
    /// Per-path-line authored origin, after every domain line.
    pub path_lines: Vec<PathLineOrigin>,
    /// Per-mask-edge authored origin, after every domain and path line.
    pub mask_lines: Vec<MaskLineOrigin>,
    /// Per-control-line authored origin, after every domain and mask line.
    pub control_lines: Vec<ControlLineOrigin>,
    /// Every fixed-height vertex resolved from `Plan::controls`.
    pub locked: Vec<LockedVertex>,
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
    /// A path band has malformed stations or tolerance.
    PathShape {
        /// Index into `Plan::paths`.
        path: u32,
    },
    /// A path design parameter or slope is invalid.
    PathParameters {
        /// Index into `Plan::paths`.
        path: u32,
    },
    /// A path vertex cannot be evaluated on the region-composed field.
    PathCoverage {
        /// Index into `Plan::paths`.
        path: u32,
        /// Output vertex that left the field.
        vertex: u32,
    },
    /// A mask ring does not have whole xyz triples or enough vertices.
    MaskShape {
        /// Index into `Plan::masks`.
        mask: u32,
        /// Index into `Mask::parts`.
        part: u32,
        /// Ring index: zero outer, one-based hole.
        ring: u32,
    },
    /// A mask coordinate is NaN or infinite.
    MaskNonFinite {
        /// Index into `Plan::masks`.
        mask: u32,
        /// Index into `Mask::parts`.
        part: u32,
        /// Ring index: zero outer, one-based hole.
        ring: u32,
    },
    /// A mask ring has a zero-length segment.
    MaskZeroLength {
        /// Index into `Plan::masks`.
        mask: u32,
        /// Index into `Mask::parts`.
        part: u32,
        /// Ring index: zero outer, one-based hole.
        ring: u32,
        /// Zero-based segment index in the ring.
        segment: u32,
    },
    /// A point/line control names malformed geometry.
    ControlShape {
        /// Index into `Plan::controls`.
        control: u32,
    },
    /// A control coordinate or tolerance is NaN or infinite.
    ControlNonFinite {
        /// Index into `Plan::controls`.
        control: u32,
    },
    /// A line control has a zero-length segment.
    ControlZeroLength {
        /// Index into `Plan::controls`.
        control: u32,
        /// Zero-based segment index in the control line.
        segment: u32,
    },
    /// Grading moved a locked vertex outside the compatibility band, or two
    /// fixed controls disagree by more than that band.
    LockedConflict {
        /// Index into `Plan::controls`.
        control: u32,
        /// Output vertex with the conflict.
        vertex: u32,
        /// Fixed height requested by the control, metres.
        height: f64,
        /// Composed or canonical height found there, metres.
        actual: f64,
    },
    /// Two sharp absolute-target domains touch without an overlap or a transition
    /// that can represent their elevation change on one 2.5D surface.
    HardStep {
        /// Output vertex on the incompatible shared boundary.
        vertex: u32,
        /// First ordered entry claiming a height.
        first: u32,
        /// Second ordered entry claiming a different height.
        second: u32,
        /// First absolute target height, metres.
        first_height: f64,
        /// Second absolute target height, metres.
        second_height: f64,
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
            Self::PathShape { .. } => {
                "BuildPathShape: paths need two or more stations with distinct consecutive plan positions and a finite tolerance"
            }
            Self::PathParameters { .. } => {
                "BuildPathParameters: a path slope or design parameter is invalid"
            }
            Self::PathCoverage { .. } => {
                "BuildPathCoverage: a path vertex is off the region-composed field"
            }
            Self::MaskShape { .. } => {
                "BuildMaskShape: mask rings need whole xyz triples and three or more vertices"
            }
            Self::MaskNonFinite { .. } => "BuildMaskNonFinite: mask coordinates must be finite",
            Self::MaskZeroLength { .. } => {
                "BuildMaskZeroLength: a mask ring has a zero-length segment"
            }
            Self::ControlShape { .. } => {
                "BuildControlShape: point controls need xyz and line controls need two or more xyz vertices"
            }
            Self::ControlNonFinite { .. } => {
                "BuildControlNonFinite: fixed control coordinates and tolerance must be finite"
            }
            Self::ControlZeroLength { .. } => {
                "BuildControlZeroLength: a fixed control line has a zero-length segment"
            }
            Self::LockedConflict { .. } => {
                "BuildLockedConflict: grading or another fixed control disagrees with a locked height"
            }
            Self::HardStep { .. } => {
                "BuildHardStep: sharp absolute-target domains touch without an overlap or transition"
            }
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

#[derive(Clone, Debug)]
struct PlannedPathLine {
    points: Vec<f64>,
    origin: PathLineOrigin,
}

#[derive(Clone, Debug)]
struct PlannedMaskLine {
    points: Vec<f64>,
    origin: MaskLineOrigin,
}

#[derive(Clone, Debug)]
struct PlannedControlLine {
    points: Vec<f64>,
    control: u32,
}

#[derive(Clone, Copy, Debug)]
struct FixedHeight {
    control: u32,
    height: f64,
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

fn path_station_normals(
    stations: &[design::Station],
    path: u32,
) -> Result<Vec<[f64; 2]>, BuildError> {
    if stations.len() < 2 {
        return Err(BuildError::PathShape { path });
    }
    let last = stations.len() - 1;
    let mut normals = Vec::with_capacity(stations.len());
    for index in 0..stations.len() {
        let (Some(a), Some(b)) = (
            stations.get(index.saturating_sub(1)),
            stations.get(if index == last { index } else { index + 1 }),
        ) else {
            return Err(BuildError::PathShape { path });
        };
        let (dx, dy) = (b.position[0] - a.position[0], b.position[1] - a.position[1]);
        let length = dx.hypot(dy);
        if length <= plan::ON || !length.is_finite() {
            return Err(BuildError::PathShape { path });
        }
        normals.push([-dy / length, dx / length]);
    }
    Ok(normals)
}

fn push_path_lines(
    lines: &mut Vec<PlannedPathLine>,
    path_index: u32,
    path: &Path<'_>,
    field: &Field,
) -> Result<(), BuildError> {
    if !path.tolerance.is_finite() {
        return Err(BuildError::PathParameters { path: path_index });
    }
    path_station_normals(path.band.stations, path_index)?;
    let design = Design::new(&path.band, field, path.tolerance)
        .map_err(|_| BuildError::PathParameters { path: path_index })?;
    let (polylines, kinds) = hybrid::design_lines(&design)
        .map_err(|_| BuildError::PathParameters { path: path_index })?;
    for (points, kind) in polylines.into_iter().zip(kinds) {
        lines.push(PlannedPathLine {
            points,
            origin: PathLineOrigin {
                path: path_index,
                kind,
            },
        });
    }
    Ok(())
}

fn push_mask_lines(
    lines: &mut Vec<PlannedMaskLine>,
    mask: u32,
    part_index: u32,
    ring_index: u32,
    ring: &[f64],
) -> Result<(), BuildError> {
    if !ring.len().is_multiple_of(3) || plan::vertices(ring).len() < 3 {
        return Err(BuildError::MaskShape {
            mask,
            part: part_index,
            ring: ring_index,
        });
    }
    if ring.iter().any(|value| !value.is_finite()) {
        return Err(BuildError::MaskNonFinite {
            mask,
            part: part_index,
            ring: ring_index,
        });
    }
    let vertices = plan::vertices(ring);
    for segment in 0..vertices.len() {
        let next = (segment + 1) % vertices.len();
        let (Some(a), Some(b)) = (vertices.get(segment), vertices.get(next)) else {
            return Err(BuildError::MaskShape {
                mask,
                part: part_index,
                ring: ring_index,
            });
        };
        if a[0].to_bits() == b[0].to_bits() && a[1].to_bits() == b[1].to_bits() {
            return Err(BuildError::MaskZeroLength {
                mask,
                part: part_index,
                ring: ring_index,
                segment: u32::try_from(segment).unwrap_or(u32::MAX),
            });
        }
        let mut points = Vec::with_capacity(6);
        points.extend_from_slice(a);
        points.extend_from_slice(b);
        lines.push(PlannedMaskLine {
            points,
            origin: MaskLineOrigin {
                mask,
                part: part_index,
                ring: ring_index,
                segment: u32::try_from(segment).unwrap_or(u32::MAX),
            },
        });
    }
    Ok(())
}

fn validate_control(control: &Control<'_>, index: usize) -> Result<u32, BuildError> {
    let index = u32::try_from(index).map_err(|_| BuildError::ControlShape { control: u32::MAX })?;
    if control.point {
        if control.vertex == 0 || !control.point_position.iter().all(|value| value.is_finite()) {
            return Err(
                if control.point_position.iter().all(|value| value.is_finite()) {
                    BuildError::ControlShape { control: index }
                } else {
                    BuildError::ControlNonFinite { control: index }
                },
            );
        }
        return Ok(index);
    }
    let vertices = plan::vertices(control.line);
    if !control.line.len().is_multiple_of(3) || vertices.len() < 2 {
        return Err(BuildError::ControlShape { control: index });
    }
    if control.line.iter().any(|value| !value.is_finite()) {
        return Err(BuildError::ControlNonFinite { control: index });
    }
    Ok(index)
}

fn push_control_lines(
    lines: &mut Vec<PlannedControlLine>,
    controls: &[Control<'_>],
) -> Result<Vec<f64>, BuildError> {
    let mut spots = Vec::new();
    for (index, control) in controls.iter().enumerate() {
        let index = validate_control(control, index)?;
        if control.point {
            spots.extend_from_slice(&control.point_position);
            continue;
        }
        let vertices = plan::vertices(control.line);
        for segment in 0..vertices.len().saturating_sub(1) {
            let (Some(a), Some(b)) = (vertices.get(segment), vertices.get(segment + 1)) else {
                return Err(BuildError::ControlShape { control: index });
            };
            if a[0].to_bits() == b[0].to_bits() && a[1].to_bits() == b[1].to_bits() {
                return Err(BuildError::ControlZeroLength {
                    control: index,
                    segment: u32::try_from(segment).unwrap_or(u32::MAX),
                });
            }
        }
        lines.push(PlannedControlLine {
            points: control.line.to_vec(),
            control: index,
        });
    }
    Ok(spots)
}

fn control_height(control: &Control<'_>, x: f64, y: f64) -> Option<f64> {
    if control.point {
        return control.point_position.get(2).copied();
    }
    plan::nearest(plan::vertices(control.line), false, x, y).map(|near| near.z)
}

fn add_fixed(
    fixed: &mut BTreeMap<u32, FixedHeight>,
    vertex: u32,
    control: u32,
    height: f64,
    tolerance: f64,
) -> Result<(), BuildError> {
    match fixed.get(&vertex) {
        Some(existing) if (existing.height - height).abs() > tolerance => {
            Err(BuildError::LockedConflict {
                control,
                vertex,
                height,
                actual: existing.height,
            })
        }
        Some(_) => Ok(()),
        None => {
            fixed.insert(vertex, FixedHeight { control, height });
            Ok(())
        }
    }
}

fn resolve_controls(
    mesh: &RegionMesh,
    controls: &[Control<'_>],
    control_lines: &[PlannedControlLine],
    preceding_lines: usize,
    tolerance: f64,
) -> Result<BTreeMap<u32, FixedHeight>, BuildError> {
    let mut fixed = BTreeMap::new();
    for (point_index, control) in controls.iter().enumerate() {
        if !control.point {
            continue;
        }
        let index = u32::try_from(point_index)
            .map_err(|_| BuildError::ControlShape { control: u32::MAX })?;
        let Some(&vertex) = mesh.point_vertices.get(point_index) else {
            return Err(BuildError::Region(RegionError::Topology));
        };
        let Some(height) = control_height(control, f64::NAN, f64::NAN) else {
            return Err(BuildError::ControlShape { control: index });
        };
        add_fixed(&mut fixed, vertex, index, height, tolerance)?;
    }
    for (ordinal, planned) in control_lines.iter().enumerate() {
        let Some(control) = controls.get(planned.control as usize) else {
            return Err(BuildError::ControlShape {
                control: planned.control,
            });
        };
        let line = preceding_lines
            .checked_add(ordinal)
            .ok_or(BuildError::Region(RegionError::Topology))?;
        let line = u32::try_from(line).map_err(|_| BuildError::Region(RegionError::Topology))?;
        let mut touched = false;
        for edge in mesh.line_edges.iter().filter(|edge| edge.line == line) {
            touched = true;
            for &vertex in &edge.vertices {
                let position = plan::vertices(&mesh.points)
                    .get(vertex as usize)
                    .ok_or(BuildError::Region(RegionError::Topology))?;
                let height = control_height(control, position[0], position[1]).ok_or(
                    BuildError::ControlShape {
                        control: planned.control,
                    },
                )?;
                add_fixed(&mut fixed, vertex, planned.control, height, tolerance)?;
            }
        }
        if !touched {
            return Err(BuildError::Region(RegionError::Topology));
        }
    }
    Ok(fixed)
}

fn apply_paths(
    paths: &[Path<'_>],
    masks: &[Mask<'_>],
    mesh: &RegionMesh,
    heights: &mut [f64],
    owners: &mut [Option<u32>],
) -> Result<(), BuildError> {
    if paths.is_empty() {
        return Ok(());
    }
    for (path_index, path) in paths.iter().enumerate() {
        let path_index =
            u32::try_from(path_index).map_err(|_| BuildError::PathShape { path: u32::MAX })?;
        let mut current_points = mesh.points.clone();
        let chunks = current_points.as_chunks_mut::<3>();
        for (height, point) in heights.iter().zip(chunks.0) {
            if let Some(slot) = point.get_mut(2) {
                *slot = *height;
            }
        }
        let field =
            Field::new(&current_points, &mesh.triangles).map_err(|_| BuildError::PathCoverage {
                path: path_index,
                vertex: u32::MAX,
            })?;
        let design = Design::new(&path.band, &field, path.tolerance)
            .map_err(|_| BuildError::PathParameters { path: path_index })?;
        for (vertex, &[x, y, _]) in (0_u32..).zip(plan::vertices(&mesh.points)) {
            if is_protected(masks, x, y) || !grading_allowed(masks, x, y) {
                continue;
            }
            let sample = design
                .height(x, y)
                .map_err(|_| BuildError::PathParameters { path: path_index })?;
            let Some(sample) = sample else {
                return Err(BuildError::PathCoverage {
                    path: path_index,
                    vertex,
                });
            };
            if matches!(sample.region, design::Region::Base) {
                continue;
            }
            let current = heights
                .get(vertex as usize)
                .copied()
                .ok_or(BuildError::Region(RegionError::Topology))?;
            if let Some(slot) = heights.get_mut(vertex as usize) {
                *slot = sample.height;
            }
            if sample.height.to_bits() != current.to_bits()
                && let Some(owner) = owners.get_mut(vertex as usize)
            {
                *owner = Some(path.order);
            }
        }
    }
    Ok(())
}

fn apply_fixed(
    fixed: &BTreeMap<u32, FixedHeight>,
    tolerance: f64,
    heights: &mut [f64],
    owners: &mut [Option<u32>],
) -> Result<Vec<LockedVertex>, BuildError> {
    let mut locked = Vec::with_capacity(fixed.len());
    for (&vertex, &target) in fixed {
        let actual = heights
            .get(vertex as usize)
            .copied()
            .ok_or(BuildError::Region(RegionError::Topology))?;
        if (actual - target.height).abs() > tolerance {
            return Err(BuildError::LockedConflict {
                control: target.control,
                vertex,
                height: target.height,
                actual,
            });
        }
        if let Some(slot) = heights.get_mut(vertex as usize) {
            *slot = target.height;
        }
        if let Some(owner) = owners.get_mut(vertex as usize) {
            *owner = None;
        }
        locked.push(LockedVertex {
            control: target.control,
            vertex,
            height: target.height,
        });
    }
    Ok(locked)
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

fn inside_parts(parts: &[Part<'_>], x: f64, y: f64) -> bool {
    parts.iter().any(|part| {
        let outer = plan::inside(plan::vertices(part.outer), x, y);
        if outer == Some(false) {
            return false;
        }
        part.holes
            .iter()
            .all(|hole| plan::inside(plan::vertices(hole), x, y) != Some(true))
    })
}

fn on_entry_boundary(entry: &Entry<'_>, x: f64, y: f64) -> bool {
    entry.parts.iter().any(|part| {
        if plan::inside(plan::vertices(part.outer), x, y).is_none() {
            return true;
        }
        part.holes
            .iter()
            .any(|hole| plan::inside(plan::vertices(hole), x, y).is_none())
    })
}

fn parts_bounds(parts: &[Part<'_>]) -> Option<[f64; 4]> {
    let mut bounds = [
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    ];
    let mut any = false;
    for part in parts {
        for ring in std::iter::once(&part.outer).chain(part.holes.iter()) {
            for &[x, y, _] in plan::vertices(ring) {
                any = true;
                bounds[0] = bounds[0].min(x);
                bounds[1] = bounds[1].min(y);
                bounds[2] = bounds[2].max(x);
                bounds[3] = bounds[3].max(y);
            }
        }
    }
    any.then_some(bounds)
}

fn domains_overlap_near(a: &[Part<'_>], b: &[Part<'_>], _x: f64, _y: f64) -> bool {
    // A nonzero bounding-box intersection distinguishes ordered overlap from a
    // shared edge/point. It is a conservative topology classifier; strict
    // nesting is classified separately in `strictly_nested`.
    let (Some(a), Some(b)) = (parts_bounds(a), parts_bounds(b)) else {
        return false;
    };
    let width = a[2].min(b[2]) - a[0].max(b[0]);
    let height = a[3].min(b[3]) - a[1].max(b[1]);
    width > 1e-9 && height > 1e-9
}

/// Every ring vertex of `inner` lies strictly inside `outer`'s parts, with no
/// contact with `outer`'s rings. Strict nesting is one of the two ordered
/// hard-step shapes (the other a nonzero overlap in `domains_overlap_near`):
/// the inner rings become exact constraints, so the elevation step between
/// the two targets is representable and the stack order decides which target
/// wins inside the inner domain. Strictly nested domains never share boundary
/// vertices, so this classifier documents the policy where the shared-vertex
/// refusal below can never fire.
fn strictly_nested(outer: &[Part<'_>], inner: &[Part<'_>]) -> bool {
    let strict_inside = |parts: &[Part<'_>], x: f64, y: f64| {
        parts.iter().any(|part| {
            plan::inside(plan::vertices(part.outer), x, y) == Some(true)
                && part
                    .holes
                    .iter()
                    .all(|hole| plan::inside(plan::vertices(hole), x, y) == Some(false))
        })
    };
    !inner.is_empty()
        && inner.iter().all(|part| {
            std::iter::once(&part.outer)
                .chain(part.holes.iter())
                .all(|ring| {
                    plan::vertices(ring)
                        .iter()
                        .all(|&[x, y, _]| strict_inside(outer, x, y))
                })
        })
}

fn detect_hard_steps(claims: &BTreeMap<u32, Vec<TargetClaim>>) -> Result<(), BuildError> {
    for (&vertex, rows) in claims {
        for (index, first) in rows.iter().enumerate() {
            for second in rows.iter().skip(index + 1) {
                if first.entry == second.entry
                    || (first.height - second.height).abs() <= 1e-9
                    || !first.boundary
                    || !second.boundary
                    || !first.sharp
                    || !second.sharp
                    // Ordered pairs carry the elevation change deliberately:
                    // an overlap blends it (earlier entries already ceded the
                    // intersection), strict nesting constrains it exactly.
                    || domains_overlap_near(first.parts, second.parts, 0.0, 0.0)
                    || strictly_nested(first.parts, second.parts)
                    || strictly_nested(second.parts, first.parts)
                {
                    continue;
                }
                return Err(BuildError::HardStep {
                    vertex,
                    first: first.entry,
                    second: second.entry,
                    first_height: first.height,
                    second_height: second.height,
                });
            }
        }
    }
    Ok(())
}

fn is_protected(masks: &[Mask<'_>], x: f64, y: f64) -> bool {
    masks
        .iter()
        .any(|mask| mask.kind == MaskKind::ProtectedZone && inside_parts(mask.parts, x, y))
}

fn grading_allowed(masks: &[Mask<'_>], x: f64, y: f64) -> bool {
    let limited = masks.iter().any(|mask| mask.kind == MaskKind::GradingLimit);
    !limited
        || masks
            .iter()
            .any(|mask| mask.kind == MaskKind::GradingLimit && inside_parts(mask.parts, x, y))
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

#[expect(
    clippy::too_many_arguments,
    reason = "one ordered entry evaluation needs the immutable plan, mutable height/owner stacks and target-claim diagnostics together"
)]
fn apply_entry<'a>(
    prepared: &PreparedEntry<'a>,
    masks: &[Mask<'_>],
    mesh_points: &[f64],
    heights: &mut [f64],
    owners: &mut [Option<u32>],
    claims: &mut BTreeMap<u32, Vec<TargetClaim<'a>>>,
) -> Result<(), BuildError> {
    for (vertex, &[x, y, _]) in (0_u32..).zip(plan::vertices(mesh_points)) {
        if is_protected(masks, x, y) || !grading_allowed(masks, x, y) {
            continue;
        }
        let Some(current) = heights.get(vertex as usize).copied() else {
            return Err(BuildError::Region(RegionError::Topology));
        };
        let Some(next) = candidate(prepared, x, y, current) else {
            continue;
        };
        if let Some(height) = prepared.target {
            let sharp = matches!(
                prepared.transition,
                PreparedTransition::Daylight { reach, .. } if reach <= 0.0
            );
            claims.entry(vertex).or_default().push(TargetClaim {
                entry: prepared.entry.order,
                height,
                boundary: on_entry_boundary(prepared.entry, x, y),
                sharp,
                parts: prepared.entry.parts,
            });
        }
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

fn region_height_field(
    baseline: &Baseline<'_>,
    prepared: &[PreparedEntry<'_>],
    masks: &[Mask<'_>],
) -> Result<Field, BuildError> {
    let mut heights: Vec<f64> = plan::vertices(baseline.points)
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let mut owners = vec![None; heights.len()];
    let mut claims = BTreeMap::new();
    for entry in prepared {
        apply_entry(
            entry,
            masks,
            baseline.points,
            &mut heights,
            &mut owners,
            &mut claims,
        )?;
    }
    let mut points = baseline.points.to_vec();
    let chunks = points.as_chunks_mut::<3>();
    for (height, point) in heights.iter().zip(chunks.0) {
        if let Some(slot) = point.get_mut(2) {
            *slot = *height;
        }
    }
    Field::new(&points, baseline.triangles).map_err(|_| BuildError::PathCoverage {
        path: u32::MAX,
        vertex: u32::MAX,
    })
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
    if !plan.control_tolerance.is_finite() {
        return Err(BuildError::ControlNonFinite { control: u32::MAX });
    }
    if plan.control_tolerance < 0.0 {
        return Err(BuildError::Parameters {
            entry: u32::MAX,
            part: u32::MAX,
            field: "control tolerance",
        });
    }
    if (!plan.paths.is_empty() || !plan.masks.is_empty() || !plan.controls.is_empty())
        && baseline.property.is_none()
    {
        return Err(BuildError::PropertyRequired);
    }
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
    if let Some(property) = baseline.property {
        // Entry ring segments coinciding with the property ring are dropped:
        // the region rebuild already constrains the property ring, and a
        // duplicate breakline degenerates the triangulation. This is the
        // common "grade the whole property" case.
        let mut property_segments = std::collections::BTreeSet::new();
        let vertices = plan::vertices(property);
        for index in 0..vertices.len() {
            let (Some(&a), Some(&b)) = (
                vertices.get(index),
                vertices.get((index + 1) % vertices.len()),
            ) else {
                break;
            };
            let key = |&[x, y, _]: &[f64; 3]| (x.to_bits(), y.to_bits());
            let (ka, kb) = (key(&a), key(&b));
            property_segments.insert(if kb < ka { (kb, ka) } else { (ka, kb) });
        }
        let coincident = |line: &PlannedLine| {
            let vertices = plan::vertices(&line.points);
            let (Some(&a), Some(&b)) = (vertices.first(), vertices.get(1)) else {
                return false;
            };
            let key = |&[x, y, _]: &[f64; 3]| (x.to_bits(), y.to_bits());
            let (ka, kb) = (key(&a), key(&b));
            property_segments.contains(&if kb < ka { (kb, ka) } else { (ka, kb) })
        };
        lines.retain(|line| !coincident(line));
    }
    let path_field = if plan.paths.is_empty() {
        None
    } else {
        Some(region_height_field(baseline, &prepared, plan.masks)?)
    };
    let mut path_lines = Vec::new();
    for (path_index, path) in plan.paths.iter().enumerate() {
        let path_index =
            u32::try_from(path_index).map_err(|_| BuildError::PathShape { path: u32::MAX })?;
        let field = path_field.as_ref().ok_or(BuildError::PathCoverage {
            path: path_index,
            vertex: u32::MAX,
        })?;
        push_path_lines(&mut path_lines, path_index, path, field)?;
    }
    let mut mask_lines = Vec::new();
    for (mask_index, mask) in plan.masks.iter().enumerate() {
        let mask_index = u32::try_from(mask_index).map_err(|_| BuildError::MaskShape {
            mask: u32::MAX,
            part: u32::MAX,
            ring: u32::MAX,
        })?;
        for (part_index, part) in mask.parts.iter().enumerate() {
            let part_index = u32::try_from(part_index).map_err(|_| BuildError::MaskShape {
                mask: mask_index,
                part: u32::MAX,
                ring: u32::MAX,
            })?;
            push_mask_lines(&mut mask_lines, mask_index, part_index, 0, part.outer)?;
            for (hole_index, hole) in part.holes.iter().enumerate() {
                let ring = u32::try_from(hole_index + 1).map_err(|_| BuildError::MaskShape {
                    mask: mask_index,
                    part: part_index,
                    ring: u32::MAX,
                })?;
                push_mask_lines(&mut mask_lines, mask_index, part_index, ring, hole)?;
            }
        }
    }
    let mut control_lines = Vec::new();
    let spots = push_control_lines(&mut control_lines, plan.controls)?;
    let mesh = if let Some(property) = baseline.property {
        let mut breaklines = Vec::with_capacity(
            lines.len() + path_lines.len() + mask_lines.len() + control_lines.len(),
        );
        breaklines.extend(lines.iter().map(|line| line.points.as_slice()));
        breaklines.extend(path_lines.iter().map(|line| line.points.as_slice()));
        breaklines.extend(mask_lines.iter().map(|line| line.points.as_slice()));
        breaklines.extend(control_lines.iter().map(|line| line.points.as_slice()));
        retriangulate_region_with_points(
            &RegionRequest {
                points: baseline.points,
                triangles: baseline.triangles,
                region: property,
                breaklines: &breaklines,
            },
            &spots,
        )
        .map_err(BuildError::Region)?
    } else {
        baseline_mesh(baseline.points, baseline.triangles)?
    };
    let fixed = resolve_controls(
        &mesh,
        plan.controls,
        &control_lines,
        lines.len() + path_lines.len() + mask_lines.len(),
        plan.control_tolerance,
    )?;
    let base: Vec<f64> = plan::vertices(&mesh.points)
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let mut heights = base.clone();
    let mut owners = vec![None; base.len()];
    let mut claims = BTreeMap::new();
    for entry in &prepared {
        apply_entry(
            entry,
            plan.masks,
            &mesh.points,
            &mut heights,
            &mut owners,
            &mut claims,
        )?;
    }
    detect_hard_steps(&claims)?;
    apply_paths(plan.paths, plan.masks, &mesh, &mut heights, &mut owners)?;
    let locked = apply_fixed(&fixed, plan.control_tolerance, &mut heights, &mut owners)?;
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
        path_lines: path_lines.iter().map(|line| line.origin).collect(),
        mask_lines: mask_lines.iter().map(|line| line.origin).collect(),
        control_lines: control_lines
            .iter()
            .map(|line| ControlLineOrigin {
                control: line.control,
                segments: u32::try_from(plan::vertices(&line.points).len().saturating_sub(1))
                    .unwrap_or(u32::MAX),
            })
            .collect(),
        locked,
        mesh,
        volumes,
        revision: plan.revision,
        plan_hash: plan.hash,
    })
}

#[cfg(test)]
mod tests;
