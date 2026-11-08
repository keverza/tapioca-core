//! Outline grading: the terrain under and around a paved area, built from the
//! area itself rather than from one path's centreline band.
//!
//! **Surface.** Inside the paved area the design height is the paved surface's
//! (a caller TIN over the area: every path, junction and kerb fillet as paved).
//!
//! **Faces.** Outside it, every point `q` of the area's outline (the TIN's
//! boundary edges) at height `h` offers a fill cone `h - fill * d` and a cut
//! cone `h + cut * d` over the plan distance `d`. The fill face is the highest
//! fill cone and the cut face the lowest cut cone (each edge's steepest point
//! is closed form), so the faces are continuous where two edges' reaches meet,
//! as in an inside corner, and never steeper than their grade. The ground is
//! raised to the fill face and then lowered to the cut face. A cone's first
//! `rounding` metres are the same cubic top as the band's ([`super::design`]);
//! each face's toe rounds onto the ground by a smooth maximum (minimum) over
//! `grade * rounding` of vertical gap, never steeper than the face. A result within the tolerance of the ground keeps the ground;
//! a cone past `limit` is ignored.
//!
//! **Clipping.** A face that would still differ from the ground on the region
//! ring ends there in a step: [`Outlined::clipped`] counts the ring samples
//! where it does.
//!
//! **Mesh.** The region is rebuilt as in [`super::region`] with the caller's plan
//! breaklines (the outline, and any lines the caller wants kept) and isolated
//! points (the TIN's interior vertices). With a [`Refine`] bound the faces then
//! get their vertices from the error they leave, not from offset rings: every
//! inside triangle is sampled, and where the faces there are further than the
//! bound from the triangle's plane its longest side is bisected, round after
//! round, until none is ([`Refined`]). A base edge between coplanar triangles
//! is not kept: it carries nothing and only cuts the faces into slivers. Private constraints
//! preserve every affine piece of the paved TIN, clipped at a shared subdivided
//! region stitch. Every vertex of an
//! outside triangle keeps its base height bit for bit; every other vertex takes
//! the design height. Volumes are the exact triangle sums against the base.
//!
//! Plan positions are world metres in `f64`; heights in metres.

use super::design::{toe, top};
use super::edit::Effect;
use super::field::Field;
use super::plan;
use super::region::{RegionError, RegionMesh, RegionRequest, retriangulate_with_supports};
use super::volume::VolumeError;
use super::volume::exact::{ExactReport, ExactRequest, exact_cut_fill};
use crate::slope::Slope;
use std::collections::BTreeMap;

mod blend;
mod crease;
mod mesher;
mod reach;
mod refine;
mod simplify;
pub(super) mod supports;
pub(super) use simplify::PREVIEW_MERGE;
pub(in crate::terrain) mod toe;
mod trace;

use refine::Graded;
pub use refine::{Refine, Refined};

/// How the faces around the area are graded.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Grading {
    /// Face grade where the outline is below the ground.
    pub cut: Slope,
    /// Face grade where the outline is above the ground.
    pub fill: Slope,
    /// Plan reach of a face from the outline, metres.
    pub limit: f64,
    /// Plan length each face rounds over at its top and toe, metres; 0 sharp.
    pub rounding: f64,
    /// Which way the design may move the ground.
    pub effect: Effect,
    /// At-grade height band, metres.
    pub tolerance: f64,
}

/// Borrowed outline request.
#[derive(Clone, Copy, Debug)]
pub struct OutlineRequest<'a> {
    /// Flat xyz base mesh vertices, world metres.
    pub points: &'a [f64],
    /// Counter-clockwise base vertex triples.
    pub triangles: &'a [u32],
    /// Implicitly closed flat xyz region ring (z ignored).
    pub region: &'a [f64],
    /// Flat xyz paved surface vertices.
    pub surface_points: &'a [f64],
    /// Counter-clockwise paved surface triples.
    pub surface_triangles: &'a [u32],
    /// Open plan polylines (z ignored), every vertex strictly inside the region.
    pub breaklines: &'a [&'a [f64]],
    /// Isolated plan points (z ignored), strictly inside the region.
    pub spots: &'a [f64],
    /// The faces.
    pub grading: Grading,
    /// Per paved surface vertex, the faces leaving it (its path's section
    /// there); empty grades every face by `grading`. An outline edge takes the
    /// mean of its two vertices'.
    pub surface_grades: &'a [SurfaceGrade],
    /// Error-driven refinement; `None` keeps the caller's vertices only.
    pub refine: Option<Refine>,
}

/// The faces leaving one paved vertex, as rise per metre of run.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SurfaceGrade {
    /// Cut face gradient, non-negative; infinite for a vertical face (none).
    pub cut: f64,
    /// Fill face gradient, non-negative; infinite for a vertical face (none).
    pub fill: f64,
    /// Plan length the face rounds over at its top and toe, metres.
    pub rounding: f64,
}

impl SurfaceGrade {
    fn valid(self) -> bool {
        [self.cut, self.fill].iter().all(|g| *g >= 0.0)
            && self.rounding.is_finite()
            && self.rounding >= 0.0
    }

    /// The mean of two vertices' faces; a vertical one stays vertical.
    fn mean(self, other: Self) -> Self {
        let mid = |a: f64, b: f64| {
            if a.is_finite() && b.is_finite() {
                a.midpoint(b)
            } else {
                f64::INFINITY
            }
        };
        Self {
            cut: mid(self.cut, other.cut),
            fill: mid(self.fill, other.fill),
            rounding: self.rounding.midpoint(other.rounding),
        }
    }
}

/// The graded mesh.
#[derive(Clone, Debug, PartialEq)]
pub struct Outlined {
    /// Flat xyz proposed vertices; the plan positions are `mesh.points`'.
    pub points: Vec<f64>,
    /// The rebuilt base mesh. Derived support breaklines append after the caller
    /// breaklines and the `toe` lines (#149 T2), mapped to source TIN edges by
    /// `surface_supports`; the
    /// refinement's crossing lines append after those and have no source. The split's
    /// line origins refer to `stitch` (before the base split adds its crossings).
    pub mesh: RegionMesh,
    /// Flat xyz region ring subdivided at private support intersections; z=0.
    pub stitch: Vec<f64>,
    /// Per appended private breakline, its original surface vertex pair (sorted).
    /// Several clipped pieces may carry the same source edge.
    pub surface_supports: Vec<[u32; 2]>,
    /// Per vertex, whether the design changed its height (bitwise).
    pub owners: Vec<bool>,
    /// Exact cut/fill: site inside the region, context outside.
    pub volumes: ExactReport,
    /// Vertices past the limit whose face had not yet met the ground: the base
    /// is kept there, so the faces end in a step. Zero when every face meets it.
    pub unreached: u32,
    /// Region ring samples (every vertex, and every [`CLIP_STEP`] metres along
    /// it) where a face had not met the ground: the region's edge, not the
    /// limit, ends it in a step. Zero when every face fits inside the region.
    pub clipped: u32,
    /// The refinement's report; default without one.
    pub refined: Refined,
    /// Toe breaklines in the mesh (#149 T2), after the caller lines; zero when
    /// there is no toe or the triangulation could not take it.
    pub toe: u32,
}

/// Plan spacing of the region ring samples checked for a clipped face, metres.
pub const CLIP_STEP: f64 = 0.25;

/// Outline grading refusals.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum OutlineError {
    /// A non-finite or negative limit, rounding or tolerance, or a bad slope.
    Parameters,
    /// The base mesh is not a field.
    Field,
    /// The paved surface is not a field with a boundary.
    Surface,
    /// A rebuilt vertex is off the base surface.
    Coverage,
    /// The region rebuild refused.
    Region(RegionError),
    /// The exact volume refused.
    Volume(VolumeError),
}

impl OutlineError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Parameters => {
                "OutlineParameters: limit, rounding and tolerance must be finite and non-negative, slopes valid"
            }
            Self::Field => "OutlineField: the base mesh has no upward face or is malformed",
            Self::Surface => "OutlineSurface: the paved surface is not a field with an outline",
            Self::Coverage => "OutlineCoverage: a rebuilt vertex is off the base surface",
            Self::Region(error) => error.message(),
            Self::Volume(error) => error.message(),
        }
    }
}

/// One outline edge and the faces leaving it.
#[derive(Clone, Copy, Debug)]
pub(super) struct Edge {
    segment: [[f64; 3]; 2],
    grade: SurfaceGrade,
}

/// The paved area's outline: its TIN's boundary edges, as xyz segments, each
/// with its vertices' mean faces (`uniform` where `grades` is empty).
fn outline(
    points: &[f64],
    triangles: &[u32],
    grades: &[SurfaceGrade],
    uniform: SurfaceGrade,
) -> Vec<Edge> {
    let vertices = plan::vertices(points);
    let mut edges = BTreeMap::<(u32, u32), u32>::new();
    for &[a, b, c] in triangles.as_chunks::<3>().0 {
        for (from, to) in [(a, b), (b, c), (c, a)] {
            *edges.entry((from.min(to), from.max(to))).or_default() += 1;
        }
    }
    let grade = |v: u32| grades.get(v as usize).copied().unwrap_or(uniform);
    edges
        .into_iter()
        .filter(|&(_, count)| count == 1)
        .filter_map(|((a, b), _)| {
            Some(Edge {
                segment: [*vertices.get(a as usize)?, *vertices.get(b as usize)?],
                grade: grade(a).mean(grade(b)),
            })
        })
        .collect()
}

/// The point of the edge `from`-`to` whose cone at `grade` reaches highest
/// over `(px, py)` (lowest when `cut`): its plan distance and height. Along the
/// edge the height is affine and the distance convex, so the cone height is
/// concave and its extremum closed form.
pub(super) fn steepest(
    segment: [[f64; 3]; 2],
    point: [f64; 2],
    grade: f64,
    cut: bool,
) -> (f64, f64) {
    let (distance, height, _, _) = steepest_point(segment, point, grade, cut);
    (distance, height)
}

/// [`steepest`], with the plan unit vector from that point to `(px, py)` (the
/// way its cone falls; zero on the edge) and whether the point is inside the
/// edge (its face is planar there) rather than at an end (a round cone).
fn steepest_point(
    [from, to]: [[f64; 3]; 2],
    [px, py]: [f64; 2],
    grade: f64,
    cut: bool,
) -> (f64, f64, [f64; 2], bool) {
    let (dx, dy) = (to[0] - from[0], to[1] - from[1]);
    let length = dx.hypot(dy);
    let at = |along: f64| {
        let t = if length > 0.0 { along / length } else { 0.0 };
        let (x, y) = (dx.mul_add(t, from[0]), dy.mul_add(t, from[1]));
        let distance = (x - px).hypot(y - py);
        let toward = if distance > 0.0 {
            [(px - x) / distance, (py - y) / distance]
        } else {
            [0.0, 0.0]
        };
        let inside = along > 0.0 && along < length;
        (
            distance,
            (to[2] - from[2]).mul_add(t, from[2]),
            toward,
            inside,
        )
    };
    if length <= 0.0 {
        return at(0.0);
    }
    let (ux, uy) = (dx / length, dy / length);
    let (rx, ry) = (px - from[0], py - from[1]);
    let foot = rx.mul_add(ux, ry * uy);
    let off = rx.mul_add(uy, -ry * ux).abs();
    // A fill cone rises along the edge's own rise; a cut cone along its fall.
    let rise = (to[2] - from[2]) / length * if cut { -1.0 } else { 1.0 };
    let along = if rise >= grade {
        length
    } else if rise <= -grade {
        0.0
    } else {
        let ratio = rise / grade;
        off.mul_add(ratio / ratio.mul_add(-ratio, 1.0).sqrt(), foot)
    };
    at(along.clamp(0.0, length))
}

/// The edge whose face moves the ground at a point, whether it cuts, and the plan unit
/// vector it falls along there.
#[derive(Clone, Copy, Debug)]
pub(super) struct Winner {
    pub cut: bool,
    pub index: u32,
    pub toward: [f64; 2],
}

/// The pavement's plan footprint: inside an odd number of its outline's rings. Every paved
/// triangle counts, a kerb's near-vertical face too (the surface field keeps only upward
/// ones). Edges are bucketed by plan rows so a point asks only its row's edges (#149 T3).
pub(super) struct Footprint {
    origin: f64,
    row: f64,
    rows: Vec<Vec<[[f64; 2]; 2]>>,
}

impl Footprint {
    fn new(edges: &[Edge]) -> Self {
        let (mut lo, mut hi) = (f64::INFINITY, f64::NEG_INFINITY);
        for edge in edges {
            for p in edge.segment {
                (lo, hi) = (lo.min(p[1]), hi.max(p[1]));
            }
        }
        let mut footprint = Self {
            origin: lo,
            row: 1.0,
            rows: Vec::new(),
        };
        if lo
            .partial_cmp(&hi)
            .is_none_or(|order| order == std::cmp::Ordering::Greater)
        {
            return footprint;
        }
        footprint.row = ((hi - lo) / 4096.0).max(0.5);
        let count = plan::clamp_index(((hi - lo) / footprint.row).floor(), 4096) as usize + 1;
        footprint.rows = vec![Vec::new(); count];
        for edge in edges {
            let [a, b] = edge.segment;
            let (r0, r1) = (
                footprint.index(a[1].min(b[1])),
                footprint.index(a[1].max(b[1])),
            );
            for row in r0..=r1 {
                if let Some(bucket) = footprint.rows.get_mut(row) {
                    bucket.push([[a[0], a[1]], [b[0], b[1]]]);
                }
            }
        }
        footprint
    }

    fn index(&self, y: f64) -> usize {
        plan::clamp_index(
            ((y - self.origin) / self.row).floor().max(0.0),
            u32::try_from(self.rows.len().saturating_sub(1)).unwrap_or(u32::MAX),
        ) as usize
    }

    /// Whether `(x, y)` lies inside the footprint (even-odd over its outline edges).
    pub(super) fn contains(&self, x: f64, y: f64) -> bool {
        let Some(bucket) = self.rows.get(self.index(y)) else {
            return false;
        };
        let mut inside = false;
        for &[a, b] in bucket {
            if (a[1] > y) != (b[1] > y) {
                let t = (y - a[1]) / (b[1] - a[1]);
                if (b[0] - a[0]).mul_add(t, a[0]) > x {
                    inside = !inside;
                }
            }
        }
        inside
    }
}

/// Where the faces can move the ground: plan cells within the limit of an outline edge
/// (#149 T3). A face reaches no further than the limit from the edge it falls from, so a
/// triangle touching no marked cell keeps the ground (the pavement inside is its own TIN, whose
/// vertices and edges the rebuild already carries). The mesher asks this before evaluating
/// anything: the unified build meshes the whole property.
pub(super) struct Near {
    origin: [f64; 2],
    cell: f64,
    columns: usize,
    rows: usize,
    marked: Vec<bool>,
}

impl Near {
    /// Cells at least a metre and a half the limit wide, at most 1024 a side.
    fn new(edges: &[Edge], limit: f64) -> Self {
        let reach = limit.max(0.0);
        let (mut lo, mut hi) = ([f64::INFINITY; 2], [f64::NEG_INFINITY; 2]);
        for edge in edges {
            for p in edge.segment {
                lo = [lo[0].min(p[0] - reach), lo[1].min(p[1] - reach)];
                hi = [hi[0].max(p[0] + reach), hi[1].max(p[1] + reach)];
            }
        }
        let mut near = Self {
            origin: lo,
            cell: 1.0,
            columns: 0,
            rows: 0,
            marked: Vec::new(),
        };
        if !(lo[0] <= hi[0] && lo[1] <= hi[1]) {
            return near;
        }
        let span = (hi[0] - lo[0]).max(hi[1] - lo[1]);
        near.cell = (reach / 2.0).max(1.0).max(span / 1024.0);
        let count =
            |extent: f64| plan::clamp_index((extent / near.cell).floor(), 1024) as usize + 1;
        (near.columns, near.rows) = (count(hi[0] - lo[0]), count(hi[1] - lo[1]));
        near.marked = vec![false; near.columns * near.rows];
        for edge in edges {
            let [a, b] = edge.segment;
            let (from, to) = (
                [a[0].min(b[0]) - reach, a[1].min(b[1]) - reach],
                [a[0].max(b[0]) + reach, a[1].max(b[1]) + reach],
            );
            let ([c0, r0], [c1, r1]) = (near.index(from), near.index(to));
            for row in r0..=r1 {
                for column in c0..=c1 {
                    if let Some(flag) = near.marked.get_mut(row * near.columns + column) {
                        *flag = true;
                    }
                }
            }
        }
        near
    }

    /// The cell holding `p`, clamped into the grid.
    fn index(&self, p: [f64; 2]) -> [usize; 2] {
        let at = |value: f64, origin: f64, count: usize| {
            plan::clamp_index(
                ((value - origin) / self.cell).floor().max(0.0),
                u32::try_from(count.saturating_sub(1)).unwrap_or(u32::MAX),
            ) as usize
        };
        [
            at(p[0], self.origin[0], self.columns),
            at(p[1], self.origin[1], self.rows),
        ]
    }

    /// Whether the box `lo`-`hi` touches a marked cell.
    pub(super) fn touches(&self, lo: [f64; 2], hi: [f64; 2]) -> bool {
        if self.marked.is_empty() {
            return false;
        }
        let count = |n: usize| f64::from(u32::try_from(n).unwrap_or(u32::MAX));
        let far = [
            self.cell.mul_add(count(self.columns), self.origin[0]),
            self.cell.mul_add(count(self.rows), self.origin[1]),
        ];
        if hi[0] < self.origin[0] || hi[1] < self.origin[1] || lo[0] > far[0] || lo[1] > far[1] {
            return false;
        }
        let ([c0, r0], [c1, r1]) = (self.index(lo), self.index(hi));
        (r0..=r1).any(|row| {
            (c0..=c1).any(|column| {
                self.marked
                    .get(row * self.columns + column)
                    .copied()
                    .unwrap_or(false)
            })
        })
    }
}

/// A flat xyz point pool and its vertex triples.
type Flat<'a> = (&'a [f64], &'a [u32]);

/// The validated grading: grades as rise per metre of run.
pub(super) struct Faces<'a> {
    base: &'a Field,
    surface: &'a Field,
    outline: &'a [Edge],
    grading: Grading,
    reach: reach::Reach,
}

/// How far the faces raise (fill) and lower (cut) the ground at a point, each
/// eased onto it: within the limit, and without it.
#[derive(Clone, Copy)]
struct Moves {
    raise: f64,
    lower: f64,
    raise_any: f64,
    lower_any: f64,
}

/// One cone's height `distance` out from an outline point at `edge`.
fn cone(edge: f64, distance: f64, rise: f64, rounding: f64) -> f64 {
    if rounding > 0.0 && distance < rounding {
        top(edge, 0.0, rise, rounding, distance)
    } else {
        rise.mul_add(distance, edge)
    }
}

/// A toe rounds over the face's vertical span at most. The usual `grade * rounding` span starts
/// easing before the daylight; where the whole edge is closer to the ground than that, an
/// uncapped span lifts the design above (or below) the pavement at the outline itself (#170).
fn toe_span(base: f64, edge: f64, grade: f64, rounding: f64) -> f64 {
    (grade * rounding).min((edge - base).abs())
}

/// The local face envelope eased onto the ground. The toe span is a smooth height-weighted mean,
/// not just the winner's: tied faces with different edge heights meet continuously (#170). Weights
/// reach zero at `fade`, the reach index's knee, so a pruned face cannot change the span.
fn envelope_toe(
    base: f64,
    faces: &[blend::Face],
    keep: &dyn Fn(&blend::Face) -> bool,
    cut: bool,
    fade: f64,
) -> Option<f64> {
    let (winner, lifted) = blend::envelope(faces, keep)?;
    let mut weighted = 0.0;
    let mut weights = 0.0;
    for face in faces.iter().filter(|face| keep(face)) {
        let t = if fade > 0.0 {
            (1.0 - (winner.height - face.height).max(0.0) / fade).clamp(0.0, 1.0)
        } else if face.index == winner.index {
            1.0
        } else {
            0.0
        };
        let weight = t * t * 2.0f64.mul_add(-t, 3.0);
        weighted += weight * toe_span(base, face.edge, face.grade, face.rounding);
        weights += weight;
    }
    let span = if weights > 0.0 {
        weighted / weights
    } else {
        toe_span(base, winner.edge, winner.grade, winner.rounding)
    };
    Some(toe(base, if cut { -lifted } else { lifted }, cut, span))
}

impl<'a> Faces<'a> {
    fn new(base: &'a Field, surface: &'a Field, outline: &'a [Edge], grading: Grading) -> Self {
        Self {
            base,
            surface,
            outline,
            grading,
            reach: reach::Reach::new(outline),
        }
    }

    /// Each edge's face eased onto the ground by its own rounding; the fill
    /// takes the highest, the cut the lowest. For one grade everywhere this is
    /// the ease of the envelope (the ease is monotone in the face). Where two
    /// faces meet at a crease the winner is lifted towards its rival
    /// ([`blend`]) and eased by its own rounding; the move is never less than
    /// without the lift, and is the same bit for bit away from creases.
    fn moves(&self, base: f64, px: f64, py: f64) -> Moves {
        self.resolve(base, px, py).0
    }

    /// [`Self::moves`], with every fill and cut face reaching the point.
    fn resolve(&self, base: f64, px: f64, py: f64) -> (Moves, Vec<blend::Face>, Vec<blend::Face>) {
        let mut moves = Moves {
            raise: 0.0,
            lower: 0.0,
            raise_any: 0.0,
            lower_any: 0.0,
        };
        let limit = self.grading.limit;
        let (mut fills, mut cuts) = (Vec::with_capacity(64), Vec::with_capacity(64));
        self.reach
            .search(px, py, base, |index, [fill_side, cut_side]| {
                let mut reached = [None, None];
                let Some(edge) = self.outline.get(index as usize) else {
                    return reached;
                };
                let SurfaceGrade {
                    cut,
                    fill,
                    rounding,
                } = edge.grade;
                if fill_side && fill.is_finite() {
                    let (distance, height, toward, planar) =
                        steepest_point(edge.segment, [px, py], fill, false);
                    let face = cone(height, distance, -fill, rounding);
                    let raise =
                        toe(base, face, false, toe_span(base, height, fill, rounding)) - base;
                    moves.raise_any = moves.raise_any.max(raise);
                    if distance <= limit {
                        moves.raise = moves.raise.max(raise);
                        reached[0] = Some(face);
                    }
                    fills.push(blend::Face {
                        index,
                        height: face,
                        edge: height,
                        toward,
                        segment: edge.segment,
                        inside: planar,
                        foot: [
                            toward[0].mul_add(-distance, px),
                            toward[1].mul_add(-distance, py),
                        ],
                        distance,
                        grade: fill,
                        rounding,
                    });
                }
                if cut_side && cut.is_finite() {
                    let (distance, height, toward, planar) =
                        steepest_point(edge.segment, [px, py], cut, true);
                    let face = cone(height, distance, cut, rounding);
                    let lower = base - toe(base, face, true, toe_span(base, height, cut, rounding));
                    moves.lower_any = moves.lower_any.max(lower);
                    if distance <= limit {
                        moves.lower = moves.lower.max(lower);
                        reached[1] = Some(-face);
                    }
                    cuts.push(blend::Face {
                        index,
                        height: -face,
                        edge: height,
                        toward,
                        segment: edge.segment,
                        inside: planar,
                        foot: [
                            toward[0].mul_add(-distance, px),
                            toward[1].mul_add(-distance, py),
                        ],
                        distance,
                        grade: cut,
                        rounding,
                    });
                }
                reached
            });
        let within = |face: &blend::Face| face.distance <= limit;
        let raised = |keep: &dyn Fn(&blend::Face) -> bool| {
            envelope_toe(base, &fills, keep, false, self.reach.knee())
                .map_or(0.0, |height| height - base)
        };
        let lowered = |keep: &dyn Fn(&blend::Face) -> bool| {
            envelope_toe(base, &cuts, keep, true, self.reach.knee())
                .map_or(0.0, |height| base - height)
        };
        // Every face within the limit: the two envelopes are one.
        let (raise, lower) = (raised(&within), lowered(&within));
        let everywhere = |faces: &[blend::Face]| faces.iter().all(within);
        moves.raise = moves.raise.max(raise);
        moves.raise_any = moves.raise_any.max(if everywhere(&fills) {
            raise
        } else {
            raised(&|_| true)
        });
        moves.lower = moves.lower.max(lower);
        moves.lower_any = moves.lower_any.max(if everywhere(&cuts) {
            lower
        } else {
            lowered(&|_| true)
        });
        (moves, fills, cuts)
    }

    /// Which edge's face moves the ground at `(px, py)` over `base`, and on which side;
    /// `None` on the pavement and where the design keeps the ground (#149 T2 creases).
    pub(super) fn winner(&self, base: f64, px: f64, py: f64) -> Option<Winner> {
        if self.surface.height_at(px, py).is_some() {
            return None;
        }
        let (moves, fills, cuts) = self.resolve(base, px, py);
        let design = self.graded(base, moves.raise, moves.lower);
        if design.to_bits() == base.to_bits() {
            return None;
        }
        let cut = design < base;
        let limit = self.grading.limit;
        let (face, _) = blend::envelope(if cut { &cuts } else { &fills }, |face| {
            face.distance <= limit
        })?;
        Some(Winner {
            cut,
            index: face.index,
            toward: face.toward,
        })
    }

    /// Edge `a`'s face height less edge `b`'s on one side at `(px, py)` (a cut's negated, so
    /// positive where `a` wins) and the knee their blend rounds over there; `None` unless one
    /// of the two moves the ground there on that side (as [`Self::winner`] finds it) and both
    /// reach it. One resolve of the faces.
    pub(super) fn tie(
        &self,
        base: f64,
        [px, py]: [f64; 2],
        cut: bool,
        [a, b]: [u32; 2],
    ) -> Option<(f64, f64)> {
        if self.surface.height_at(px, py).is_some() {
            return None;
        }
        let (moves, fills, cuts) = self.resolve(base, px, py);
        let design = self.graded(base, moves.raise, moves.lower);
        if design.to_bits() == base.to_bits() || (design < base) != cut {
            return None;
        }
        let faces = if cut { &cuts } else { &fills };
        let limit = self.grading.limit;
        let (won, _) = blend::envelope(faces, |face| face.distance <= limit)?;
        if won.index != a && won.index != b {
            return None;
        }
        let find = |index: u32| faces.iter().find(|face| face.index == index);
        let (first, second) = (find(a)?, find(b)?);
        Some((first.height - second.height, blend::knee(first, second)))
    }

    /// The largest rounding of any outline edge, metres: how far a rounded crease spreads.
    pub(super) fn widest_rounding(&self) -> f64 {
        self.outline
            .iter()
            .map(|edge| edge.grade.rounding)
            .fold(0.0, f64::max)
    }

    /// Whether edge `a`'s and edge `b`'s faces on one side meet in a crease at `(px, py)`:
    /// both reach it, neither falls from a vertex the other's edge ends at (a round cone meets
    /// the plane smoothly there), and they fall at least [`blend::FLAT`] apart.
    pub(super) fn creased(
        &self,
        base: f64,
        [px, py]: [f64; 2],
        cut: bool,
        [a, b]: [u32; 2],
    ) -> bool {
        let (_, fills, cuts) = self.resolve(base, px, py);
        let faces = if cut { &cuts } else { &fills };
        let find = |index: u32| faces.iter().find(|face| face.index == index);
        let (Some(first), Some(second)) = (find(a), find(b)) else {
            return false;
        };
        if blend::joint(first, second) || blend::joint(second, first) {
            return false;
        }
        let along = first.toward[0].mul_add(second.toward[0], first.toward[1] * second.toward[1]);
        along.clamp(-1.0, 1.0).acos() >= blend::FLAT
    }

    /// The ground raised and lowered; within the tolerance of the ground, the
    /// ground bit for bit. Where the paved surface is no steeper than the
    /// grades, no point is below a fill face and above a cut face, so at most
    /// one moves it.
    fn graded(&self, base: f64, raise: f64, lower: f64) -> f64 {
        let design = self.restrict(base, base + raise - lower);
        if (design - base).abs() <= self.grading.tolerance {
            base
        } else {
            design
        }
    }

    /// Design height at `(x, y)` and whether a face there ran out at the limit
    /// before meeting the ground; `None` off the base surface.
    fn height(&self, px: f64, py: f64) -> Option<(f64, bool)> {
        let base = self.base.height_at(px, py)?.height;
        Some(self.over(base, px, py))
    }

    /// Where these faces can move the ground at all (#149 T3).
    pub(super) fn near(&self) -> Near {
        Near::new(self.outline, self.grading.limit)
    }

    /// The pavement's plan footprint (#149 T3).
    pub(super) fn footprint(&self) -> Footprint {
        Footprint::new(self.outline)
    }

    /// [`Self::height`] over a given current ground height rather than the
    /// base surface's: the unified build (#149 T1) grades the outline over the
    /// ground its region entries have already composed there.
    pub(super) fn over(&self, base: f64, px: f64, py: f64) -> (f64, bool) {
        if let Some(paved) = self.surface.height_at(px, py) {
            return (self.restrict(base, paved.height), false);
        }
        let moves = self.moves(base, px, py);
        let design = self.graded(base, moves.raise, moves.lower);
        let unlimited = self.graded(base, moves.raise_any, moves.lower_any);
        (design, design.to_bits() != unlimited.to_bits())
    }

    fn restrict(&self, base: f64, design: f64) -> f64 {
        match self.grading.effect {
            Effect::Cut => base.min(design),
            Effect::Fill => base.max(design),
            Effect::Both => design,
        }
    }
}

/// Region ring samples, every vertex and every [`CLIP_STEP`] metres between,
/// where the faces would leave the ground: there the ring ends them in a step.
pub(super) fn clipped(
    faces: &Faces<'_>,
    base: &Field,
    vertices: &[[f64; 3]],
    ring: &[u32],
) -> Result<u32, OutlineError> {
    let mut count = 0_u32;
    for (index, &from) in ring.iter().enumerate() {
        let to = ring.get((index + 1) % ring.len()).copied();
        let (Some(start), Some(end)) = (
            vertices.get(from as usize),
            to.and_then(|to| vertices.get(to as usize)),
        ) else {
            return Err(OutlineError::Coverage);
        };
        let samples = ((end[0] - start[0]).hypot(end[1] - start[1]) / CLIP_STEP)
            .ceil()
            .max(1.0);
        let mut sample = 0.0;
        while sample < samples {
            let along = sample / samples;
            sample += 1.0;
            let x = (end[0] - start[0]).mul_add(along, start[0]);
            let y = (end[1] - start[1]).mul_add(along, start[1]);
            let Some(ground) = base.height_at(x, y) else {
                continue;
            };
            if let Some((height, _)) = faces.height(x, y) {
                count += u32::from(height.to_bits() != ground.height.to_bits());
            }
        }
    }
    Ok(count)
}

/// The validated outline grading inputs, owned: the base and paved fields,
/// the outline's edges with their faces, and the grading. The T0 lane
/// ([`build`]) and the unified build (#149 T1) both grade through it.
pub(super) struct Operator {
    pub(super) base: Field,
    surface: Field,
    pub(super) edges: Vec<Edge>,
    grading: Grading,
}

impl Operator {
    /// # Errors
    /// [`OutlineError::Parameters`] for a bad limit, rounding, tolerance, slope
    /// or grade table; [`OutlineError::Field`] / [`OutlineError::Surface`] for
    /// a base or paved surface that is not a field (or has no outline).
    #[expect(
        clippy::too_many_arguments,
        reason = "the base and paved meshes, their grade table and the grading are one validated input set"
    )]
    pub(super) fn new(
        points: &[f64],
        triangles: &[u32],
        surface_points: &[f64],
        surface_triangles: &[u32],
        surface_grades: &[SurfaceGrade],
        grading: Grading,
    ) -> Result<Self, OutlineError> {
        let g = grading;
        if [g.limit, g.rounding, g.tolerance]
            .iter()
            .any(|v| !(v.is_finite() && *v >= 0.0))
        {
            return Err(OutlineError::Parameters);
        }
        let uniform = SurfaceGrade {
            cut: g.cut.gradient().map_err(|_| OutlineError::Parameters)?,
            fill: g.fill.gradient().map_err(|_| OutlineError::Parameters)?,
            rounding: g.rounding,
        };
        if !(surface_grades.is_empty() || surface_grades.len() == surface_points.len() / 3)
            || !surface_grades.iter().all(|grade| grade.valid())
        {
            return Err(OutlineError::Parameters);
        }
        let base = Field::new(points, triangles).map_err(|_| OutlineError::Field)?;
        let surface =
            Field::new(surface_points, surface_triangles).map_err(|_| OutlineError::Surface)?;
        let edges = outline(surface_points, surface_triangles, surface_grades, uniform);
        if edges.is_empty() {
            return Err(OutlineError::Surface);
        }
        Ok(Self {
            base,
            surface,
            edges,
            grading,
        })
    }

    /// The vertical height a fill and a cut toe round over: the largest grade times the largest
    /// rounding of any outline edge (#149 T2 traces the rounded toe across it).
    pub(super) fn toe_rounding(&self) -> [f64; 2] {
        let mut out = [0.0_f64; 2];
        for edge in &self.edges {
            let SurfaceGrade {
                cut,
                fill,
                rounding,
            } = edge.grade;
            if fill.is_finite() {
                out[0] = out[0].max(fill * rounding);
            }
            if cut.is_finite() {
                out[1] = out[1].max(cut * rounding);
            }
        }
        out
    }

    /// The faces over these inputs; builds the reach index once.
    pub(super) fn faces(&self) -> Faces<'_> {
        Faces::new(&self.base, &self.surface, &self.edges, self.grading)
    }

    /// `refine` with the steepest gradient the faces have wherever they are continuous: the
    /// grades, the base's and the pavement's (each as flat points and triangles, in that order).
    fn bound(
        &self,
        refine: Refine,
        [base, surface]: [Flat<'_>; 2],
        traced: [u32; 2],
    ) -> refine::Bound {
        let ((base_points, base_triangles), (surface_points, surface_triangles)) = (base, surface);
        refine::Bound {
            refine,
            steepest: self
                .edges
                .iter()
                .flat_map(|edge| [edge.grade.cut, edge.grade.fill])
                .filter(|grade| grade.is_finite())
                .fold(
                    refine::steepest(base_points, base_triangles)
                        .max(refine::steepest(surface_points, surface_triangles)),
                    f64::max,
                ),
            traced,
        }
    }

    /// #149 T3 in the unified build: `mesh` (its region rebuilt with the outline's lines)
    /// meshed to `refine` for these faces graded over `ground`, the surface the build's region
    /// entries composed (or the base); `None` when the mesher cannot take it (the caller keeps
    /// it). `rebuilt` is that mesh and the range of its line indices that are the traced toe and
    /// creases; `meshes` are the base and the pavement, as for the bound.
    pub(super) fn meshed(
        &self,
        ground: &(dyn Fn(f64, f64) -> Option<f64> + Sync),
        (mesh, traced): (&RegionMesh, [u32; 2]),
        refine: Refine,
        meshes: [Flat<'_>; 2],
    ) -> Option<(RegionMesh, Refined)> {
        let faces = self.faces();
        let design = |x: f64, y: f64| ground(x, y).map(|base| faces.over(base, x, y));
        let near = faces.near();
        let footprint = faces.footprint();
        let lane = mesher::Lane {
            design: &design,
            ground,
            base: &self.base,
            snap: self.grading.tolerance,
            near: &near,
            traced,
            paved: &|x, y| footprint.contains(x, y),
        };
        mesher::refine(
            &lane,
            &mut refine::Memo::default(),
            mesh,
            self.bound(refine, meshes, traced),
        )
    }
}

/// Grade the terrain inside `request.region` from the paved area.
///
/// # Errors
/// See [`OutlineError`].
pub fn build(request: &OutlineRequest<'_>) -> Result<Outlined, OutlineError> {
    build_with(request, Some(toe::Toe::DEFAULT))
}

/// [`build`] with the toe traced as `toe` asks, or not at all (the T2 measurements' "before").
pub(in crate::terrain) fn build_with(
    request: &OutlineRequest<'_>,
    toe_options: Option<toe::Toe>,
) -> Result<Outlined, OutlineError> {
    let operator = Operator::new(
        request.points,
        request.triangles,
        request.surface_points,
        request.surface_triangles,
        request.surface_grades,
        request.grading,
    )?;
    let base = &operator.base;
    let faces = operator.faces();
    if request.refine.is_some_and(|refine| !refine.valid()) {
        return Err(OutlineError::Parameters);
    }
    let supports = supports::prepare(
        request.region,
        request.surface_points,
        request.surface_triangles,
    )?;
    // #149 T2: the toe as breaklines, traced from these faces over the base, clear of every
    // caller line, support, spot and the stitched ring.
    let ground = |x: f64, y: f64| base.height_at(x, y).map(|sample| sample.height);
    let mut obstacles: Vec<&[f64]> = request.breaklines.to_vec();
    obstacles.extend(supports.lines.iter().map(Vec::as_slice));
    let toe_lines = toe_options.map_or_else(Vec::new, |options| {
        toe::lines(
            &faces,
            &ground,
            &supports.ring,
            &obstacles,
            request.spots,
            operator.toe_rounding(),
            options,
        )
    });
    let with_toe: Vec<&[f64]> = request
        .breaklines
        .iter()
        .copied()
        .chain(toe_lines.iter().map(Vec::as_slice))
        .collect();
    let rebuild_with = |breaklines: &[&[f64]], spots: &[f64], lines: &[Vec<f64>]| {
        retriangulate_with_supports(
            &RegionRequest {
                points: request.points,
                triangles: request.triangles,
                region: request.region,
                breaklines,
            },
            spots,
            &supports.ring,
            lines,
        )
        .map_err(OutlineError::Region)
    };
    let bound = request.refine.map(|refine| {
        operator.bound(
            refine,
            [
                (request.points, request.triangles),
                (request.surface_points, request.surface_triangles),
            ],
            [0, 0],
        )
    });
    // A toe the triangulation cannot take is left out, not a refusal: the toe lines refine the
    // mesh, they carry no height of their own. `toe` says how many lines the result has.
    // The toe and creases follow the caller's lines in the rebuild: a vertex on one is at the toe.
    let traced = |count: usize| {
        let first = u32::try_from(request.breaklines.len()).unwrap_or(u32::MAX);
        [
            first,
            first.saturating_add(u32::try_from(count).unwrap_or(u32::MAX)),
        ]
    };
    let (mesh, graded, refined, toe) = match refine::refined(
        &faces,
        |spots: &[f64], lines: &[Vec<f64>]| rebuild_with(&with_toe, spots, lines),
        request.spots,
        &supports.lines,
        bound.map(|bound| refine::Bound {
            traced: traced(toe_lines.len()),
            ..bound
        }),
    ) {
        Ok((mesh, graded, refined)) => (
            mesh,
            graded,
            refined,
            u32::try_from(toe_lines.len()).unwrap_or(u32::MAX),
        ),
        Err(_) if !toe_lines.is_empty() => {
            let (mesh, graded, refined) = refine::refined(
                &faces,
                |spots: &[f64], lines: &[Vec<f64>]| rebuild_with(request.breaklines, spots, lines),
                request.spots,
                &supports.lines,
                bound.map(|bound| refine::Bound {
                    traced: traced(0),
                    ..bound
                }),
            )?;
            (mesh, graded, refined, 0)
        }
        Err(error) => return Err(error),
    };
    let Graded {
        points,
        owners,
        based,
        unreached,
    } = graded;
    let vertices = plan::vertices(&mesh.points);
    let clipped = clipped(&faces, base, vertices, &mesh.ring)?;
    let volumes = exact_cut_fill(&ExactRequest {
        points: &points,
        base: &based,
        triangles: &mesh.triangles,
        inside: Some(&mesh.inside),
    })
    .map_err(OutlineError::Volume)?;
    Ok(Outlined {
        points,
        mesh,
        stitch: supports.ring,
        surface_supports: supports.sources,
        owners,
        volumes,
        unreached,
        clipped,
        refined,
        toe,
    })
}

#[cfg(test)]
mod tests;
