//! S3b.2 local terrain operators (G13): point, curve, area, path and surface
//! edits on an existing mesh's vertex heights.
//!
//! **What moves.** Only heights. Vertex indices, plan positions and triangles are
//! unchanged, so every output vertex and face keeps its input identity; a result
//! names, per vertex, the edit that moved it.
//!
//! **One edit, one vertex.** The edit's core is its geometry (a point with a flat
//! radius, a curve or path band, the inside of an area ring, the covered plan of a
//! target surface). `d` is the vertex's plan distance outside the core (zero
//! inside) and `base` the core height there: the point's height, the nearest
//! curve/path height (plus crossfall across a path band), the area level, or the
//! target surface (its nearest boundary height outside it). Beyond
//! [`Edit::max_distance`] an edit does nothing.
//!
//! - [`Transition::Falloff`]: `influence = 1 - SmootherStep(d / blur)` for
//!   `0 < d < blur`, 1 inside the core, 0 beyond the blur; `SmootherStep(t) =
//!   6t^5 - 15t^4 + 10t^3`. `new = old + influence * (target - old)` where
//!   `target = base` (absolute) or `old + base` (relative, `base` a delta).
//! - [`Transition::Daylight`] (absolute only): above the base the cut face rises
//!   at its own slope, `base + d * tan(cut)`, and `new = min(old, that)`; below it
//!   the fill falls at its own slope, `base - d * tan(fill)`, and `new = max(old,
//!   that)`. A vertical cut leaves everything outside the core untouched. A side's
//!   extension terminates its march: vertices beyond it are left as they are and
//!   counted as clipped (the dangling-edge diagnostic), never silently extended.
//!
//! [`Effect::Cut`] keeps `min(old, new)`, [`Effect::Fill`] `max(old, new)`,
//! [`Effect::Both`] `new`; then [`Edit::strength`] scales the displacement.
//!
//! **Several edits.** Each vertex takes the displacement of largest magnitude
//! (the lower edit index on a tie). Vertices outside the clip ring (the property
//! line), on it, or inside or on a protected ring never move.
//!
//! **Smoothing.** An optional Gaussian relaxation (radius `r`, `sigma = r / 3`,
//! kernel renormalized over the vertices within `r`) is applied to every movable
//! vertex within `r` of a moved one, from the edited heights. A vertex it moves
//! without an edit having moved it takes the owner of its nearest moved vertex.

use super::field::{Field, Segment};
use super::plan::{self, Nearest};
use crate::slope::Slope;

/// How a target height is read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// The target geometry's height is the new height.
    Absolute,
    /// The target geometry's height is a displacement added to the old height.
    Relative,
}

/// Which way an edit may move the surface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Effect {
    /// Only lower.
    Cut,
    /// Only raise.
    Fill,
    /// Either.
    Both,
}

/// One side of a daylight transition.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Daylight {
    /// Grade of the transition face.
    pub slope: Slope,
    /// Plan distance in metres beyond which the march stops; `None` for none.
    pub extension: Option<f64>,
}

/// How an edit blends from its core into the existing surface.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Transition {
    /// Separate cut and fill faces marched to the surface (absolute mode only).
    Daylight {
        /// The face used where the surface is above the base.
        cut: Daylight,
        /// The face used where the surface is below the base.
        fill: Daylight,
    },
    /// `1 - SmootherStep(d / blur)` influence over `blur` metres.
    Falloff {
        /// Blur width in metres, positive.
        blur: f64,
    },
}

/// The level inside an area edit.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum AreaLevel {
    /// This height (absolute) or displacement (relative), metres.
    Height(f64),
    /// The mean old height of the vertices strictly inside the ring (absolute
    /// only); an area with no vertex inside does nothing.
    Flatten,
}

/// A side of a path, looking along its direction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Hand {
    /// To the left.
    Left,
    /// To the right.
    Right,
}

/// Cross slope of a path band, twisting about its centreline.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Crossfall {
    /// Grade across the band.
    pub slope: Slope,
    /// The lower edge.
    pub falls_to: Hand,
}

/// The geometry an edit acts from.
#[derive(Clone, Copy, Debug)]
pub enum Target<'a> {
    /// A point; flat within `radius` metres (a cone when zero).
    Point {
        /// x, y and the height or displacement, metres.
        position: [f64; 3],
        /// Flat core radius, metres, non-negative.
        radius: f64,
    },
    /// An open xyz polyline; flat within `half_width` metres either side.
    Curve {
        /// Flat xyz, at least two vertices.
        points: &'a [f64],
        /// Half band width, metres, non-negative.
        half_width: f64,
    },
    /// A closed xy ring (z ignored), implicitly closed, at least three vertices.
    Area {
        /// Flat xyz ring.
        ring: &'a [f64],
        /// The level inside it.
        level: AreaLevel,
    },
    /// An open xyz centreline with a band of `width` metres and optional crossfall.
    Path {
        /// Flat xyz, at least two vertices.
        centreline: &'a [f64],
        /// Full band width, metres, non-negative.
        width: f64,
        /// Cross slope across the band.
        crossfall: Option<Crossfall>,
    },
    /// Another surface: its height where it covers the plan, its nearest
    /// boundary height outside it.
    Surface {
        /// The target surface.
        field: &'a Field,
    },
}

/// One operation node.
#[derive(Clone, Copy, Debug)]
pub struct Edit<'a> {
    /// Its geometry.
    pub target: Target<'a>,
    /// Absolute or relative heights.
    pub mode: Mode,
    /// Cut, fill or both.
    pub effect: Effect,
    /// Falloff or daylight.
    pub transition: Transition,
    /// Plan distance outside the core beyond which nothing moves, metres.
    pub max_distance: f64,
    /// Displacement scale in `(0, 1]`.
    pub strength: f64,
}

/// Borrowed edit request over an existing mesh.
#[derive(Clone, Copy, Debug)]
pub struct EditRequest<'a> {
    /// Flat xyz vertices, world metres.
    pub points: &'a [f64],
    /// Vertex triples (used only for face attribution).
    pub triangles: &'a [u32],
    /// Edits in operation order; their index is the attribution.
    pub edits: &'a [Edit<'a>],
    /// The property line: only vertices strictly inside it may move.
    pub clip: Option<&'a [f64]>,
    /// Rings inside or on which nothing moves.
    pub protected: &'a [&'a [f64]],
    /// Gaussian relaxation radius in metres; `None` or zero for none.
    pub smoothing: Option<f64>,
}

/// A changed face and one edit that moved one of its vertices.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct FaceOwner {
    /// Triangle index.
    pub face: u32,
    /// Edit index.
    pub edit: u32,
}

/// What one edit did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EditReport {
    /// Vertices whose winning displacement was this edit's.
    pub moved: u32,
    /// Vertices beyond a daylight extension that the march would have moved.
    pub extension_clipped: u32,
    /// Vertices beyond the maximum distance that the transition would have moved.
    pub limit_clipped: u32,
}

/// Why an edit left a vertex where it was.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClipKind {
    /// Beyond a daylight extension (counted in [`EditReport::extension_clipped`]).
    Extension,
    /// Beyond the maximum distance (counted in [`EditReport::limit_clipped`]).
    Limit,
}

/// One located soft-tier clip: where an edit would have moved a vertex and did
/// not. Additive to the [`EditReport`] counters, which they sum to.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Clip {
    /// Edit index.
    pub edit: u32,
    /// Input vertex; its xyz is the position.
    pub vertex: u32,
    /// Which limit held it.
    pub kind: ClipKind,
    /// Plan distance from the edit's core, metres.
    pub distance: f64,
    /// The height change withheld, metres (signed, after strength).
    pub withheld: f64,
}

/// The edited heights and their attribution.
#[derive(Clone, Debug, PartialEq)]
pub struct Edited {
    /// One height per input vertex, metres.
    pub heights: Vec<f64>,
    /// Per vertex, the edit that moved it.
    pub owners: Vec<Option<u32>>,
    /// Changed faces and their owning edits, ascending.
    pub faces: Vec<FaceOwner>,
    /// One report per edit.
    pub reports: Vec<EditReport>,
    /// Located clips by vertex, then edit.
    pub clips: Vec<Clip>,
}

/// Edit refusals. Nothing partial is returned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EditError {
    /// Points or triangles not whole triples, too many to index, or a polyline
    /// or ring with too few vertices.
    Shape,
    /// A coordinate or parameter is NaN or infinite.
    NonFinite,
    /// A triangle names a vertex that does not exist.
    Index,
    /// A blur, width, radius, distance, strength or smoothing radius out of
    /// range, or daylight/flatten requested in relative mode.
    Parameters,
    /// A slope does not describe a grade.
    Slope,
}

impl EditError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "EditShape: whole triples and long-enough polylines and rings required",
            Self::NonFinite => "EditNonFinite: coordinates and parameters must be finite",
            Self::Index => "EditIndex: a triangle names a missing vertex",
            Self::Parameters => {
                "EditParameters: blur > 0, widths/radii/distance >= 0, 0 < strength <= 1; daylight and flatten are absolute"
            }
            Self::Slope => "EditSlope: a transition or crossfall slope is not a valid grade",
        }
    }
}

/// `6t^5 - 15t^4 + 10t^3` on `[0, 1]`, clamped outside.
#[must_use]
pub fn smoother_step(t: f64) -> f64 {
    let t = t.clamp(0.0, 1.0);
    t * t * t * (t * (t * 6.0 - 15.0) + 10.0)
}

/// An edit checked and prepared for evaluation.
struct Prepared<'a> {
    edit: &'a Edit<'a>,
    core: Core<'a>,
    /// Grade of the cut and fill faces (daylight), or `None` (falloff).
    faces: Option<([f64; 2], [Option<f64>; 2])>,
    /// Plan bounding box of the core, grown by the maximum distance.
    reach: [f64; 4],
}

enum Core<'a> {
    Point([f64; 3], f64),
    Line(&'a [[f64; 3]], f64, Option<(f64, Hand)>),
    Area(&'a [[f64; 3]], Option<f64>),
    Surface(&'a Field, Vec<Segment>),
}

fn finite(values: &[f64]) -> Result<(), EditError> {
    if values.iter().all(|value| value.is_finite()) {
        Ok(())
    } else {
        Err(EditError::NonFinite)
    }
}

fn non_negative(value: f64) -> Result<f64, EditError> {
    if !value.is_finite() {
        Err(EditError::NonFinite)
    } else if value < 0.0 {
        Err(EditError::Parameters)
    } else {
        Ok(value)
    }
}

fn polyline(pool: &[f64], minimum: usize) -> Result<&[[f64; 3]], EditError> {
    if !pool.len().is_multiple_of(3) {
        return Err(EditError::Shape);
    }
    finite(pool)?;
    let vertices = plan::vertices(pool);
    if vertices.len() < minimum {
        return Err(EditError::Shape);
    }
    Ok(vertices)
}

fn bounds(points: &[[f64; 3]]) -> [f64; 4] {
    points.iter().fold(
        [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ],
        |[x0, y0, x1, y1], &[x, y, _]| [x0.min(x), y0.min(y), x1.max(x), y1.max(y)],
    )
}

fn grow([x0, y0, x1, y1]: [f64; 4], by: f64) -> [f64; 4] {
    [x0 - by, y0 - by, x1 + by, y1 + by]
}

fn prepare<'a>(edit: &'a Edit<'a>, vertices: &[[f64; 3]]) -> Result<Prepared<'a>, EditError> {
    let max_distance = non_negative(edit.max_distance)?;
    if !edit.strength.is_finite() {
        return Err(EditError::NonFinite);
    }
    if !(edit.strength > 0.0 && edit.strength <= 1.0) {
        return Err(EditError::Parameters);
    }
    let faces = match edit.transition {
        Transition::Falloff { blur } => {
            if !blur.is_finite() {
                return Err(EditError::NonFinite);
            }
            if blur <= 0.0 {
                return Err(EditError::Parameters);
            }
            None
        }
        Transition::Daylight { cut, fill } => {
            if edit.mode == Mode::Relative {
                return Err(EditError::Parameters);
            }
            let grade = |side: Daylight| side.slope.gradient().map_err(|_| EditError::Slope);
            let extension = |side: Daylight| side.extension.map(non_negative).transpose();
            Some((
                [grade(cut)?, grade(fill)?],
                [extension(cut)?, extension(fill)?],
            ))
        }
    };
    let (core, extent) = match edit.target {
        Target::Point { position, radius } => {
            finite(&position)?;
            let radius = non_negative(radius)?;
            let [x, y, _] = position;
            (Core::Point(position, radius), grow([x, y, x, y], radius))
        }
        Target::Curve { points, half_width } => {
            let line = polyline(points, 2)?;
            let half = non_negative(half_width)?;
            (Core::Line(line, half, None), grow(bounds(line), half))
        }
        Target::Path {
            centreline,
            width,
            crossfall,
        } => {
            let line = polyline(centreline, 2)?;
            let half = non_negative(width)? / 2.0;
            let fall = crossfall
                .map(|fall| {
                    fall.slope
                        .gradient()
                        .map(|g| (g, fall.falls_to))
                        .map_err(|_| EditError::Slope)
                })
                .transpose()?;
            if fall.is_some_and(|(g, _)| !g.is_finite()) {
                return Err(EditError::Slope);
            }
            (Core::Line(line, half, fall), grow(bounds(line), half))
        }
        Target::Area { ring, level } => {
            let ring = polyline(ring, 3)?;
            let level = match level {
                AreaLevel::Height(value) => {
                    finite(&[value])?;
                    Some(value)
                }
                AreaLevel::Flatten if edit.mode == Mode::Relative => {
                    return Err(EditError::Parameters);
                }
                AreaLevel::Flatten => {
                    let inside: Vec<f64> = vertices
                        .iter()
                        .filter(|&&[x, y, _]| plan::inside(ring, x, y) == Some(true))
                        .map(|&[_, _, z]| z)
                        .collect();
                    (!inside.is_empty()).then(|| {
                        inside.iter().sum::<f64>()
                            / f64::from(u32::try_from(inside.len()).unwrap_or(u32::MAX))
                    })
                }
            };
            (Core::Area(ring, level), bounds(ring))
        }
        Target::Surface { field } => {
            let box3 = field.bounds();
            (
                Core::Surface(field, field.boundary()),
                [box3.min[0], box3.min[1], box3.max[0], box3.max[1]],
            )
        }
    };
    Ok(Prepared {
        edit,
        core,
        faces,
        reach: grow(extent, max_distance),
    })
}

/// `(d, base)` for a vertex, or `None` where the edit defines nothing.
fn locate(core: &Core<'_>, x: f64, y: f64) -> Option<(f64, f64)> {
    match core {
        Core::Point([px, py, pz], radius) => {
            Some(((x - px).hypot(y - py) - radius).max(0.0)).map(|d| (d, *pz))
        }
        Core::Line(line, half, fall) => {
            let Nearest {
                distance, z, left, ..
            } = plan::nearest(line, false, x, y)?;
            let across = left.clamp(-half, *half);
            let twist = fall.map_or(0.0, |(gradient, hand)| match hand {
                Hand::Left => -gradient * across,
                Hand::Right => gradient * across,
            });
            Some(((distance - half).max(0.0), z + twist))
        }
        Core::Area(ring, level) => {
            let level = (*level)?;
            match plan::inside(ring, x, y) {
                Some(false) => plan::nearest(ring, true, x, y).map(|near| (near.distance, level)),
                _ => Some((0.0, level)),
            }
        }
        Core::Surface(field, boundary) => {
            if let Some(sample) = field.height_at(x, y) {
                return Some((0.0, sample.height));
            }
            boundary
                .iter()
                .filter_map(|segment| plan::nearest(segment, false, x, y))
                .min_by(|a, b| a.distance.total_cmp(&b.distance))
                .map(|near| (near.distance, near.z))
        }
    }
}

/// Displacement and clipping for one edit at one vertex.
struct Outcome {
    displacement: f64,
    extension_clipped: bool,
    limit_clipped: bool,
    distance: f64,
    withheld: f64,
}

fn evaluate(prepared: &Prepared<'_>, x: f64, y: f64, old: f64) -> Option<Outcome> {
    let [x0, y0, x1, y1] = prepared.reach;
    if x < x0 || x > x1 || y < y0 || y > y1 {
        return None;
    }
    let edit = prepared.edit;
    let (d, base) = locate(&prepared.core, x, y)?;
    let mut extension_clipped = false;
    let mut withheld = 0.0;
    let raw = match (edit.transition, prepared.faces) {
        (Transition::Daylight { .. }, Some(([cut, fill], [cut_extension, fill_extension]))) => {
            let (candidate, beyond) = if old > base {
                let face = if d > 0.0 { base + d * cut } else { base };
                (old.min(face), cut_extension.is_some_and(|limit| d > limit))
            } else {
                let face = if d > 0.0 { base - d * fill } else { base };
                (old.max(face), fill_extension.is_some_and(|limit| d > limit))
            };
            if beyond {
                extension_clipped = (candidate - old).abs() > 0.0;
                withheld = edit.strength * (candidate - old);
                old
            } else {
                candidate
            }
        }
        (Transition::Falloff { blur }, _) => {
            let influence = if d <= 0.0 {
                1.0
            } else {
                1.0 - smoother_step(d / blur)
            };
            let target = if edit.mode == Mode::Absolute {
                base
            } else {
                old + base
            };
            old + influence * (target - old)
        }
        _ => old,
    };
    let limited = match edit.effect {
        Effect::Cut => old.min(raw),
        Effect::Fill => old.max(raw),
        Effect::Both => raw,
    };
    let displacement = edit.strength * (limited - old);
    if d > edit.max_distance {
        return Some(Outcome {
            displacement: 0.0,
            extension_clipped: false,
            limit_clipped: displacement != 0.0,
            distance: d,
            withheld: displacement,
        });
    }
    Some(Outcome {
        displacement,
        extension_clipped,
        limit_clipped: false,
        distance: d,
        withheld,
    })
}

/// Apply edits to a mesh's vertex heights.
///
/// O(vertices x edits x target segments); each edit skips vertices outside its
/// reach box. Worker-classified; allocates.
///
/// # Errors
/// See [`EditError`]; nothing partial is returned.
pub fn edit_terrain(request: &EditRequest<'_>) -> Result<Edited, EditError> {
    let points = request.points;
    if !points.len().is_multiple_of(3) || !request.triangles.len().is_multiple_of(3) {
        return Err(EditError::Shape);
    }
    if u32::try_from(points.len() / 3).is_err() || u32::try_from(request.edits.len()).is_err() {
        return Err(EditError::Shape);
    }
    finite(points)?;
    let vertices = plan::vertices(points);
    if request
        .triangles
        .iter()
        .any(|&v| v as usize >= vertices.len())
    {
        return Err(EditError::Index);
    }
    let clip = request.clip.map(|ring| polyline(ring, 3)).transpose()?;
    let protected = request
        .protected
        .iter()
        .map(|ring| polyline(ring, 3))
        .collect::<Result<Vec<_>, _>>()?;
    let smoothing = request
        .smoothing
        .map(non_negative)
        .transpose()?
        .filter(|&r| r > 0.0);
    let prepared = request
        .edits
        .iter()
        .map(|edit| prepare(edit, vertices))
        .collect::<Result<Vec<_>, _>>()?;
    let movable: Vec<bool> = vertices
        .iter()
        .map(|&[x, y, _]| {
            clip.is_none_or(|ring| plan::inside(ring, x, y) == Some(true))
                && protected
                    .iter()
                    .all(|ring| plan::inside(ring, x, y) == Some(false))
        })
        .collect();
    let mut heights: Vec<f64> = vertices.iter().map(|&[_, _, z]| z).collect();
    let mut owners: Vec<Option<u32>> = vec![None; vertices.len()];
    let mut reports = vec![EditReport::default(); prepared.len()];
    let mut clips = Vec::new();
    for (vertex, &[x, y, old]) in (0_u32..).zip(vertices) {
        if !movable.get(vertex as usize).copied().unwrap_or(false) {
            continue;
        }
        let mut best: Option<(f64, u32)> = None;
        for (index, (edit, report)) in (0_u32..).zip(prepared.iter().zip(reports.iter_mut())) {
            let Some(outcome) = evaluate(edit, x, y, old) else {
                continue;
            };
            report.extension_clipped += u32::from(outcome.extension_clipped);
            report.limit_clipped += u32::from(outcome.limit_clipped);
            for (clipped, kind) in [
                (outcome.extension_clipped, ClipKind::Extension),
                (outcome.limit_clipped, ClipKind::Limit),
            ] {
                if clipped {
                    clips.push(Clip {
                        edit: index,
                        vertex,
                        kind,
                        distance: outcome.distance,
                        withheld: outcome.withheld,
                    });
                }
            }
            if outcome.displacement != 0.0
                && best.is_none_or(|(most, _)| outcome.displacement.abs() > most.abs())
            {
                best = Some((outcome.displacement, index));
            }
        }
        if let Some((displacement, index)) = best {
            if let Some(slot) = heights.get_mut(vertex as usize) {
                *slot = old + displacement;
            }
            if let Some(slot) = owners.get_mut(vertex as usize) {
                *slot = Some(index);
            }
            if let Some(report) = reports.get_mut(index as usize) {
                report.moved += 1;
            }
        }
    }
    if let Some(radius) = smoothing {
        smooth::relax(vertices, &movable, radius, &mut heights, &mut owners);
    }
    let mut faces = Vec::new();
    for (face, &triangle) in (0_u32..).zip(request.triangles.as_chunks::<3>().0) {
        for corner in triangle {
            let index = corner as usize;
            let moved = heights
                .get(index)
                .zip(vertices.get(index))
                .is_some_and(|(new, old)| (new - old[2]).abs() > 0.0);
            if let (true, Some(Some(edit))) = (moved, owners.get(index)) {
                faces.push(FaceOwner { face, edit: *edit });
            }
        }
    }
    faces.sort_unstable();
    faces.dedup();
    Ok(Edited {
        heights,
        owners,
        faces,
        reports,
        clips,
    })
}

mod smooth;

#[cfg(test)]
mod tests;
