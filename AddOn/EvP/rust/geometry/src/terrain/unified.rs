//! #112 P3 unified terrain build: one immutable baseline, one compiled field
//! stack and one constrained result topology.
//!
//! This module is the pure-Rust landscape mesh authority. It assembles every
//! plan domain edge, hybrid-derived path line, mask boundary and fixed control
//! as constraints of one region rebuild, then evaluates the ordered field stack
//! on that topology. Region entries support existing, constant and triangulated
//! targets with the absolute Grade, Add and Subtract rules, sampled path bands,
//! grading/protection masks and fixed-height point/line controls. Production
//! dispatch remains a later C++ integration phase.
//!
//! A zero-reach absolute target that differs from baseline at its boundary, or
//! from another sharp claim at the same position, refuses with `BuildHardStep`.
//! Overlap and nesting are not exemptions: one shared TIN vertex cannot represent
//! two heights. Use a positive-width transition; vertical duplication/retaining
//! geometry is a separate future output contract.
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
use super::field::{Field, FieldError, Segment};
use super::hybrid::{self, LineKind};
use super::outline::{self, Grading, OutlineError, SurfaceGrade, steepest};
use super::plan;
use super::region::{
    RegionError, RegionMesh, RegionRequest, retriangulate_region_split, retriangulate_with_supports,
};
use super::volume::VolumeError;
use super::volume::exact::{ExactReport, ExactRequest, exact_cut_fill};
use crate::identity::{ObjectId, OperationId, SourceId};
use crate::slope::Slope;

/// One authored constant-height claim an entry makes at an output vertex.
#[derive(Clone, Copy, Debug)]
struct TargetClaim {
    entry: u32,
    height: f64,
    boundary: bool,
    sharp: bool,
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
    /// Paved areas graded from their outline (#149 T1), at most one. Evaluated
    /// after every region entry and before the path bands.
    pub outlines: &'a [Outline<'a>],
}

/// One paved area graded from its outline (#149 T1): inside it the ground is
/// the paved surface, outside it the outline's slope-cone faces round onto the
/// ground composed by the region entries before it, exactly as the outline
/// lane grades them over the baseline ([`outline`]). Its private paved-TIN
/// supports, its caller face lines and spots join the one constrained rebuild.
#[derive(Clone, Copy, Debug)]
pub struct Outline<'a> {
    /// Stack position: after every region entry.
    pub order: u32,
    /// Flat xyz paved surface vertices.
    pub surface_points: &'a [f64],
    /// Counter-clockwise paved surface triples.
    pub surface_triangles: &'a [u32],
    /// Per paved vertex, the faces leaving it; empty grades every face by
    /// `grading`.
    pub surface_grades: &'a [SurfaceGrade],
    /// The faces' slopes, reach limit, rounding, effect and tolerance.
    pub grading: Grading,
    /// Open plan lines the faces lie on, every vertex strictly inside the
    /// rebuild ring.
    pub lines: &'a [&'a [f64]],
    /// Isolated plan points (the paved surface's interior vertices), strictly
    /// inside the rebuild ring.
    pub spots: &'a [f64],
}

/// Authored origin of one outline breakline: a caller face line, or a private
/// support carrying its paved-surface vertex pair.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OutlineLineOrigin {
    /// Index into `Plan::outlines`.
    pub outline: u32,
    /// The paved-TIN edge (sorted vertex pair) a support line was clipped from;
    /// `None` for a caller face line or a toe line.
    pub support: Option<[u32; 2]>,
    /// A traced toe line (#149 T2), after the caller face lines.
    pub toe: bool,
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
    pub surface: Surface<'a>,
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

/// Absolute surface target sampled inside an entry domain and extended from its
/// nearest boundary through the entry transition.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Surface<'a> {
    /// The current composed surface. Existing has no absolute target and cannot
    /// introduce a sharp absolute-target step.
    Existing,
    /// Absolute constant target height, metres.
    Constant {
        /// Target height inside the domain, metres.
        level: f64,
    },
    /// An upward target triangle mesh, such as a two-point, contour or sampled
    /// boundary-Z field. Inside its coverage the target is sampled exactly;
    /// outside it, the nearest target-boundary height is used.
    Triangulated {
        /// Flat xyz target vertices, world metres.
        points: &'a [f64],
        /// Counter-clockwise target triangle triples.
        triangles: &'a [u32],
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
    /// Per-outline-line origin, after every control line: the caller face
    /// lines, then the private paved supports.
    pub outline_lines: Vec<OutlineLineOrigin>,
    /// Outline vertices whose face ran out at its limit before meeting the
    /// ground (the composed ground is kept there: a step).
    pub outline_unreached: u32,
    /// Rebuild-ring samples where an outline face had not met the ground: the
    /// ring, not the limit, ends it in a step.
    pub outline_clipped: u32,
    /// Bounded located outline-step observations. Mesher strips precede reach-limit vertices and
    /// ring samples; counts above still include observations whose witnesses were not retained.
    pub outline_steps: Vec<outline::StepWitness>,
    /// How the regions' transitions were resolved in the published mesh (#219).
    pub transition: TransitionReport,
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
    /// An entry's triangulated target is not a valid upward height field.
    Surface {
        /// Index into `Plan::entries`.
        entry: u32,
        /// Target-field validation failure.
        error: FieldError,
    },
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
    /// The outline grading refused (#149 T1).
    Outline(OutlineError),
    /// More than one outline, or one placed before a region entry.
    OutlineOrder,
    /// A zero-reach absolute target differs from baseline (`second == u32::MAX`)
    /// or another sharp claim at one plan position. A shared TIN vertex cannot
    /// represent both heights without inventing an interpolated transition.
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
            Self::Surface { .. } => {
                "BuildSurface: the target must be an upward triangulated height field"
            }
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
            Self::Outline(error) => (*error).message(),
            Self::OutlineOrder => {
                "BuildOutlineOrder: one paved outline per build, after every region entry"
            }
            Self::HardStep { .. } => {
                "BuildHardStep: a zero-reach target needs two heights at one plan position"
            }
        }
    }
}

#[derive(Clone, Debug)]
enum PreparedTarget {
    Existing,
    Constant(f64),
    Surface {
        field: Field,
        boundary: Vec<Segment>,
    },
}

impl PreparedTarget {
    fn height(&self, x: f64, y: f64, current: f64) -> Option<f64> {
        match self {
            Self::Existing => Some(current),
            Self::Constant(level) => Some(*level),
            Self::Surface { field, boundary } => {
                if let Some(sample) = field.height_at(x, y) {
                    return Some(sample.height);
                }
                boundary
                    .iter()
                    .filter_map(|segment| plan::nearest(segment, false, x, y))
                    .min_by(|a, b| a.distance.total_cmp(&b.distance))
                    .map(|near| near.z)
            }
        }
    }

    const fn is_absolute(&self) -> bool {
        !matches!(self, Self::Existing)
    }

    /// The fill (highest) and cut (lowest) daylight faces over `(x, y)`,
    /// `distance` outside the domain whose nearest target height is `target`.
    /// A surface target's faces are the slope-cone envelope of its boundary
    /// (as outline grading's): each boundary point at height `h` offers
    /// `h - fill * d` and `h + cut * d`, so the faces stay continuous where two
    /// boundary edges' reaches meet. The nearest boundary height alone jumps
    /// there. A constant or existing target's boundary is level, where the
    /// envelope is the target's own cone.
    fn faces(
        &self,
        [x, y]: [f64; 2],
        distance: f64,
        target: f64,
        [cut, fill]: [f64; 2],
    ) -> (f64, f64) {
        let Self::Surface { boundary, .. } = self else {
            return (target - distance * fill, target + distance * cut);
        };
        if boundary.is_empty() {
            return (target - distance * fill, target + distance * cut);
        }
        let mut faces = (f64::NEG_INFINITY, f64::INFINITY);
        for &segment in boundary {
            if fill.is_finite() {
                let (run, height) = steepest(segment, [x, y], fill, false);
                faces.0 = faces.0.max(height - run * fill);
            }
            if cut.is_finite() {
                let (run, height) = steepest(segment, [x, y], cut, true);
                faces.1 = faces.1.min(height + run * cut);
            }
        }
        faces
    }
}

#[derive(Clone, Debug)]
struct PreparedEntry<'a> {
    entry: &'a Entry<'a>,
    target: PreparedTarget,
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
        Surface::Existing => PreparedTarget::Existing,
        Surface::Constant { level } => {
            if !level.is_finite() {
                return Err(BuildError::NonFinite {
                    entry: entry.order,
                    part: u32::MAX,
                    ring: u32::MAX,
                });
            }
            PreparedTarget::Constant(level)
        }
        Surface::Triangulated { points, triangles } => {
            let field = Field::new(points, triangles).map_err(|error| BuildError::Surface {
                entry: entry.order,
                error,
            })?;
            let boundary = field.boundary();
            PreparedTarget::Surface { field, boundary }
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
    // Spots are pushed for point controls only, in control order (push_control_lines): the n-th
    // point control's vertex is point_vertices[n], not point_vertices[its control index]. A line
    // control before a point used to shift the lookup past the end (#149: refused as a topology
    // error when a line and its own vertex were both fixed).
    let mut spot = 0_usize;
    for (control_index, control) in controls.iter().enumerate() {
        if !control.point {
            continue;
        }
        let index = u32::try_from(control_index)
            .map_err(|_| BuildError::ControlShape { control: u32::MAX })?;
        let Some(&vertex) = mesh.point_vertices.get(spot) else {
            return Err(BuildError::Region(RegionError::Topology));
        };
        spot += 1;
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

fn detect_hard_steps(claims: &BTreeMap<u32, Vec<TargetClaim>>) -> Result<(), BuildError> {
    // No overlap/nesting exemption: those shapes still need two heights on a
    // shared zero-reach ring. Ordered overlap is valid only with a transition.
    for (&vertex, rows) in claims {
        for (index, first) in rows.iter().enumerate() {
            for second in rows.iter().skip(index + 1) {
                if first.entry == second.entry
                    || (first.height - second.height).abs() <= 1e-9
                    || !first.boundary
                    || !second.boundary
                    || !first.sharp
                    || !second.sharp
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

#[derive(Clone, Copy, Debug)]
struct Candidate {
    next: f64,
    target: Option<f64>,
}

fn candidate(prepared: &PreparedEntry<'_>, x: f64, y: f64, current: f64) -> Option<Candidate> {
    if prepared.entry.parts.is_empty() {
        return None;
    }
    let distance = distance_to_parts(prepared.entry.parts, x, y)?;
    let target = prepared.target.height(x, y, current)?;
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
            } else {
                // Raised to the fill face, then lowered to the cut face: for a
                // level target exactly the former above/below-target choice.
                let (fill_face, cut_face) =
                    prepared.target.faces([x, y], distance, target, [cut, fill]);
                current.max(fill_face).min(cut_face)
            }
        }
    };
    let next = match prepared.effect {
        Effect::Cut => current.min(raw),
        Effect::Fill => current.max(raw),
        Effect::Both => raw,
    };
    Some(Candidate {
        next,
        target: prepared.target.is_absolute().then_some(target),
    })
}

#[expect(
    clippy::too_many_arguments,
    reason = "one ordered entry evaluation needs the immutable plan, mutable height/owner stacks and target-claim diagnostics together"
)]
fn apply_entry(
    prepared: &PreparedEntry<'_>,
    masks: &[Mask<'_>],
    mesh_points: &[f64],
    heights: &mut [f64],
    owners: &mut [Option<u32>],
    claims: &mut BTreeMap<u32, Vec<TargetClaim>>,
) -> Result<(), BuildError> {
    for (vertex, &[x, y, base]) in (0_u32..).zip(plan::vertices(mesh_points)) {
        if is_protected(masks, x, y) || !grading_allowed(masks, x, y) {
            continue;
        }
        let Some(current) = heights.get(vertex as usize).copied() else {
            return Err(BuildError::Region(RegionError::Topology));
        };
        let Some(Candidate { next, target }) = candidate(prepared, x, y, current) else {
            continue;
        };
        if let Some(_target) = target {
            let sharp = matches!(
                prepared.transition,
                PreparedTransition::Daylight { reach, .. } if reach <= 0.0
            );
            let boundary = on_entry_boundary(prepared.entry, x, y);
            if sharp && boundary && (next - base).abs() > 1e-9 {
                // A zero-reach target that differs from baseline needs two heights
                // at one plan position. Shared TIN vertices would silently turn it
                // into an arbitrary-width ramp, so refuse before publication.
                return Err(BuildError::HardStep {
                    vertex,
                    first: prepared.entry.order,
                    second: u32::MAX,
                    first_height: next,
                    second_height: base,
                });
            }
            claims.entry(vertex).or_default().push(TargetClaim {
                entry: prepared.entry.order,
                height: next,
                boundary,
                sharp,
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

/// The entry rings (less any segment on the property ring, which the region rebuild already
/// constrains) and the mask rings, as the build's breaklines.
fn ring_lines(
    baseline: &Baseline<'_>,
    plan: &Plan<'_>,
) -> Result<(Vec<PlannedLine>, Vec<PlannedMaskLine>), BuildError> {
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
    Ok((lines, mask_lines))
}

/// #219: the vertical error a resolved transition may keep between its vertices, metres.
/// Vertices are added along each ray and each constrained base edge until linear interpolation
/// between them is within it.
const TRANSITION_TOLERANCE: f64 = 0.01;
/// Rows every transition ray starts from: `k / TRANSITION_ROWS` of the spread.
const TRANSITION_ROWS: u32 = 4;
/// Halvings of one starting row a profile may refine to meet [`TRANSITION_TOLERANCE`]; a
/// profile still over it then is counted in [`TransitionReport::unresolved`].
const TRANSITION_DEPTH: u32 = 8;
/// The finest spacing a profile refines to, metres, whatever the spread: an interval this short
/// still over the tolerance is a step in the profile (a daylight face ending above the ground at
/// its reach), which no vertex resolves.
const TRANSITION_FINEST: f64 = 0.01;
/// A cap on the transition points of one split; past it the rays are spaced wider along the
/// rings and around the corners, and [`TransitionReport::widened`] says so.
const TRANSITION_POINT_CAP: usize = 20_000;
/// Directions around one ring corner: at least, and at most.
const TRANSITION_FAN: (u32, u32) = (16, 256);

/// How a build resolved its regions' transitions (#219).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransitionReport {
    /// The point cap spaced the rays wider than the tolerance asks: the transitions are
    /// coarser than the 1 cm vertical tolerance between rays.
    pub widened: bool,
    /// Rays and base edges whose profile was still over the tolerance at the finest spacing.
    pub unresolved: u32,
}

/// `(position, height)` nodes over `knots` (ascending; each interval checked on its own), with
/// every interval halved until the profile is within [`TRANSITION_TOLERANCE`] of linear on it
/// or it is no longer than `finest`. Each interval is checked at its midpoint and quarter
/// points: a crease anywhere inside it shows at least half its interpolation error at one of
/// them, so against half the tolerance. A knot off the ground ends the nodes there. Returns
/// whether the tolerance was met everywhere.
fn refine_profile(
    knots: &[f64],
    finest: f64,
    profile: &impl Fn(f64) -> Option<f64>,
) -> (Vec<(f64, f64)>, bool) {
    let over = |a: f64, za: f64, b: f64, zb: f64| {
        [0.25, 0.5, 0.75].iter().any(|&s| {
            profile(a + (b - a) * s)
                .is_some_and(|z| (z - (zb - za).mul_add(s, za)).abs() > 0.5 * TRANSITION_TOLERANCE)
        })
    };
    let mut nodes: Vec<(f64, f64)> = Vec::new();
    let mut resolved = true;
    for &knot in knots {
        let Some(z) = profile(knot) else {
            break; // Off the ground: nothing to resolve past it.
        };
        let Some(&(start, z_start)) = nodes.last() else {
            nodes.push((knot, z));
            continue;
        };
        // Depth-first halving, emitted in ascending order.
        let mut stack = vec![(knot, z)];
        let mut a = (start, z_start);
        while let Some(&(b, zb)) = stack.last() {
            if over(a.0, a.1, b, zb) {
                if 0.5 * (b - a.0) >= finest
                    && let Some(zm) = profile(a.0.midpoint(b))
                {
                    stack.push((a.0.midpoint(b), zm));
                    continue;
                }
                resolved = false;
            }
            stack.pop();
            nodes.push((b, zb));
            a = (b, zb);
        }
    }
    (nodes, resolved)
}

/// Offsets along one ray, ascending, ending at `spread`, so that the composed profile is within
/// [`TRANSITION_TOLERANCE`] of linear between consecutive offsets (zero, the ring, is implied),
/// the profile's steepest slope between them, and whether the tolerance was met.
fn transition_offsets(spread: f64, profile: impl Fn(f64) -> Option<f64>) -> (Vec<f64>, f64, bool) {
    let knots: Vec<f64> = (0..=TRANSITION_ROWS)
        .map(|row| spread * f64::from(row) / f64::from(TRANSITION_ROWS))
        .collect();
    let finest = finest_spacing(spread);
    let (nodes, resolved) = refine_profile(&knots, finest, &profile);
    let mut slope = 0.0_f64;
    let mut offsets = Vec::with_capacity(nodes.len());
    for pair in nodes.windows(2) {
        if let [(a, za), (b, zb)] = *pair {
            slope = slope.max((zb - za).abs() / (b - a));
            offsets.push(b);
        }
    }
    (offsets, slope, resolved)
}

/// The finest spacing a transition of `spread` refines to: [`TRANSITION_DEPTH`] halvings of a
/// starting row, but never under [`TRANSITION_FINEST`].
fn finest_spacing(spread: f64) -> f64 {
    (spread / f64::from(TRANSITION_ROWS) / f64::from(1_u32 << TRANSITION_DEPTH))
        .max(TRANSITION_FINEST)
}

/// The spread of an entry's transition, metres: its falloff blur or daylight reach.
const fn spread_of(entry: &PreparedEntry<'_>) -> f64 {
    match entry.transition {
        PreparedTransition::Falloff { blur } => blur,
        PreparedTransition::Daylight { reach, .. } => reach,
    }
}

/// The ground the region entries compose at a plan point, masks aside: `ground` with every
/// entry's candidate applied in order.
fn composed_height(prepared: &[PreparedEntry<'_>], ground: &Field, x: f64, y: f64) -> Option<f64> {
    ground.height_at(x, y).map(|sample| {
        prepared.iter().fold(sample.height, |current, entry| {
            candidate(entry, x, y, current).map_or(current, |next| next.next)
        })
    })
}

/// Isolated points that resolve every entry's falloff or daylight transition: along outward rays
/// from each outer-ring segment and fanned around each ring corner, at offsets chosen so that the
/// composed surface is within [`TRANSITION_TOLERANCE`] of linear between them along the ray
/// (#219; four fixed rows before it, #209). The last point of a ray sits at the full spread,
/// where the transition ends. Without them the split interpolates a transition from its ring to
/// the next baseline vertex, which on a coarse baseline can be the far corner of the site.
/// Constrained base edges across a transition get their vertices from [`TransitionEdges`].
///
/// The profile along a ray is the regions' composition over `ground` (masks aside). Points are
/// kept only on their ray's offset curve, strictly inside the property, clear of every line and
/// caller spot and of each other; they are vertices, never constraints, so they cannot cross
/// anything. Both the ground the path and outline stages design against and the published mesh
/// take them. O(rays x refinement x entries x ring segments).
fn transition_points(
    prepared: &[PreparedEntry<'_>],
    ground: &Field,
    property: &[f64],
    rings: &[&[f64]],
    spots: &[f64],
) -> (Vec<f64>, TransitionReport) {
    let (points, report) = transition_points_spaced(prepared, ground, property, rings, spots, 1.0);
    let count = points.len() / 3;
    if count <= TRANSITION_POINT_CAP {
        return (points, report);
    }
    #[expect(
        clippy::cast_precision_loss,
        reason = "a ratio of point counts far below 2^52"
    )]
    let widen = count as f64 / TRANSITION_POINT_CAP as f64;
    let (points, mut report) =
        transition_points_spaced(prepared, ground, property, rings, spots, widen);
    report.widened = true;
    (points, report)
}

#[expect(
    clippy::too_many_arguments,
    reason = "the ray pass of transition_points, again with the rays spaced wider past the cap"
)]
fn transition_points_spaced(
    prepared: &[PreparedEntry<'_>],
    ground: &Field,
    property: &[f64],
    rings: &[&[f64]],
    spots: &[f64],
    widen: f64,
) -> (Vec<f64>, TransitionReport) {
    let boundary = plan::vertices(property);
    let mut points: Vec<f64> = Vec::new();
    let mut report = TransitionReport::default();
    let mut seen = std::collections::BTreeSet::new();
    for entry in prepared {
        let spread = spread_of(entry);
        if !(spread > 0.0 && spread.is_finite()) {
            continue;
        }
        let cell = 0.4 * finest_spacing(spread);
        // One ray: its resolved offsets offered as points, its steepest slope returned.
        let mut ray = |[ox, oy]: [f64; 2], [ux, uy]: [f64; 2]| -> f64 {
            let (offsets, slope, resolved) = transition_offsets(spread, |d| {
                composed_height(prepared, ground, ux.mul_add(d, ox), uy.mul_add(d, oy))
            });
            let mut kept = false;
            let mut previous = 0.0;
            for (index, &offset) in offsets.iter().enumerate() {
                let next = offsets
                    .get(index + 1)
                    .copied()
                    .unwrap_or(2.0f64.mul_add(offset, -previous));
                let clearance = 0.4 * (offset - previous).min(next - offset);
                previous = offset;
                let (x, y) = (ux.mul_add(offset, ox), uy.mul_add(offset, oy));
                // Only a point on its offset curve: a fan direction off the corner's outward
                // wedge, or a side's ray at a concave corner, lands nearer the ring than its
                // offset. There it would be a lone vertex a little off its neighbours, a fold in
                // the published surface the transition does not have.
                let Some(distance) = distance_to_parts(entry.entry.parts, x, y) else {
                    continue;
                };
                if distance <= clearance || (distance - offset).abs() > 1e-9 * spread.max(1.0) {
                    continue;
                }
                if plan::inside(boundary, x, y) != Some(true)
                    || plan::nearest(boundary, true, x, y).is_none_or(|n| n.distance <= clearance)
                {
                    continue;
                }
                if rings.iter().any(|line| {
                    plan::nearest(plan::vertices(line), false, x, y)
                        .is_some_and(|n| n.distance <= clearance)
                }) || plan::vertices(spots)
                    .iter()
                    .any(|&[sx, sy, _]| (sx - x).hypot(sy - y) <= clearance)
                {
                    continue;
                }
                #[expect(
                    clippy::cast_possible_truncation,
                    reason = "a dedupe key: a fine cell of a finite plan position"
                )]
                let key = ((x / cell).round() as i64, (y / cell).round() as i64);
                if seen.insert(key) {
                    points.extend([x, y, 0.0]);
                    kept = true;
                }
            }
            if kept && !resolved {
                report.unresolved = report.unresolved.saturating_add(1);
            }
            slope
        };
        for part in entry.entry.parts {
            let ring = plan::vertices(part.outer);
            let count = ring.len();
            let mut steepest = 0.0_f64;
            for index in 0..count {
                let (Some(&[ax, ay, _]), Some(&[bx, by, _])) =
                    (ring.get(index), ring.get((index + 1) % count))
                else {
                    continue;
                };
                let length = (bx - ax).hypot(by - ay);
                if length <= 0.0 {
                    continue;
                }
                let (ux, uy) = ((bx - ax) / length, (by - ay) / length);
                #[expect(
                    clippy::cast_possible_truncation,
                    clippy::cast_sign_loss,
                    reason = "a positive station count, clamped"
                )]
                let stations = ((length / (0.5 * spread * widen)).ceil() as u32).clamp(1, 1 << 16);
                // Both normals: the offset-curve test keeps the outward side whichever the winding.
                for side in [-1.0, 1.0] {
                    for station in 0..=stations {
                        let t = length * f64::from(station) / f64::from(stations);
                        steepest = steepest.max(ray(
                            [ux.mul_add(t, ax), uy.mul_add(t, ay)],
                            [-uy * side, ux * side],
                        ));
                    }
                }
            }
            // Around each corner, a fan fine enough that its chords sag less than the tolerance
            // at the steepest slope the sides showed: `spread * (1 - cos(pi / n)) * slope`.
            let sag = TRANSITION_TOLERANCE / (steepest * spread).max(f64::MIN_POSITIVE);
            let wanted = if sag >= 1.0 {
                f64::from(TRANSITION_FAN.0)
            } else {
                (std::f64::consts::PI / (1.0 - sag).acos()).ceil() / widen
            };
            #[expect(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                reason = "a positive direction count, clamped"
            )]
            let fan = (wanted.max(1.0) as u32).clamp(TRANSITION_FAN.0, TRANSITION_FAN.1);
            for &[ax, ay, _] in ring {
                for step in 0..fan {
                    let angle = std::f64::consts::TAU * f64::from(step) / f64::from(fan);
                    ray([ax, ay], [angle.cos(), angle.sin()]);
                }
            }
        }
    }
    (points, report)
}

/// Vertices on the constrained base edges that cross a transition (#219). A base edge is a
/// constraint of the rebuild (it keeps the base affine under every output triangle), so a
/// transition crossing it is interpolated along it from vertex to vertex whatever points lie
/// beside it: from a region's ring to a site corner, on a coarse baseline. The rays' points
/// never land exactly on it, so the edge is split instead, at positions refined along it to
/// [`TRANSITION_TOLERANCE`] like a ray, then thinned to the ones the profile needs.
struct TransitionEdges<'a> {
    prepared: &'a [PreparedEntry<'a>],
    ground: &'a Field,
    /// The lines of the rebuild: a crossing with one is a crease of the profile and gets the
    /// triangulation's own vertex, so the split keeps clear of it.
    lines: &'a [&'a [f64]],
    /// Each transition's plan bounds (ring bounds grown by its spread) and spread.
    belts: Vec<([f64; 4], f64)>,
    unresolved: std::cell::Cell<u32>,
}

impl<'a> TransitionEdges<'a> {
    fn new(prepared: &'a [PreparedEntry<'a>], ground: &'a Field, lines: &'a [&'a [f64]]) -> Self {
        let mut belts = Vec::new();
        for entry in prepared {
            let spread = spread_of(entry);
            if !(spread > 0.0 && spread.is_finite()) {
                continue;
            }
            for part in entry.entry.parts {
                let mut bounds = [
                    f64::INFINITY,
                    f64::INFINITY,
                    f64::NEG_INFINITY,
                    f64::NEG_INFINITY,
                ];
                for &[x, y, _] in plan::vertices(part.outer) {
                    bounds = [
                        bounds[0].min(x),
                        bounds[1].min(y),
                        bounds[2].max(x),
                        bounds[3].max(y),
                    ];
                }
                if bounds[0] <= bounds[2] {
                    belts.push((
                        [
                            bounds[0] - spread,
                            bounds[1] - spread,
                            bounds[2] + spread,
                            bounds[3] + spread,
                        ],
                        spread,
                    ));
                }
            }
        }
        Self {
            prepared,
            ground,
            lines,
            belts,
            unresolved: std::cell::Cell::new(0),
        }
    }

    /// Edge parameters strictly between 0 and 1, ascending, where the edge `a`-`b` takes a
    /// vertex.
    fn splits(&self, [ax, ay]: [f64; 2], [bx, by]: [f64; 2]) -> Vec<f64> {
        let length = (bx - ax).hypot(by - ay);
        if !length.is_finite() || length <= 0.0 {
            return Vec::new();
        }
        // The edge's parameter range inside each belt's bounds, and the narrowest spread there.
        let (mut low, mut high, mut spread) = (1.0_f64, 0.0_f64, f64::INFINITY);
        for &([x0, y0, x1, y1], belt) in &self.belts {
            let (mut t0, mut t1) = (0.0_f64, 1.0_f64);
            for (start, delta, min, max) in [(ax, bx - ax, x0, x1), (ay, by - ay, y0, y1)] {
                if delta == 0.0 {
                    if start < min || start > max {
                        t0 = 1.0;
                        t1 = 0.0;
                    }
                } else {
                    let (u, v) = ((min - start) / delta, (max - start) / delta);
                    t0 = t0.max(u.min(v));
                    t1 = t1.min(u.max(v));
                }
            }
            if t0 < t1 {
                low = low.min(t0);
                high = high.max(t1);
                spread = spread.min(belt);
            }
        }
        if low >= high {
            return Vec::new();
        }
        // Knots: the range sampled a quarter spread apart (a check over a longer interval
        // could miss a narrow belt), and every crossing with a line, a crease the
        // triangulation gives its own vertex.
        let step = 0.25 * spread / length;
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "a positive knot count, clamped"
        )]
        let count = (((high - low) / step).ceil() as u32).clamp(1, 1 << 16);
        // The ends are knots too: the split's profile is checked from vertex to vertex.
        let mut knots: Vec<f64> = (0..=count)
            .map(|k| (high - low).mul_add(f64::from(k) / f64::from(count), low))
            .chain([0.0, 1.0])
            .collect();
        let mut creases = Vec::new();
        for line in self.lines {
            for pair in plan::vertices(line).windows(2) {
                if let [[px, py, _], [qx, qy, _]] = *pair
                    && let Some(t) = crossing([ax, ay], [bx, by], [px, py], [qx, qy])
                    && t > low
                    && t < high
                {
                    creases.push(t);
                }
            }
        }
        knots.extend(&creases);
        knots.sort_by(f64::total_cmp);
        knots.dedup();
        let finest = finest_spacing(spread);
        let (nodes, resolved) = refine_profile(&knots, finest / length, &|t: f64| {
            composed_height(
                self.prepared,
                self.ground,
                (bx - ax).mul_add(t, ax),
                (by - ay).mul_add(t, ay),
            )
        });
        let kept = thin(&nodes, 0.5 * TRANSITION_TOLERANCE);
        let clear = 0.4 * finest / length;
        let splits: Vec<f64> = kept
            .into_iter()
            .filter(|&t| {
                t > clear
                    && t < 1.0 - clear
                    && creases.iter().all(|&crease| (crease - t).abs() > clear)
            })
            .collect();
        if !resolved && !splits.is_empty() {
            self.unresolved.set(self.unresolved.get().saturating_add(1));
        }
        splits
    }
}

/// The parameter along `a`-`b` where it properly crosses `p`-`q`, if it does.
fn crossing(
    [ax, ay]: [f64; 2],
    [bx, by]: [f64; 2],
    [px, py]: [f64; 2],
    [qx, qy]: [f64; 2],
) -> Option<f64> {
    let (rx, ry) = (bx - ax, by - ay);
    let (sx, sy) = (qx - px, qy - py);
    let denominator = rx.mul_add(sy, -(ry * sx));
    if denominator == 0.0 {
        return None;
    }
    let (wx, wy) = (px - ax, py - ay);
    let along = wx.mul_add(sy, -(wy * sx)) / denominator;
    let across = wx.mul_add(ry, -(wy * rx)) / denominator;
    ((0.0..=1.0).contains(&along) && (0.0..=1.0).contains(&across)).then_some(along)
}

/// The interior positions of `nodes` a piecewise-linear profile within `tolerance` of every node
/// needs (Douglas-Peucker on height): the ends are implied.
fn thin(nodes: &[(f64, f64)], tolerance: f64) -> Vec<f64> {
    let mut keep = vec![false; nodes.len()];
    let mut stack = vec![(0, nodes.len().saturating_sub(1))];
    while let Some((first, last)) = stack.pop() {
        let (Some(&(ta, za)), Some(&(tb, zb))) = (nodes.get(first), nodes.get(last)) else {
            continue;
        };
        let mut worst = (0.0_f64, 0);
        for (index, &(t, z)) in nodes.iter().enumerate().take(last).skip(first + 1) {
            let error = (z - (zb - za).mul_add((t - ta) / (tb - ta), za)).abs();
            if error > worst.0 {
                worst = (error, index);
            }
        }
        if worst.0 > tolerance {
            if let Some(slot) = keep.get_mut(worst.1) {
                *slot = true;
            }
            stack.push((first, worst.1));
            stack.push((worst.1, last));
        }
    }
    nodes
        .iter()
        .zip(keep)
        .filter_map(|(&(t, _), kept)| kept.then_some(t))
        .collect()
}

/// The ground the region entries compose, as one [`Field`] for the path and outline stages.
///
/// #209: composed at the vertices of the baseline split by the entry and mask rings and by
/// [`transition_points`], so a region between the baseline's own vertices, and its
/// transition, are part of it. The field stays piecewise linear (daylight roots stay
/// closed-form); a transition is linear between its rows of points. O(transition points x
/// ring segments) plus one region split; worker-classified.
fn region_height_field(
    baseline: &Baseline<'_>,
    (entry_lines, mask_lines): (&[PlannedLine], &[PlannedMaskLine]),
    prepared: &[PreparedEntry<'_>],
    masks: &[Mask<'_>],
) -> Result<Field, BuildError> {
    if prepared.is_empty() {
        return Field::new(baseline.points, baseline.triangles).map_err(BuildError::Baseline);
    }
    // Without a property line there is nothing to split inside: the baseline's own vertices.
    let split = match baseline.property {
        Some(property) => {
            let mut rings: Vec<&[f64]> = entry_lines
                .iter()
                .map(|line| line.points.as_slice())
                .collect();
            rings.extend(mask_lines.iter().map(|line| line.points.as_slice()));
            let ground =
                Field::new(baseline.points, baseline.triangles).map_err(BuildError::Baseline)?;
            let (spots, _) = transition_points(prepared, &ground, property, &rings, &[]);
            let edges = TransitionEdges::new(prepared, &ground, &rings);
            retriangulate_region_split(
                &RegionRequest {
                    points: baseline.points,
                    triangles: baseline.triangles,
                    region: property,
                    breaklines: &rings,
                },
                &spots,
                &|a, b| edges.splits(a, b),
            )
            .map_err(BuildError::Region)?
        }
        None => baseline_mesh(baseline.points, baseline.triangles)?,
    };
    let mut heights: Vec<f64> = plan::vertices(&split.points)
        .iter()
        .map(|&[_, _, z]| z)
        .collect();
    let mut owners = vec![None; heights.len()];
    let mut claims = BTreeMap::new();
    for entry in prepared {
        apply_entry(
            entry,
            masks,
            &split.points,
            &mut heights,
            &mut owners,
            &mut claims,
        )?;
    }
    let mut points = split.points;
    let chunks = points.as_chunks_mut::<3>();
    for (height, point) in heights.iter().zip(chunks.0) {
        if let Some(slot) = point.get_mut(2) {
            *slot = *height;
        }
    }
    Field::new(&points, &split.triangles).map_err(|_| BuildError::PathCoverage {
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
    let _running = super::progress::Running::begin();
    let base_field =
        Field::new(baseline.points, baseline.triangles).map_err(BuildError::Baseline)?;
    let mut transition = TransitionReport::default();
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
    if (!plan.paths.is_empty()
        || !plan.masks.is_empty()
        || !plan.controls.is_empty()
        || !plan.outlines.is_empty())
        && baseline.property.is_none()
    {
        return Err(BuildError::PropertyRequired);
    }
    if plan.outlines.len() > 1
        || plan
            .outlines
            .iter()
            .any(|o| (o.order as usize) < plan.entries.len())
    {
        return Err(BuildError::OutlineOrder);
    }
    let paved = plan
        .outlines
        .first()
        .map(|o| {
            outline::Operator::new(
                baseline.points,
                baseline.triangles,
                o.surface_points,
                o.surface_triangles,
                o.surface_grades,
                o.grading,
            )
            .map(|operator| (o, operator))
        })
        .transpose()
        .map_err(BuildError::Outline)?;
    let (lines, mask_lines) = ring_lines(baseline, plan)?;
    // #209: one composed ground for the path and outline stages, built only when one needs it.
    let mut ground_field = if plan.paths.is_empty() && paved.is_none() {
        None
    } else {
        Some(region_height_field(
            baseline,
            (&lines, &mask_lines),
            &prepared,
            plan.masks,
        )?)
    };
    let mut path_lines = Vec::new();
    for (path_index, path) in plan.paths.iter().enumerate() {
        let path_index =
            u32::try_from(path_index).map_err(|_| BuildError::PathShape { path: u32::MAX })?;
        let field = ground_field.as_ref().ok_or(BuildError::PathCoverage {
            path: path_index,
            vertex: u32::MAX,
        })?;
        push_path_lines(&mut path_lines, path_index, path, field)?;
    }
    let mut control_lines = Vec::new();
    let spots = push_control_lines(&mut control_lines, plan.controls)?;
    let mut outline_lines = Vec::new();
    let mut outline_ground = None;
    let mut outline_steps = Vec::new();
    let mesh = if let Some(property) = baseline.property {
        let mut breaklines = Vec::with_capacity(
            lines.len() + path_lines.len() + mask_lines.len() + control_lines.len(),
        );
        breaklines.extend(lines.iter().map(|line| line.points.as_slice()));
        breaklines.extend(path_lines.iter().map(|line| line.points.as_slice()));
        breaklines.extend(mask_lines.iter().map(|line| line.points.as_slice()));
        breaklines.extend(control_lines.iter().map(|line| line.points.as_slice()));
        let request = |breaklines| RegionRequest {
            points: baseline.points,
            triangles: baseline.triangles,
            region: property,
            breaklines,
        };
        if let Some((planned, operator)) = &paved {
            // The outline lane's rebuild: its face lines and the paved surface's
            // interior vertices as caller constraints (control spots first, so
            // their indices are unchanged), its toe lines (#149 T2), and its
            // paved TIN edges as private supports stitched into the ring.
            // Coplanar base edges are freed there as in that lane.
            breaklines.extend(planned.lines.iter().copied());
            let supports = outline::supports::prepare(
                property,
                planned.surface_points,
                planned.surface_triangles,
            )
            .map_err(BuildError::Outline)?;
            let all_spots = [spots.as_slice(), planned.spots].concat();
            // The toe is traced over the ground the outline grades: the baseline, or the
            // region-composed surface when region entries come first.
            outline_ground = ground_field.take();
            let ground = |x: f64, y: f64| {
                outline_ground
                    .as_ref()?
                    .height_at(x, y)
                    .map(|sample| sample.height)
            };
            let faces = operator.faces();
            let mut obstacles = breaklines.clone();
            obstacles.extend(supports.lines.iter().map(Vec::as_slice));
            let toe = outline::toe::lines(
                &faces,
                &ground,
                &supports.ring,
                &obstacles,
                &all_spots,
                operator.toe_rounding(),
                outline::toe::Toe::DEFAULT,
            );
            // The toe and creases follow every other line in the rebuild.
            let first_traced = u32::try_from(breaklines.len()).unwrap_or(u32::MAX);
            let mut with_toe = breaklines.clone();
            with_toe.extend(toe.iter().map(Vec::as_slice));
            // #209: the regions' transitions resolved in the published mesh, after every
            // caller spot so their indices are unchanged.
            let mut clear_of = with_toe.clone();
            clear_of.extend(supports.lines.iter().map(Vec::as_slice));
            let (resolved, report) =
                transition_points(&prepared, &base_field, property, &clear_of, &all_spots);
            let all_spots = [all_spots.as_slice(), &resolved].concat();
            let edges = TransitionEdges::new(&prepared, &base_field, &clear_of);
            let splits = |a, b| edges.splits(a, b);
            let caller = |toe_count: usize| {
                let mut origins: Vec<OutlineLineOrigin> = planned
                    .lines
                    .iter()
                    .map(|_| OutlineLineOrigin {
                        outline: 0,
                        support: None,
                        toe: false,
                    })
                    .collect();
                origins.extend((0..toe_count).map(|_| OutlineLineOrigin {
                    outline: 0,
                    support: None,
                    toe: true,
                }));
                origins.extend(supports.sources.iter().map(|&source| OutlineLineOrigin {
                    outline: 0,
                    support: Some(source),
                    toe: false,
                }));
                origins
            };
            // As in the outline lane: a toe the triangulation cannot take is left out.
            let rebuild_with = |lines: &[&[f64]]| {
                retriangulate_with_supports(
                    &RegionRequest {
                        points: baseline.points,
                        triangles: baseline.triangles,
                        region: property,
                        breaklines: lines,
                    },
                    &all_spots,
                    &supports.ring,
                    &supports.lines,
                    Some(&splits),
                )
            };
            let (rebuilt, traced) = match rebuild_with(&with_toe) {
                Ok(mesh) => {
                    outline_lines = caller(toe.len());
                    let count = u32::try_from(toe.len()).unwrap_or(u32::MAX);
                    (mesh, [first_traced, first_traced.saturating_add(count)])
                }
                Err(_) if !toe.is_empty() => {
                    outline_lines = caller(0);
                    rebuild_with(&breaklines)
                        .map_err(BuildError::Region)
                        .map(|mesh| (mesh, [first_traced, first_traced]))?
                }
                Err(error) => return Err(BuildError::Region(error)),
            };
            transition = TransitionReport {
                unresolved: report.unresolved.saturating_add(edges.unresolved.get()),
                ..report
            };
            // #149 T3: the outline's faces get their vertices from the error and the angles
            // they leave (the mesher), over the same ground the toe was traced over. An
            // uncertified vertical-error bound refuses the build rather than publishing `rebuilt`.
            let allowed =
                |x, y| !is_protected(plan.masks, x, y) && grading_allowed(plan.masks, x, y);
            let (meshed, _, steps) = operator
                .meshed(
                    &ground,
                    &allowed,
                    (&rebuilt, traced),
                    outline::Refine::DEFAULT,
                    [
                        (baseline.points, baseline.triangles),
                        (planned.surface_points, planned.surface_triangles),
                    ],
                )
                .map_err(BuildError::Outline)?;
            outline_steps = steps;
            meshed
        } else {
            // #209: the regions' transitions resolved in the published mesh, after the
            // control spots so their indices are unchanged.
            let (resolved, report) =
                transition_points(&prepared, &base_field, property, &breaklines, &spots);
            let all_spots = [spots.as_slice(), &resolved].concat();
            let edges = TransitionEdges::new(&prepared, &base_field, &breaklines);
            let mesh = retriangulate_region_split(&request(&breaklines), &all_spots, &|a, b| {
                edges.splits(a, b)
            })
            .map_err(BuildError::Region)?;
            transition = TransitionReport {
                unresolved: report.unresolved.saturating_add(edges.unresolved.get()),
                ..report
            };
            mesh
        }
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
    let (outline_unreached, outline_clipped, observed_steps) = match &paved {
        Some((planned, operator)) => apply_outline(
            planned,
            operator,
            outline_ground
                .as_ref()
                .ok_or(BuildError::Outline(outline::OutlineError::Field))?,
            plan.masks,
            &mesh,
            &mut heights,
            &mut owners,
        )?,
        None => (0, 0, Vec::new()),
    };
    outline_steps.extend(observed_steps);
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
        outline_lines,
        outline_unreached,
        outline_clipped,
        outline_steps,
        transition,
        locked,
        mesh,
        volumes,
        revision: plan.revision,
        plan_hash: plan.hash,
    })
}

/// Grade the paved outline over the composed heights (#149 T1): as the outline
/// lane, a vertex of an outside triangle keeps its height and every other
/// vertex takes the outline design over the ground composed there, except
/// where a mask forbids grading. Returns the unreached vertex count, clipped ring sample count and
/// their bounded located witnesses.
#[expect(
    clippy::too_many_arguments,
    reason = "one outline stage needs the plan stage, its operator, the masks, the mesh and the mutable height/owner stacks together"
)]
fn apply_outline(
    planned: &Outline<'_>,
    operator: &outline::Operator,
    ground: &Field,
    masks: &[Mask<'_>],
    mesh: &RegionMesh,
    heights: &mut [f64],
    owners: &mut [Option<u32>],
) -> Result<(u32, u32, Vec<outline::StepWitness>), BuildError> {
    let faces = operator.faces();
    let vertices = plan::vertices(&mesh.points);
    let mut fixed = vec![false; vertices.len()];
    for (corners, _) in mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&mesh.inside)
        .filter(|(_, inside)| !**inside)
    {
        for &vertex in corners {
            if let Some(flag) = fixed.get_mut(vertex as usize) {
                *flag = true;
            }
        }
    }
    // Ring diagnostics query the same composed field as this stage's grading, never a new height
    // field filtered by the steepness of the output triangles.
    let ground = |x, y| ground.height_at(x, y).map(|sample| sample.height);
    let mut unreached = 0_u32;
    let mut steps = Vec::new();
    for (vertex, (&[x, y, _], &keep)) in vertices.iter().zip(&fixed).enumerate() {
        if keep || is_protected(masks, x, y) || !grading_allowed(masks, x, y) {
            continue;
        }
        let (Some(current), Some(owner)) = (heights.get_mut(vertex), owners.get_mut(vertex)) else {
            return Err(BuildError::Region(RegionError::Topology));
        };
        let (design, short) = faces.over(*current, x, y);
        unreached += u32::from(short);
        if short {
            outline::push_step_witness(
                &mut steps,
                outline::StepWitness::ReachLimit {
                    vertex: u32::try_from(vertex).unwrap_or(u32::MAX),
                    position: [x, y, design],
                },
            );
        }
        if design.to_bits() != current.to_bits() {
            *current = design;
            *owner = Some(planned.order);
        }
    }
    let allowed = |x, y| !is_protected(masks, x, y) && grading_allowed(masks, x, y);
    let clipped = outline::clipped(&faces, &ground, &allowed, vertices, &mesh.ring)
        .map_err(BuildError::Outline)?;
    steps.extend(clipped.witnesses);
    Ok((unreached, clipped.count, steps))
}

mod field_preview;
pub use field_preview::field_preview;

#[cfg(test)]
mod tests;
