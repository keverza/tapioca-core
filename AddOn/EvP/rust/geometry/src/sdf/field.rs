//! Immutable ordered-field compilation, evaluation and conservative capabilities.

use std::collections::BTreeSet;
use std::fmt;

use super::{Bounds, Primitive, primitive};
use crate::affine::Vec3;

/// Volumetric Boolean operation applied at an operation's stack position.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BooleanOperation {
    /// Add the operand to the accumulated field.
    Union,
    /// Remove the operand from the accumulated field.
    Subtract,
    /// Retain only the common interior.
    Intersect,
}

/// Distance blend profile. Zero radius is a hard Boolean for either profile.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendProfile {
    /// Compact cubic blend with a tighter crease.
    Sharp,
    /// Polynomial smooth minimum/maximum with a round transition.
    Round,
}

/// One authored primitive operation. Dimensions are metres and angles, where
/// present in a frame supplied by the caller, are radians before conversion.
#[derive(Clone, Debug, PartialEq)]
pub struct StackOperation {
    /// Durable operation ID within the detail.
    pub id: u64,
    /// Exact primitive evaluated by this operation.
    pub primitive: Primitive,
    /// Boolean applied at this position.
    pub operation: BooleanOperation,
    /// Sharp or Round distance blend.
    pub profile: BlendProfile,
    /// Nonnegative blend radius in metres; zero is a hard Boolean.
    pub blend_radius: f64,
    /// Positive material pool ID; materials never interpolate into colours.
    pub material_id: i32,
    /// Disabled operations are omitted without reordering the remainder.
    pub enabled: bool,
}

/// Explicit header for the operations that follow it. SDF-1 groups do not nest.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GroupHeader {
    /// Durable group ID within the detail.
    pub id: u64,
    /// A disabled header skips its complete group.
    pub enabled: bool,
    /// Evaluate locally and union once into the parent when true.
    pub isolated: bool,
    /// Profile used to union an isolated group into its parent.
    pub profile: BlendProfile,
    /// Nonnegative isolated-group union radius in metres.
    pub blend_radius: f64,
}

/// One ordered stack item.
#[derive(Clone, Debug, PartialEq)]
pub enum StackEntry {
    /// Primitive operation.
    Operation(StackOperation),
    /// Header owning subsequent operations until the next header.
    Group(GroupHeader),
}

/// Compilation controls. These affect evaluation only; they are not persisted
/// preview state and do not mutate the authored stack.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CompileOptions {
    /// Evaluate from the stack start through this operation, inclusive.
    pub solo_operation: Option<u64>,
    /// Caller-specific operation/header ceiling, capped by the global limit.
    pub max_operations: usize,
}

impl Default for CompileOptions {
    fn default() -> Self {
        Self {
            solo_operation: None,
            max_operations: 256,
        }
    }
}

/// One exact field value and its hard material owner.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FieldSample {
    /// Signed distance in metres; negative is inside.
    pub distance: f64,
    /// Positive material pool ID selected by the ordered operation rules.
    pub material_id: i32,
}

/// Inclusive conservative range of field values over a closed region.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ValueRange {
    /// Lowest possible distance.
    pub min: f64,
    /// Highest possible distance.
    pub max: f64,
}

impl ValueRange {
    /// Whether this range can contain the zero isosurface.
    #[must_use]
    pub fn may_contain_zero(self) -> bool {
        self.min <= 0.0 && self.max >= 0.0
    }

    /// Whether an exact value lies inside this conservative range.
    #[must_use]
    pub fn contains(self, value: f64) -> bool {
        self.min <= value && value <= self.max
    }
}

/// Prepared-root capability summary. Missing/unknown capabilities are reported
/// conservatively and never authorize optimistic pruning.
#[expect(
    clippy::struct_excessive_bools,
    reason = "this is a five-bit capability report, not coordinated object state"
)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Capabilities {
    /// Finite conservative bounds are available.
    pub bounds: bool,
    /// A finite Lipschitz interval bound is available.
    pub interval: bool,
    /// A finite Lipschitz upper bound is available.
    pub lipschitz: bool,
    /// Analytic gradients are available away from non-differentiable points.
    pub gradient: bool,
    /// Batch evaluation is implemented as exact scalar-order evaluation.
    pub batch: bool,
}

/// Typed field compilation/evaluation refusal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// No enabled positive field remains after filtering.
    EmptyStack,
    /// The operation/header ceiling is exceeded.
    OperationBudget,
    /// Durable item IDs must be nonzero and unique within one detail.
    InvalidId,
    /// A primitive or profile has non-finite/invalid parameters.
    InvalidPrimitive(u64),
    /// A group header has non-finite/invalid blend parameters.
    InvalidGroup(u64),
    /// The first enabled operation of the root or a group is not Union.
    FirstOperationNotUnion(u64),
    /// Solo names no enabled operation in an enabled group.
    SoloOperationUnavailable,
    /// A query point is NaN or infinite.
    NonFinitePoint,
    /// Distance arithmetic overflowed or otherwise became non-finite.
    NonFiniteEvaluation,
    /// A region has non-finite or reversed bounds.
    InvalidRegion,
    /// A finite-difference step is not finite and positive.
    InvalidStep,
}

impl Error {
    /// Stable allocation-free diagnostic prefix and description.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::EmptyStack => "EmptyStack: the filtered detail has no enabled operations",
            Self::OperationBudget => "OperationBudget: the detail exceeds its operation ceiling",
            Self::InvalidId => "InvalidId: operation and group IDs must be nonzero and unique",
            Self::InvalidPrimitive(_) => {
                "InvalidPrimitive: primitive dimensions, frame, profile and material must be valid"
            }
            Self::InvalidGroup(_) => {
                "InvalidGroup: group blend radius must be finite and nonnegative"
            }
            Self::FirstOperationNotUnion(_) => {
                "FirstOperationNotUnion: every root or group must start with Union"
            }
            Self::SoloOperationUnavailable => {
                "SoloOperationUnavailable: solo must name an enabled operation in an enabled group"
            }
            Self::NonFinitePoint => "NonFinitePoint: field query coordinates must be finite",
            Self::NonFiniteEvaluation => {
                "NonFiniteEvaluation: field arithmetic produced NaN or infinity"
            }
            Self::InvalidRegion => "InvalidRegion: interval bounds must be finite and ordered",
            Self::InvalidStep => "InvalidStep: gradient step must be finite and positive",
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message())
    }
}

#[derive(Clone, Debug, PartialEq)]
enum NodeKind {
    Primitive {
        primitive: Primitive,
        material_id: i32,
    },
    Combine {
        operation: BooleanOperation,
        profile: BlendProfile,
        radius: f64,
        left: usize,
        right: usize,
    },
}

#[derive(Clone, Debug, PartialEq)]
struct Node {
    kind: NodeKind,
    bounds: Option<Bounds>,
    lipschitz: f64,
    analytic_gradient: bool,
}

/// Immutable compiled field. Nodes are private implementation details; callers
/// use exact evaluation, capabilities, conservative bounds and intervals.
#[derive(Clone, Debug, PartialEq)]
pub struct PreparedField {
    nodes: Vec<Node>,
    root: usize,
    bounds: Option<Bounds>,
    lipschitz: f64,
    analytic_gradient: bool,
}

impl PreparedField {
    /// Validate and compile an ordered stack. The first enabled operation of the
    /// root and each nonempty enabled group must be Union. Empty disabled groups
    /// are skipped; invalid definitions and invalid solo requests fail closed.
    ///
    /// # Errors
    /// Returns a typed [`Error`] for invalid input, budget excess or an empty
    /// filtered stack. No partially compiled field is returned.
    pub fn compile(entries: &[StackEntry], options: CompileOptions) -> Result<Self, Error> {
        if entries.len() > 256 || entries.len() > options.max_operations {
            return Err(Error::OperationBudget);
        }
        validate_entries(entries)?;
        let end = solo_end(entries, options.solo_operation)?;
        let mut compiler = Compiler { nodes: Vec::new() };
        let mut root = None;
        let mut index = 0;
        while index < end {
            match entries.get(index) {
                Some(StackEntry::Operation(operation)) => {
                    root = compiler.apply_operation(root, operation)?;
                    index += 1;
                }
                Some(StackEntry::Group(header)) => {
                    let group_start = index + 1;
                    let mut group_end = group_start;
                    while group_end < end
                        && matches!(entries.get(group_end), Some(StackEntry::Operation(_)))
                    {
                        group_end += 1;
                    }
                    if header.enabled {
                        let group = entries
                            .get(group_start..group_end)
                            .ok_or(Error::InvalidGroup(header.id))?;
                        root = compiler.compile_group(root, header, group)?;
                    }
                    index = group_end;
                }
                None => return Err(Error::EmptyStack),
            }
        }
        let root = root.ok_or(Error::EmptyStack)?;
        let node = compiler.nodes.get(root).ok_or(Error::EmptyStack)?;
        Ok(Self {
            bounds: node.bounds,
            lipschitz: node.lipschitz,
            analytic_gradient: node.analytic_gradient,
            nodes: compiler.nodes,
            root,
        })
    }

    /// Exact distance and hard material ownership at one finite world-metre point.
    ///
    /// # Errors
    /// Refuses non-finite points or unrepresentable derived arithmetic.
    pub fn evaluate(&self, point: Vec3) -> Result<FieldSample, Error> {
        if !primitive::finite(point) {
            return Err(Error::NonFinitePoint);
        }
        self.evaluate_node(self.root, point)
    }

    /// Exact scalar-order batch evaluation. No partial batch is returned.
    ///
    /// # Errors
    /// Any point/evaluation error discards the complete batch.
    pub fn evaluate_batch(&self, points: &[Vec3]) -> Result<Vec<FieldSample>, Error> {
        points.iter().map(|&point| self.evaluate(point)).collect()
    }

    /// Conservative finite field bounds, or `None` when unavailable.
    #[must_use]
    pub const fn bounds(&self) -> Option<Bounds> {
        self.bounds
    }

    /// Conservative Lipschitz upper bound; infinity means unknown.
    #[must_use]
    pub const fn lipschitz(&self) -> f64 {
        self.lipschitz
    }

    /// Root capability summary.
    #[must_use]
    pub const fn capabilities(&self) -> Capabilities {
        Capabilities {
            bounds: self.bounds.is_some(),
            interval: self.lipschitz.is_finite(),
            lipschitz: self.lipschitz.is_finite(),
            gradient: self.analytic_gradient,
            batch: true,
        }
    }

    /// Conservative interval from the center value and Lipschitz bound. Unknown
    /// Lipschitz returns an unbounded range rather than an optimistic interval.
    ///
    /// # Errors
    /// Refuses an invalid region or non-finite center/arithmetic.
    pub fn interval(&self, region: Bounds) -> Result<ValueRange, Error> {
        if !region.is_valid() {
            return Err(Error::InvalidRegion);
        }
        let center = self.evaluate(region.center())?.distance;
        let reach = self.lipschitz * region.half_diagonal();
        if reach.is_nan() {
            return Err(Error::NonFiniteEvaluation);
        }
        let range = if reach.is_finite() {
            ValueRange {
                min: center - reach,
                max: center + reach,
            }
        } else {
            ValueRange {
                min: f64::NEG_INFINITY,
                max: f64::INFINITY,
            }
        };
        if range.min.is_nan() || range.max.is_nan() {
            return Err(Error::NonFiniteEvaluation);
        }
        Ok(range)
    }

    /// Return a region-pruned field. SDF-1 deliberately uses identity pruning:
    /// it is a valid bit-identical baseline for the sampler and a correctness
    /// oracle for later specialized pruning.
    ///
    /// # Errors
    /// Refuses an invalid region. The returned clone has exactly the original
    /// arithmetic and operation order within the region.
    pub fn pruned(&self, region: Bounds) -> Result<Self, Error> {
        if !region.is_valid() {
            return Err(Error::InvalidRegion);
        }
        Ok(self.clone())
    }

    /// Field gradient in world coordinates. Analytic gradients are used where
    /// implemented; non-differentiable points and smooth blends use the
    /// four-sample tetrahedral finite-difference stencil with `step` metres.
    ///
    /// # Errors
    /// Refuses invalid points/steps and any non-finite derived value.
    pub fn gradient(&self, point: Vec3, step: f64) -> Result<Vec3, Error> {
        if !primitive::finite(point) {
            return Err(Error::NonFinitePoint);
        }
        if !step.is_finite() || step <= 0.0 {
            return Err(Error::InvalidStep);
        }
        if let Some(gradient) = self.analytic_node_gradient(self.root, point)? {
            return Ok(gradient);
        }
        self.finite_difference_gradient(point, step)
    }

    fn evaluate_node(&self, index: usize, point: Vec3) -> Result<FieldSample, Error> {
        let node = self.nodes.get(index).ok_or(Error::EmptyStack)?;
        match &node.kind {
            NodeKind::Primitive {
                primitive,
                material_id,
            } => {
                let distance = primitive
                    .evaluate(point)
                    .map_err(|()| Error::NonFiniteEvaluation)?;
                Ok(FieldSample {
                    distance,
                    material_id: *material_id,
                })
            }
            NodeKind::Combine {
                operation,
                profile,
                radius,
                left,
                right,
            } => {
                let a = self.evaluate_node(*left, point)?;
                let b = self.evaluate_node(*right, point)?;
                let distance =
                    combine_distance(*operation, *profile, *radius, a.distance, b.distance);
                if !distance.is_finite() {
                    return Err(Error::NonFiniteEvaluation);
                }
                let selected = match operation {
                    BooleanOperation::Union if b.distance <= a.distance => b,
                    BooleanOperation::Subtract if -b.distance >= a.distance => b,
                    BooleanOperation::Intersect if b.distance >= a.distance => b,
                    _ => a,
                };
                Ok(FieldSample {
                    distance,
                    material_id: selected.material_id,
                })
            }
        }
    }

    #[expect(
        clippy::float_cmp,
        reason = "exact equality selects the finite-difference fallback at non-differentiable ties"
    )]
    fn analytic_node_gradient(&self, index: usize, point: Vec3) -> Result<Option<Vec3>, Error> {
        let node = self.nodes.get(index).ok_or(Error::EmptyStack)?;
        match &node.kind {
            NodeKind::Primitive { primitive, .. } => primitive
                .gradient(point)
                .map_err(|()| Error::NonFiniteEvaluation),
            NodeKind::Combine {
                operation,
                radius,
                left,
                right,
                ..
            } => {
                if *radius > 0.0 {
                    return Ok(None);
                }
                let a = self.evaluate_node(*left, point)?.distance;
                let b = self.evaluate_node(*right, point)?.distance;
                let choose_right = match operation {
                    BooleanOperation::Union => b < a,
                    BooleanOperation::Subtract => -b > a,
                    BooleanOperation::Intersect => b > a,
                };
                if a == b || (*operation == BooleanOperation::Subtract && a == -b) {
                    return Ok(None);
                }
                if choose_right {
                    let gradient = self.analytic_node_gradient(*right, point)?;
                    match operation {
                        BooleanOperation::Subtract => Ok(gradient.map(primitive::negate)),
                        _ => Ok(gradient),
                    }
                } else {
                    self.analytic_node_gradient(*left, point)
                }
            }
        }
    }

    fn finite_difference_gradient(&self, point: Vec3, step: f64) -> Result<Vec3, Error> {
        let offsets = [
            Vec3 {
                x: step,
                y: -step,
                z: -step,
            },
            Vec3 {
                x: -step,
                y: -step,
                z: step,
            },
            Vec3 {
                x: -step,
                y: step,
                z: -step,
            },
            Vec3 {
                x: step,
                y: step,
                z: step,
            },
        ];
        let mut values = [0.0; 4];
        for (target, offset) in values.iter_mut().zip(offsets) {
            let sample = primitive::add(point, offset);
            *target = self.evaluate(sample)?.distance;
        }
        let [a, b, c, d] = values;
        let divisor = 4.0 * step;
        let gradient = Vec3 {
            x: (a - b - c + d) / divisor,
            y: (-a - b + c + d) / divisor,
            z: (-a + b - c + d) / divisor,
        };
        if primitive::finite(gradient) {
            Ok(gradient)
        } else {
            Err(Error::NonFiniteEvaluation)
        }
    }
}

struct Compiler {
    nodes: Vec<Node>,
}

#[derive(Clone, Copy)]
struct CombineRule {
    operation: BooleanOperation,
    profile: BlendProfile,
    radius: f64,
}

impl Compiler {
    fn compile_group(
        &mut self,
        mut root: Option<usize>,
        header: &GroupHeader,
        entries: &[StackEntry],
    ) -> Result<Option<usize>, Error> {
        for entry in entries {
            let StackEntry::Operation(operation) = entry else {
                return Err(Error::InvalidGroup(header.id));
            };
            if operation.enabled {
                if operation.operation != BooleanOperation::Union {
                    return Err(Error::FirstOperationNotUnion(operation.id));
                }
                break;
            }
        }
        if !header.isolated {
            for entry in entries {
                let StackEntry::Operation(operation) = entry else {
                    return Err(Error::InvalidGroup(header.id));
                };
                root = self.apply_operation(root, operation)?;
            }
            return Ok(root);
        }

        let mut local = None;
        for entry in entries {
            let StackEntry::Operation(operation) = entry else {
                return Err(Error::InvalidGroup(header.id));
            };
            local = self.apply_operation(local, operation)?;
        }
        if let Some(group) = local {
            root = Some(match root {
                Some(parent) => self.combine(
                    parent,
                    group,
                    CombineRule {
                        operation: BooleanOperation::Union,
                        profile: header.profile,
                        radius: header.blend_radius,
                    },
                ),
                None => group,
            });
        }
        Ok(root)
    }

    fn apply_operation(
        &mut self,
        accumulated: Option<usize>,
        operation: &StackOperation,
    ) -> Result<Option<usize>, Error> {
        if !operation.enabled {
            return Ok(accumulated);
        }
        if accumulated.is_none() && operation.operation != BooleanOperation::Union {
            return Err(Error::FirstOperationNotUnion(operation.id));
        }
        let primitive = self.push_primitive(operation);
        Ok(Some(match accumulated {
            Some(left) => self.combine(
                left,
                primitive,
                CombineRule {
                    operation: operation.operation,
                    profile: operation.profile,
                    radius: operation.blend_radius,
                },
            ),
            None => primitive,
        }))
    }

    fn push_primitive(&mut self, operation: &StackOperation) -> usize {
        let index = self.nodes.len();
        self.nodes.push(Node {
            bounds: operation.primitive.bounds(),
            kind: NodeKind::Primitive {
                primitive: operation.primitive.clone(),
                material_id: operation.material_id,
            },
            lipschitz: 1.0,
            analytic_gradient: true,
        });
        index
    }

    fn combine(&mut self, left: usize, right: usize, rule: CombineRule) -> usize {
        let index = self.nodes.len();
        let (bounds, left_lipschitz, right_lipschitz, left_gradient, right_gradient) =
            match (self.nodes.get(left), self.nodes.get(right)) {
                (Some(left), Some(right)) => (
                    combine_bounds(rule.operation, rule.radius, left.bounds, right.bounds),
                    left.lipschitz,
                    right.lipschitz,
                    left.analytic_gradient,
                    right.analytic_gradient,
                ),
                _ => (None, f64::INFINITY, f64::INFINITY, false, false),
            };
        self.nodes.push(Node {
            kind: NodeKind::Combine {
                operation: rule.operation,
                profile: rule.profile,
                radius: rule.radius,
                left,
                right,
            },
            bounds,
            lipschitz: left_lipschitz.max(right_lipschitz),
            analytic_gradient: left_gradient && right_gradient && rule.radius == 0.0,
        });
        index
    }
}

fn validate_entries(entries: &[StackEntry]) -> Result<(), Error> {
    let mut ids = BTreeSet::new();
    for entry in entries {
        let id = match entry {
            StackEntry::Operation(operation) => {
                if !operation.blend_radius.is_finite()
                    || operation.blend_radius < 0.0
                    || operation.material_id <= 0
                    || operation.primitive.validate().is_err()
                {
                    return Err(Error::InvalidPrimitive(operation.id));
                }
                operation.id
            }
            StackEntry::Group(group) => {
                if !group.blend_radius.is_finite() || group.blend_radius < 0.0 {
                    return Err(Error::InvalidGroup(group.id));
                }
                group.id
            }
        };
        if id == 0 || !ids.insert(id) {
            return Err(Error::InvalidId);
        }
    }
    Ok(())
}

fn solo_end(entries: &[StackEntry], solo: Option<u64>) -> Result<usize, Error> {
    let Some(solo) = solo else {
        return Ok(entries.len());
    };
    let Some(position) = entries.iter().position(
        |entry| matches!(entry, StackEntry::Operation(operation) if operation.id == solo),
    ) else {
        return Err(Error::SoloOperationUnavailable);
    };
    let prefix = entries
        .get(..=position)
        .ok_or(Error::SoloOperationUnavailable)?;
    let mut group_enabled = true;
    for entry in prefix {
        match entry {
            StackEntry::Group(group) => group_enabled = group.enabled,
            StackEntry::Operation(operation)
                if operation.id == solo && (!operation.enabled || !group_enabled) =>
            {
                return Err(Error::SoloOperationUnavailable);
            }
            StackEntry::Operation(_) => {}
        }
    }
    Ok(position + 1)
}

fn combine_bounds(
    operation: BooleanOperation,
    radius: f64,
    left: Option<Bounds>,
    right: Option<Bounds>,
) -> Option<Bounds> {
    match operation {
        BooleanOperation::Union => match (left, right) {
            (Some(left), Some(right)) => left.union(right).inflated(radius),
            _ => None,
        },
        BooleanOperation::Subtract | BooleanOperation::Intersect => left,
    }
}

fn combine_distance(
    operation: BooleanOperation,
    profile: BlendProfile,
    radius: f64,
    a: f64,
    b: f64,
) -> f64 {
    if radius == 0.0 {
        return match operation {
            BooleanOperation::Union => a.min(b),
            BooleanOperation::Subtract => a.max(-b),
            BooleanOperation::Intersect => a.max(b),
        };
    }
    match profile {
        BlendProfile::Sharp => sharp_distance(operation, radius, a, b),
        BlendProfile::Round => round_distance(operation, radius, a, b),
    }
}

fn sharp_distance(operation: BooleanOperation, radius: f64, a: f64, b: f64) -> f64 {
    let h = match operation {
        BooleanOperation::Subtract => (radius - (a + b).abs()).max(0.0) / radius,
        _ => (radius - (a - b).abs()).max(0.0) / radius,
    };
    let correction = radius * h * h * h / 6.0;
    match operation {
        BooleanOperation::Union => a.min(b) - correction,
        BooleanOperation::Subtract => a.max(-b) + correction,
        BooleanOperation::Intersect => a.max(b) + correction,
    }
}

fn round_distance(operation: BooleanOperation, radius: f64, a: f64, b: f64) -> f64 {
    match operation {
        BooleanOperation::Union => {
            let h = (0.5 + 0.5 * (b - a) / radius).clamp(0.0, 1.0);
            mix(b, a, h) - radius * h * (1.0 - h)
        }
        BooleanOperation::Subtract => {
            let h = (0.5 + f64::midpoint(a, b) / radius).clamp(0.0, 1.0);
            mix(-b, a, h) + radius * h * (1.0 - h)
        }
        BooleanOperation::Intersect => {
            let h = (0.5 + 0.5 * (b - a) / radius).clamp(0.0, 1.0);
            mix(a, b, h) + radius * h * (1.0 - h)
        }
    }
}

fn mix(a: f64, b: f64, amount: f64) -> f64 {
    a * (1.0 - amount) + b * amount
}
