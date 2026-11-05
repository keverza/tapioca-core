//! Error-driven refinement of the graded region (#149 T3): the faces get their
//! vertices from the error they leave, not from offset rings.
//!
//! Every inside triangle is sampled at its centroid and its three corner
//! sub-centroids. Where the faces there are further than the tolerance from
//! the triangle's plane, its longest side is bisected; the region is rebuilt
//! with the new vertices and graded again, round after round, until no
//! triangle still large enough to split misses the faces.
//!
//! A free side takes its midpoint as an isolated point. A constrained side (a
//! base edge, a breakline or a support) would leave a point beside it whole,
//! so it is crossed by a line instead: the triangulator gives every crossing a
//! vertex on both constraints. The line runs from one opposite corner to the
//! other, so its pieces are the edges a bisection makes; where that is not
//! clean it is a short line between the two triangles. A ring side is the
//! stitch and is never split; a triangle against it bisects its other sides.
//!
//! **Steps.** Inside a triangle a field no steeper than the grades, the ground
//! and the pavement strays from the corners' plane by at most twice that slope
//! times the triangle's size. A larger gap is a step, where a face ends at its
//! limit or at the ring: it is reported ([`super::Outlined::unreached`],
//! [`super::Outlined::clipped`]) and not refined.
//!
//! **Toe band.** A triangle whose vertices are all unmoved is the ground's own
//! surface (the base edges are constraints), so the ground stays exact except
//! in the triangles straddling the toe. Those are split, whatever their error,
//! down to [`Refine::toe`]: the band beyond the toe where the ground may differ
//! within the tolerance (#65 locality). The toe as a breakline (T2) would close
//! it, but crossings that settle on existing vertices do not yet triangulate.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use super::{Faces, OutlineError, plan};
use crate::terrain::region::{RegionMesh, RegionVertex};

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
    /// beyond the toe (#65).
    pub toe: f64,
    /// Most vertices the refinement may add; past it the mesh is kept as it is
    /// and [`Refined::residual`] says how far it still is.
    pub budget: u32,
}

impl Refine {
    /// One centimetre, down to five-centimetre triangles, a ten-centimetre toe
    /// band, at most 200 000 added vertices.
    pub const DEFAULT: Self = Self {
        tolerance: 0.01,
        shortest: 0.05,
        toe: 0.1,
        budget: 200_000,
    };

    pub(super) fn valid(self) -> bool {
        self.tolerance.is_finite()
            && self.tolerance > 0.0
            && self.shortest.is_finite()
            && self.shortest > 0.0
            && self.toe.is_finite()
            && self.toe >= self.shortest
    }
}

/// What the refinement did.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Refined {
    /// Vertices it added.
    pub added: u32,
    /// Rebuilds it took (the first build included).
    pub rounds: u32,
    /// Largest sampled error left, metres, over triangles still large enough to split.
    pub residual: f64,
    /// Whether it stopped at its budget, its round cap or a refused rebuild
    /// rather than at the tolerance.
    pub exhausted: bool,
}

/// Rebuilds the refinement may take before it reports itself exhausted.
const MAX_ROUNDS: u32 = 24;

/// The faces' heights on a rebuilt mesh.
pub(super) struct Graded {
    pub points: Vec<f64>,
    pub owners: Vec<bool>,
    pub based: Vec<f64>,
    pub unreached: u32,
}

/// The faces evaluated once per plan point. Each round rebuilds the mesh, but
/// most of its vertices and triangles are the last round's.
#[derive(Default)]
pub(super) struct Memo {
    heights: HashMap<[u64; 2], Option<(f64, bool)>>,
    errors: HashMap<[[u64; 2]; 3], f64>,
    /// Midpoints already inserted as points. One asked for again lies on a
    /// side that point did not split: a constraint read as free (a sliver's
    /// centroid can fall in the base triangle across its base edge).
    midpoints: BTreeSet<[u64; 2]>,
}

impl Memo {
    fn height(&mut self, faces: &Faces<'_>, x: f64, y: f64) -> Option<(f64, bool)> {
        *self
            .heights
            .entry([x.to_bits(), y.to_bits()])
            .or_insert_with(|| faces.height(x, y))
    }

    /// [`sampled_error`] of the triangle, by its corners' plan positions (its
    /// corners' heights follow from them).
    fn error(&mut self, faces: &Faces<'_>, corners: [[f64; 3]; 3]) -> f64 {
        let mut id = corners.map(|[x, y, _]| [x.to_bits(), y.to_bits()]);
        id.sort_unstable();
        *self
            .errors
            .entry(id)
            .or_insert_with(|| sampled_error(faces, corners))
    }
}

/// Every vertex of an outside triangle keeps its base height bit for bit;
/// every other vertex takes the design height.
pub(super) fn grade(
    faces: &Faces<'_>,
    memo: &mut Memo,
    mesh: &RegionMesh,
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
    };
    for (&[x, y, z], &keep) in vertices.iter().zip(&fixed) {
        let height = if keep {
            z
        } else {
            let (height, short) = memo.height(faces, x, y).ok_or(OutlineError::Coverage)?;
            graded.unreached += u32::from(short);
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
}

/// The region rebuilt from `spots` and `lines`, graded, and refined to
/// `bound` when there is one.
pub(super) fn refined(
    faces: &Faces<'_>,
    rebuild: impl Fn(&[f64], &[Vec<f64>]) -> Result<RegionMesh, OutlineError>,
    mut spots: Vec<f64>,
    mut lines: Vec<Vec<f64>>,
    bound: Option<Bound>,
) -> Result<(RegionMesh, Graded, Refined), OutlineError> {
    let mut mesh = rebuild(&spots, &lines)?;
    let initial = mesh.points.len() / 3;
    let mut report = Refined::default();
    let mut memo = Memo::default();
    loop {
        let graded = grade(faces, &mut memo, &mesh)?;
        report.rounds += 1;
        let Some(bound) = bound else {
            return Ok((mesh, graded, report));
        };
        let refine = bound.refine;
        let found = splits(faces, &mut memo, &mesh, &graded, bound);
        report.residual = found.residual;
        if found.is_empty() || report.added >= refine.budget || report.rounds > MAX_ROUNDS {
            report.exhausted = !found.is_empty();
            return Ok((mesh, graded, report));
        }
        for [x, y] in &found.spots {
            spots.extend([*x, *y, 0.0]);
        }
        // Corner-to-corner lines first; a round they do not triangulate is
        // retried with the short ones, and a round neither does ends it.
        let next = [0, 1].into_iter().find_map(|choice| {
            let all: Vec<Vec<f64>> = lines
                .iter()
                .cloned()
                .chain(
                    found
                        .lines
                        .iter()
                        .filter_map(|pair| pair.get(choice).cloned()),
                )
                .collect();
            rebuild(&spots, &all).ok().map(|built| (built, all))
        });
        let Some((built, all)) = next else {
            report.exhausted = true;
            return Ok((mesh, graded, report));
        };
        // A round that adds no vertex would repeat itself.
        if built.points.len() <= mesh.points.len() {
            report.exhausted = true;
            return Ok((mesh, graded, report));
        }
        mesh = built;
        lines = all;
        report.added =
            u32::try_from((mesh.points.len() / 3).saturating_sub(initial)).unwrap_or(u32::MAX);
    }
}

/// Where each triangle is sampled, as weights of its corners: its centroid and
/// the centroids of the three corner sub-triangles, all strictly inside it.
const SAMPLES: [[f64; 3]; 4] = [
    [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
    [2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0],
    [1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0],
    [1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0],
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

/// The largest gap between the faces and the triangle's plane at its samples.
fn sampled_error(faces: &Faces<'_>, corners: [[f64; 3]; 3]) -> f64 {
    SAMPLES.iter().fold(0.0_f64, |worst, &weights| {
        let [x, y, z] = mix(weights, corners);
        faces
            .height(x, y)
            .map_or(worst, |(height, _)| worst.max((height - z).abs()))
    })
}

/// The vertices one refinement round adds.
#[derive(Default)]
struct Splits {
    /// Isolated points: midpoints of free sides, centroids beside the ring.
    spots: Vec<[f64; 2]>,
    /// Per constrained side, its corner-to-corner line and its short line.
    lines: Vec<[Vec<f64>; 2]>,
    /// Largest sampled error over the triangles split.
    residual: f64,
}

impl Splits {
    fn is_empty(&self) -> bool {
        self.spots.is_empty() && self.lines.is_empty()
    }
}

fn key(a: u32, b: u32) -> [u32; 2] {
    if a < b { [a, b] } else { [b, a] }
}

fn plan_length(p: [f64; 3], q: [f64; 3]) -> f64 {
    (q[0] - p[0]).hypot(q[1] - p[1])
}

/// What a round needs to know about the rebuilt mesh beyond its triangles.
struct Topology<'a> {
    mesh: &'a RegionMesh,
    corners: &'a [[f64; 3]],
    /// The triangles on each side.
    sides: BTreeMap<[u32; 2], Vec<usize>>,
    /// Sides on a breakline or support.
    lines: BTreeSet<[u32; 2]>,
    /// Vertices where two constraints cross.
    crossings: BTreeSet<u32>,
    /// The ring's plan positions, bitwise.
    ring: BTreeSet<[u64; 2]>,
}

impl<'a> Topology<'a> {
    fn new(mesh: &'a RegionMesh, corners: &'a [[f64; 3]]) -> Self {
        let mut sides = BTreeMap::<[u32; 2], Vec<usize>>::new();
        for (index, &[a, b, c]) in mesh.triangles.as_chunks::<3>().0.iter().enumerate() {
            for (p, q) in [(a, b), (b, c), (c, a)] {
                sides.entry(key(p, q)).or_default().push(index);
            }
        }
        let first = mesh.input_vertices as usize + mesh.split_vertices.len();
        Self {
            mesh,
            corners,
            sides,
            lines: mesh
                .line_edges
                .iter()
                .map(|edge| key(edge.vertices[0], edge.vertices[1]))
                .collect(),
            crossings: (0_usize..)
                .zip(&mesh.new_vertices)
                .filter(|(_, origin)| matches!(origin, RegionVertex::Crossing(_)))
                .filter_map(|(index, _)| u32::try_from(first + index).ok())
                .collect(),
            ring: mesh
                .ring
                .iter()
                .filter_map(|&v| corners.get(v as usize))
                .map(|r| [r[0].to_bits(), r[1].to_bits()])
                .collect(),
        }
    }

    fn corner(&self, vertex: u32) -> Option<[f64; 3]> {
        self.corners.get(vertex as usize).copied()
    }

    fn triangle(&self, index: usize) -> Option<[u32; 3]> {
        self.mesh.triangles.as_chunks::<3>().0.get(index).copied()
    }

    /// The inside triangle across `side` from `index`; none at the ring.
    fn across(&self, side: [u32; 2], index: usize) -> Option<usize> {
        self.sides
            .get(&side)?
            .iter()
            .copied()
            .find(|&other| other != index)
            .filter(|&other| self.mesh.inside.get(other).copied().unwrap_or(false))
    }

    /// A base edge (the two triangles lie in different base triangles) or a line.
    fn constrained(&self, side: [u32; 2], index: usize, other: usize) -> bool {
        self.lines.contains(&side) || self.mesh.parents.get(index) != self.mesh.parents.get(other)
    }

    /// A vertex a line may start at: exact, not a crossing rounded off the
    /// constraints it lies on (a line from it would cross them again a rounding
    /// away), and not on the ring, which takes no line's end.
    fn clean(&self, vertex: u32) -> bool {
        !self.crossings.contains(&vertex)
            && self
                .corner(vertex)
                .is_some_and(|r| !self.ring.contains(&[r[0].to_bits(), r[1].to_bits()]))
    }

    /// The lines that cross the constrained `side` between the triangles with
    /// opposite corners `far` and `apex`.
    fn crossing(&self, side: [u32; 2], far: u32, apex: u32) -> Option<[Vec<f64>; 2]> {
        let (start, end) = (self.corner(side[0])?, self.corner(side[1])?);
        let (p, r) = (self.corner(far)?, self.corner(apex)?);
        let mid = [start[0].midpoint(end[0]), start[1].midpoint(end[1])];
        let toward = |q: [f64; 3]| {
            [
                0.25f64.mul_add(q[0] - mid[0], mid[0]),
                0.25f64.mul_add(q[1] - mid[1], mid[1]),
            ]
        };
        let ([x0, y0], [x1, y1]) = (toward(p), toward(r));
        let short = vec![x0, y0, 0.0, x1, y1, 0.0];
        // The corner-to-corner line crosses only this side where the two
        // triangles make a convex quadrilateral; near its middle, it bisects it.
        let side_of =
            |q: [f64; 3]| (r[0] - p[0]).mul_add(q[1] - p[1], -(r[1] - p[1]) * (q[0] - p[0]));
        let (before, after) = (side_of(start), side_of(end));
        let along = before / (before - after);
        let line = if before * after < 0.0
            && (0.25..=0.75).contains(&along)
            && self.clean(far)
            && self.clean(apex)
        {
            vec![p[0], p[1], 0.0, r[0], r[1], 0.0]
        } else {
            short.clone()
        };
        Some([line, short])
    }
}

/// Longest-side bisection of every inside triangle still large enough to split
/// whose plane misses the faces by more than the tolerance at a sample (and by
/// no more than a continuous field could), or that straddles the toe and is
/// wider than its band. Ring sides are not candidates: a triangle against the ring where a face ends in
/// a step is split towards it until its other sides are too short.
fn splits(
    faces: &Faces<'_>,
    memo: &mut Memo,
    mesh: &RegionMesh,
    graded: &Graded,
    bound: Bound,
) -> Splits {
    let Bound { refine, steepest } = bound;
    let corners = plan::vertices(&graded.points);
    let moved = |v: u32| graded.owners.get(v as usize).copied().unwrap_or(false);
    let topology = Topology::new(mesh, corners);
    let mut out = Splits::default();
    let mut taken = BTreeSet::new();
    for (index, _) in mesh
        .inside
        .iter()
        .enumerate()
        .filter(|(_, inside)| **inside)
    {
        let Some(vertices) = topology.triangle(index) else {
            continue;
        };
        let [Some(pa), Some(pb), Some(pc)] = vertices.map(|v| topology.corner(v)) else {
            continue;
        };
        let [va, vb, vc] = vertices;
        // The longest side with an inside triangle across, its two vertices,
        // the one opposite, and that triangle.
        let mut candidates = [
            (va, vb, vc, pa, pb),
            (vb, vc, va, pb, pc),
            (vc, va, vb, pc, pa),
        ];
        candidates.sort_by(|x, y| plan_length(y.3, y.4).total_cmp(&plan_length(x.3, x.4)));
        let Some((from, to, far, start, end, other)) =
            candidates
                .into_iter()
                .find_map(|(from, to, far, start, end)| {
                    let other = topology.across(key(from, to), index)?;
                    Some((from, to, far, start, end, other))
                })
        else {
            continue;
        };
        if plan_length(start, end) < refine.shortest {
            continue;
        }
        let error = memo.error(faces, [pa, pb, pc]);
        let straddles = !(moved(va) == moved(vb) && moved(vb) == moved(vc));
        // Within a triangle of side `h` a field no steeper than `steepest`
        // strays from the plane through its corners by at most twice
        // `steepest * h` (and the at-grade snap): more is a step, where the
        // faces end at their limit or the ring, reported and not refined.
        let span = plan_length(pa, pb)
            .max(plan_length(pb, pc))
            .max(plan_length(pc, pa));
        let step = 2.0f64.mul_add(steepest * span, 2.0 * faces.grading.tolerance);
        let across_toe = straddles && plan_length(start, end) > refine.toe;
        if error > step || (error <= refine.tolerance && !across_toe) {
            continue;
        }
        out.residual = out.residual.max(error);
        let side = key(from, to);
        if !taken.insert(side) {
            continue;
        }
        let mid = [start[0].midpoint(end[0]), start[1].midpoint(end[1])];
        let repeated = memo
            .midpoints
            .contains(&[mid[0].to_bits(), mid[1].to_bits()]);
        if !repeated && !topology.constrained(side, index, other) {
            memo.midpoints.insert([mid[0].to_bits(), mid[1].to_bits()]);
            out.spots.push(mid);
            continue;
        }
        let apex = topology
            .triangle(other)
            .and_then(|t| t.into_iter().find(|&v| v != from && v != to));
        if let Some(lines) = apex.and_then(|apex| topology.crossing(side, far, apex)) {
            out.lines.push(lines);
        }
    }
    out
}
