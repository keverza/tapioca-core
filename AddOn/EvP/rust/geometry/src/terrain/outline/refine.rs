//! Error-driven refinement of the graded region (#149 T3): the faces get their
//! vertices from the error they leave, not from offset rings.
//!
//! The region is rebuilt once with the caller's lines, the toe and creases and
//! the supports, graded, and then meshed on one constrained Delaunay
//! triangulation ([`super::mesher`]) until every triangle large enough to split
//! is within the tolerance of the faces, off the toe band and above the angle
//! limit, or the budget runs out. A step (a face ending at its limit or cut off
//! by the ring) is reported, not refined.

use std::collections::HashMap;

use super::{Faces, OutlineError, StepWitness, mesher, plan, push_step_witness};
use crate::terrain::region::RegionMesh;

/// The vertical error the faces may leave between the mesh's vertices.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Refine {
    /// Largest height difference, metres, between the faces and a triangle's
    /// plane at its samples; positive.
    pub tolerance: f64,
    /// A triangle whose longest side is shorter is not split again, metres:
    /// where the faces end in a step no plane fits, however small.
    pub shortest: f64,
    /// A triangle with both moved and unmoved vertices straddles the toe and
    /// is split, whatever its error, until its longest side is no longer,
    /// metres. A triangle whose vertices are all unmoved is the ground's own
    /// surface, so the ground is kept exactly everywhere but in this band
    /// beyond the toe (#65). Also the width of a demonstrated boundary-step
    /// strip whose error is reported separately from actionable residual.
    pub toe: f64,
    /// Most vertices the refinement may add; past it the mesh is kept as it is
    /// and [`Refined::residual`] says how far it still is.
    pub budget: u32,
    /// Smallest triangle angle, degrees, refined towards wherever no ring side
    /// prevents it; zero refines for the error alone. At most 30 (Delaunay
    /// refinement is sure to finish below about 20.7).
    pub angle: f64,
}

impl Refine {
    /// One centimetre, down to five-centimetre triangles, a ten-centimetre toe
    /// band, at most 200 000 added vertices, angles towards 20 degrees.
    pub const DEFAULT: Self = Self {
        tolerance: 0.01,
        shortest: 0.05,
        toe: 0.1,
        budget: 200_000,
        angle: 20.0,
    };

    pub(super) fn valid(self) -> bool {
        self.tolerance.is_finite()
            && self.tolerance > 0.0
            && self.shortest.is_finite()
            && self.shortest > 0.0
            && self.toe.is_finite()
            && self.toe >= self.shortest
            && (0.0..=30.0).contains(&self.angle)
    }
}

/// What the refinement did.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Refined {
    /// Vertices it added.
    pub added: u32,
    /// Rebuilds it took (the first build included).
    pub rounds: u32,
    /// Largest actionable sampled error left, metres, over triangles still large enough to split;
    /// demonstrated, bounded boundary-step strips are excluded.
    pub residual: f64,
    /// Whether it stopped at its budget, its round cap or a refused rebuild
    /// rather than at the tolerance.
    pub exhausted: bool,
}

/// Certify the eligible sampled vertical-error target. Angle-only exhaustion is allowed where
/// constrained input angles cannot be improved; demonstrated boundary-step strips are reported
/// separately. An above-tolerance actionable residual is never publishable.
pub(super) fn certify_vertical(report: &Refined, request: &Refine) -> Result<(), OutlineError> {
    if report.residual <= request.tolerance {
        Ok(())
    } else {
        Err(OutlineError::Refinement)
    }
}

/// A design height at a plan point and whether a face ran out there (as
/// [`Faces::height`]); `None` off the base surface.
pub(in crate::terrain) type Design<'a> = dyn Fn(f64, f64) -> Option<(f64, bool)> + 'a;

/// The faces' heights on a rebuilt mesh.
pub(super) struct Graded {
    pub points: Vec<f64>,
    pub owners: Vec<bool>,
    pub based: Vec<f64>,
    pub unreached: u32,
    pub steps: Vec<StepWitness>,
}

/// The faces evaluated once per plan point. Each round rebuilds the mesh, but
/// most of its vertices and triangles are the last round's.
#[derive(Default)]
pub(super) struct Memo {
    heights: HashMap<[u64; 2], Option<(f64, bool)>>,
    errors: HashMap<[[u64; 2]; 3], f64>,
}

impl Memo {
    pub(super) fn height(&mut self, design: &Design<'_>, x: f64, y: f64) -> Option<(f64, bool)> {
        *self
            .heights
            .entry([x.to_bits(), y.to_bits()])
            .or_insert_with(|| design(x, y))
    }

    /// [`sampled_error`] of the triangle, by its corners' plan positions (its
    /// corners' heights follow from them).
    pub(super) fn error(&mut self, design: &Design<'_>, corners: [[f64; 3]; 3]) -> f64 {
        let mut id = corners.map(|[x, y, _]| [x.to_bits(), y.to_bits()]);
        id.sort_unstable();
        if let Some(&error) = self.errors.get(&id) {
            return error;
        }
        let error = sampled_error(self, design, corners);
        self.errors.insert(id, error);
        error
    }
}

/// Every vertex of an outside triangle keeps its base height bit for bit;
/// every other vertex takes the design height.
pub(super) fn grade(
    faces: &Faces<'_>,
    memo: &mut Memo,
    mesh: &RegionMesh,
    steps: Vec<StepWitness>,
) -> Result<Graded, OutlineError> {
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
    let mut graded = Graded {
        points: Vec::with_capacity(mesh.points.len()),
        owners: Vec::with_capacity(vertices.len()),
        based: Vec::with_capacity(vertices.len()),
        unreached: 0,
        steps,
    };
    for (vertex, (&[x, y, z], &keep)) in (0_u32..).zip(vertices.iter().zip(&fixed)) {
        let height = if keep {
            z
        } else {
            let (height, short) = memo
                .height(&|x, y| faces.height(x, y), x, y)
                .ok_or(OutlineError::Coverage)?;
            graded.unreached += u32::from(short);
            if short {
                push_step_witness(
                    &mut graded.steps,
                    StepWitness::ReachLimit {
                        vertex,
                        position: [x, y, height],
                    },
                );
            }
            height
        };
        graded.points.extend([x, y, height]);
        graded.owners.push(height.to_bits() != z.to_bits());
        graded.based.push(z);
    }
    Ok(graded)
}

/// The steepest plan gradient of any triangle of a flat xyz mesh.
pub(super) fn steepest(points: &[f64], triangles: &[u32]) -> f64 {
    let vertices = plan::vertices(points);
    triangles
        .as_chunks::<3>()
        .0
        .iter()
        .filter_map(|&[first, second, third]| {
            let (origin, along, across) = (
                vertices.get(first as usize)?,
                vertices.get(second as usize)?,
                vertices.get(third as usize)?,
            );
            let (u, v) = (
                [
                    along[0] - origin[0],
                    along[1] - origin[1],
                    along[2] - origin[2],
                ],
                [
                    across[0] - origin[0],
                    across[1] - origin[1],
                    across[2] - origin[2],
                ],
            );
            let normal = [
                u[1].mul_add(v[2], -u[2] * v[1]),
                u[2].mul_add(v[0], -u[0] * v[2]),
                u[0].mul_add(v[1], -u[1] * v[0]),
            ];
            (normal[2].abs() > 0.0).then(|| normal[0].hypot(normal[1]) / normal[2].abs())
        })
        .fold(0.0, f64::max)
}

/// A refinement bound and the steepest gradient the faces have wherever they
/// are continuous (the grades, the ground's, the pavement's).
#[derive(Clone, Copy)]
pub(super) struct Bound {
    pub refine: Refine,
    pub steepest: f64,
    /// The rebuilt region's traced toe and crease lines (#149 T2), as a range of its line
    /// indices: a vertex on one is at the toe.
    pub traced: [u32; 2],
}

/// The region rebuilt from `spots` and `lines`, graded, and refined to
/// `bound` when there is one. A mesher that cannot take the rebuilt region
/// leaves it as it is, reported exhausted.
pub(super) fn refined(
    faces: &Faces<'_>,
    rebuild: impl Fn(&[f64], &[Vec<f64>]) -> Result<RegionMesh, OutlineError>,
    spots: &[f64],
    lines: &[Vec<f64>],
    bound: Option<Bound>,
) -> Result<(RegionMesh, Graded, Refined), OutlineError> {
    let mesh = rebuild(spots, lines)?;
    let mut memo = Memo::default();
    let Some(bound) = bound else {
        let graded = grade(faces, &mut memo, &mesh, Vec::new())?;
        return Ok((
            mesh,
            graded,
            Refined {
                rounds: 1,
                ..Refined::default()
            },
        ));
    };
    let design = |x: f64, y: f64| faces.height(x, y);
    let ground = |x: f64, y: f64| faces.base.height_at(x, y).map(|sample| sample.height);
    let near = faces.near();
    let footprint = faces.footprint();
    let lane = mesher::Lane {
        design: &design,
        ground: &ground,
        base: faces.base,
        snap: faces.grading.tolerance,
        near: &near,
        traced: bound.traced,
        paved: &|x, y| footprint.contains(x, y),
        allowed: &|_, _| true,
    };
    let (mesh, report, steps) = mesher::refine(&lane, &mut memo, &mesh, bound).unwrap_or((
        mesh,
        Refined {
            rounds: 1,
            exhausted: true,
            ..Refined::default()
        },
        Vec::new(),
    ));
    let graded = grade(faces, &mut memo, &mesh, steps)?;
    Ok((mesh, graded, report))
}

/// Where each triangle is sampled, as weights of its corners: its centroid and
/// the centroids of the three corner sub-triangles, all strictly inside it.
pub(super) const SAMPLES: [[f64; 3]; 4] = [
    [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
    [2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0],
    [1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0],
    [1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0],
];

/// Where a triangle's error is sampled: [`SAMPLES`] and its sides' midpoints. Between two
/// triangles the faces stray furthest from the mesh along their shared side; four inside
/// samples at the tolerance left 1.3 cm there against ten-sample measurements.
const ERROR_SAMPLES: [[f64; 3]; 7] = [
    [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
    [2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0],
    [1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0],
    [1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0],
    [0.5, 0.5, 0.0],
    [0.0, 0.5, 0.5],
    [0.5, 0.0, 0.5],
];

/// The point with barycentric weights `w` in the triangle `corners`.
pub(super) fn mix(w: [f64; 3], corners: [[f64; 3]; 3]) -> [f64; 3] {
    let [[xa, ya, za], [xb, yb, zb], [xc, yc, zc]] = corners;
    let [wa, wb, wc] = w;
    [
        wa.mul_add(xa, wb.mul_add(xb, wc * xc)),
        wa.mul_add(ya, wb.mul_add(yb, wc * yc)),
        wa.mul_add(za, wb.mul_add(zb, wc * zc)),
    ]
}

/// The largest gap between the faces and the triangle's plane at its samples. Interior samples
/// share the height memo with the mesher's moved/kept assessment; only the side midpoints are new.
fn sampled_error(memo: &mut Memo, design: &Design<'_>, corners: [[f64; 3]; 3]) -> f64 {
    ERROR_SAMPLES.iter().fold(0.0_f64, |worst, &weights| {
        let [x, y, z] = mix(weights, corners);
        memo.height(design, x, y)
            .map_or(worst, |(height, _)| worst.max((height - z).abs()))
    })
}
