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
//! breaklines (the outline and rings offset from it, so a face has vertices to
//! lie on) and isolated points (the TIN's interior vertices). Private constraints
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

mod supports;

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
}

/// The graded mesh.
#[derive(Clone, Debug, PartialEq)]
pub struct Outlined {
    /// Flat xyz proposed vertices; the plan positions are `mesh.points`'.
    pub points: Vec<f64>,
    /// The rebuilt base mesh. Derived support breaklines append after the caller
    /// breaklines, mapped to source TIN edges by `surface_supports`. The split's
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

/// The paved area's outline: its TIN's boundary edges, as xyz segments.
fn outline(points: &[f64], triangles: &[u32]) -> Vec<[[f64; 3]; 2]> {
    let vertices = plan::vertices(points);
    let mut edges = BTreeMap::<(u32, u32), u32>::new();
    for &[a, b, c] in triangles.as_chunks::<3>().0 {
        for (from, to) in [(a, b), (b, c), (c, a)] {
            *edges.entry((from.min(to), from.max(to))).or_default() += 1;
        }
    }
    edges
        .into_iter()
        .filter(|&(_, count)| count == 1)
        .filter_map(|((a, b), _)| Some([*vertices.get(a as usize)?, *vertices.get(b as usize)?]))
        .collect()
}

/// The point of the edge `from`-`to` whose cone at `grade` reaches highest
/// over `(px, py)` (lowest when `cut`): its plan distance and height. Along the
/// edge the height is affine and the distance convex, so the cone height is
/// concave and its extremum closed form.
fn steepest([from, to]: [[f64; 3]; 2], [px, py]: [f64; 2], grade: f64, cut: bool) -> (f64, f64) {
    let (dx, dy) = (to[0] - from[0], to[1] - from[1]);
    let length = dx.hypot(dy);
    let at = |along: f64| {
        let t = if length > 0.0 { along / length } else { 0.0 };
        let (x, y) = (dx.mul_add(t, from[0]), dy.mul_add(t, from[1]));
        (
            (x - px).hypot(y - py),
            (to[2] - from[2]).mul_add(t, from[2]),
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

/// The validated grading: grades as rise per metre of run.
struct Faces<'a> {
    base: &'a Field,
    surface: &'a Field,
    outline: &'a [[[f64; 3]; 2]],
    cut: f64,
    fill: f64,
    grading: Grading,
}

/// The fill (highest) and cut (lowest) faces over a point: within the limit,
/// and without it.
#[derive(Clone, Copy)]
struct Envelope {
    fill: f64,
    cut: f64,
    fill_any: f64,
    cut_any: f64,
}

impl Faces<'_> {
    /// One cone's height `distance` out from an outline point at `edge`.
    fn cone(&self, edge: f64, distance: f64, rise: f64) -> f64 {
        let rounding = self.grading.rounding;
        if rounding > 0.0 && distance < rounding {
            top(edge, 0.0, rise, rounding, distance)
        } else {
            rise.mul_add(distance, edge)
        }
    }

    fn envelope(&self, px: f64, py: f64) -> Envelope {
        let mut faces = Envelope {
            fill: f64::NEG_INFINITY,
            cut: f64::INFINITY,
            fill_any: f64::NEG_INFINITY,
            cut_any: f64::INFINITY,
        };
        let limit = self.grading.limit;
        for &[from, to] in self.outline {
            if self.fill.is_finite() {
                let (distance, edge) = steepest([from, to], [px, py], self.fill, false);
                let face = self.cone(edge, distance, -self.fill);
                faces.fill_any = faces.fill_any.max(face);
                if distance <= limit {
                    faces.fill = faces.fill.max(face);
                }
            }
            if self.cut.is_finite() {
                let (distance, edge) = steepest([from, to], [px, py], self.cut, true);
                let face = self.cone(edge, distance, self.cut);
                faces.cut_any = faces.cut_any.min(face);
                if distance <= limit {
                    faces.cut = faces.cut.min(face);
                }
            }
        }
        faces
    }

    /// The ground raised to `fill` and lowered to `cut`, each easing onto it
    /// on its own; within the tolerance of the ground, the ground bit for bit.
    /// Where the paved surface is no steeper than the grades, no point is
    /// below the fill face and above the cut face, so at most one moves it.
    fn graded(&self, base: f64, fill: f64, cut: f64) -> f64 {
        let rounding = self.grading.rounding;
        let raise = toe(base, fill, false, self.fill * rounding) - base;
        let lower = base - toe(base, cut, true, self.cut * rounding);
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
        if let Some(paved) = self.surface.height_at(px, py) {
            return Some((self.restrict(base, paved.height), false));
        }
        let faces = self.envelope(px, py);
        let design = self.graded(base, faces.fill, faces.cut);
        let unlimited = self.graded(base, faces.fill_any, faces.cut_any);
        Some((design, design.to_bits() != unlimited.to_bits()))
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
fn clipped(
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

/// Grade the terrain inside `request.region` from the paved area.
///
/// # Errors
/// See [`OutlineError`].
pub fn build(request: &OutlineRequest<'_>) -> Result<Outlined, OutlineError> {
    let g = request.grading;
    if [g.limit, g.rounding, g.tolerance]
        .iter()
        .any(|v| !(v.is_finite() && *v >= 0.0))
    {
        return Err(OutlineError::Parameters);
    }
    let cut = g.cut.gradient().map_err(|_| OutlineError::Parameters)?;
    let fill = g.fill.gradient().map_err(|_| OutlineError::Parameters)?;
    let base = Field::new(request.points, request.triangles).map_err(|_| OutlineError::Field)?;
    let surface = Field::new(request.surface_points, request.surface_triangles)
        .map_err(|_| OutlineError::Surface)?;
    let edges = outline(request.surface_points, request.surface_triangles);
    if edges.is_empty() {
        return Err(OutlineError::Surface);
    }
    let faces = Faces {
        base: &base,
        surface: &surface,
        outline: &edges,
        cut,
        fill,
        grading: g,
    };
    let supports = supports::prepare(request)?;
    let mesh = retriangulate_with_supports(
        &RegionRequest {
            points: request.points,
            triangles: request.triangles,
            region: request.region,
            breaklines: request.breaklines,
        },
        request.spots,
        &supports.ring,
        &supports.lines,
    )
    .map_err(OutlineError::Region)?;
    let vertices = plan::vertices(&mesh.points);
    let mut fixed = vec![false; vertices.len()];
    for (&[a, b, c], _) in mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&mesh.inside)
        .filter(|(_, inside)| !**inside)
    {
        for vertex in [a, b, c] {
            if let Some(flag) = fixed.get_mut(vertex as usize) {
                *flag = true;
            }
        }
    }
    let mut points = Vec::with_capacity(mesh.points.len());
    let mut owners = Vec::with_capacity(vertices.len());
    let mut based = Vec::with_capacity(vertices.len());
    let mut unreached = 0_u32;
    for (&[x, y, z], &keep) in vertices.iter().zip(&fixed) {
        let height = if keep {
            z
        } else {
            let (height, short) = faces.height(x, y).ok_or(OutlineError::Coverage)?;
            unreached += u32::from(short);
            height
        };
        points.extend([x, y, height]);
        owners.push(height.to_bits() != z.to_bits());
        based.push(z);
    }
    let clipped = clipped(&faces, &base, vertices, &mesh.ring)?;
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
    })
}

#[cfg(test)]
mod tests;
