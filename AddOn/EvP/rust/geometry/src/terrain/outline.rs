//! Outline grading: the terrain under and around a paved area, built from the
//! area itself rather than from one path's centreline band.
//!
//! **Surface.** Inside the paved area the design height is the paved surface's
//! (a caller TIN over the area: every path, junction and kerb fillet as paved).
//!
//! **Faces.** Outside it, a query takes the nearest point of the area's outline
//! (the TIN's boundary edges), its height `h` there and its plan distance `d`.
//! The outline point is cut where `h` is below the base there by more than the
//! tolerance, fill above it, at grade otherwise (the base stays). The face runs
//! from `h` at the cut or fill grade; its first `rounding` metres are the same
//! cubic top as the band's ([`super::design`]), and its toe blends onto the
//! ground by the same smoothstep of the vertical gap. Past `limit` the base stays.
//!
//! **Mesh.** The region is rebuilt as in [`super::region`] with the caller's plan
//! breaklines (the outline and rings offset from it, so a face has vertices to
//! lie on) and isolated points (the TIN's interior vertices). Every vertex of an
//! outside triangle keeps its base height bit for bit; every other vertex takes
//! the design height. Volumes are the exact triangle sums against the base.
//!
//! Plan positions are world metres in `f64`; heights in metres.

use super::design::{toe, top};
use super::edit::Effect;
use super::field::Field;
use super::plan;
use super::region::{RegionError, RegionMesh, RegionRequest, retriangulate_region_with_points};
use super::volume::VolumeError;
use super::volume::exact::{ExactReport, ExactRequest, exact_cut_fill};
use crate::slope::Slope;
use std::collections::BTreeMap;

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
    /// The rebuilt base mesh and its mapping to the request.
    pub mesh: RegionMesh,
    /// Per vertex, whether the design changed its height (bitwise).
    pub owners: Vec<bool>,
    /// Exact cut/fill: site inside the region, context outside.
    pub volumes: ExactReport,
    /// Vertices past the limit whose face had not yet met the ground: the base
    /// is kept there, so the faces end in a step. Zero when every face meets it.
    pub unreached: u32,
}

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

/// The outline point nearest `(px, py)` in plan: its distance and the point,
/// with the height interpolated along its edge.
fn nearest(outline: &[[[f64; 3]; 2]], px: f64, py: f64) -> (f64, [f64; 3]) {
    let mut best = (f64::INFINITY, [0.0; 3]);
    for &[from, to] in outline {
        let (dx, dy) = (to[0] - from[0], to[1] - from[1]);
        let length2 = dx.mul_add(dx, dy * dy);
        let along = if length2 > 0.0 {
            ((px - from[0]).mul_add(dx, (py - from[1]) * dy) / length2).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let point = [
            dx.mul_add(along, from[0]),
            dy.mul_add(along, from[1]),
            (to[2] - from[2]).mul_add(along, from[2]),
        ];
        let distance = (point[0] - px).hypot(point[1] - py);
        if distance < best.0 {
            best = (distance, point);
        }
    }
    best
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

impl Faces<'_> {
    /// Design height at `(x, y)` and whether the face there ran out before
    /// meeting the ground; `None` off the base surface.
    fn height(&self, px: f64, py: f64) -> Option<(f64, bool)> {
        let base = self.base.height_at(px, py)?.height;
        if let Some(paved) = self.surface.height_at(px, py) {
            return Some((self.restrict(base, paved.height), false));
        }
        let (distance, [qx, qy, edge]) = nearest(self.outline, px, py);
        let ground = self
            .base
            .height_at(qx, qy)
            .map_or(base, |sample| sample.height);
        let delta = edge - ground;
        if !distance.is_finite() || delta.abs() <= self.grading.tolerance {
            return Some((base, false));
        }
        let cut = delta < 0.0;
        let grade = if cut { self.cut } else { self.fill };
        if !grade.is_finite() {
            return Some((base, false));
        }
        let rise = if cut { grade } else { -grade };
        let rounding = self.grading.rounding;
        if distance > self.grading.limit {
            let end = rise.mul_add(self.grading.limit, edge);
            let short = if cut { end < base } else { end > base };
            return Some((base, short));
        }
        let face = if rounding > 0.0 && distance < rounding {
            top(edge, 0.0, rise, rounding, distance)
        } else {
            rise.mul_add(distance, edge)
        };
        Some((
            self.restrict(base, toe(base, face, cut, grade * rounding)),
            false,
        ))
    }

    fn restrict(&self, base: f64, design: f64) -> f64 {
        match self.grading.effect {
            Effect::Cut => base.min(design),
            Effect::Fill => base.max(design),
            Effect::Both => design,
        }
    }
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
    let mesh = retriangulate_region_with_points(
        &RegionRequest {
            points: request.points,
            triangles: request.triangles,
            region: request.region,
            breaklines: request.breaklines,
        },
        request.spots,
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
        owners,
        volumes,
        unreached,
    })
}

#[cfg(test)]
mod tests;
