//! Rounded creases between faces (#149): where two outline edges' faces meet
//! at an inside corner, the envelope takes the higher fill (lower cut) and
//! has a crease there, and every contour a sharp corner. The winning face is
//! lifted towards its rival by a quadratic smooth maximum whose knee is the
//! difference of their gradients times the section's rounding: the crease
//! rounds over about a rounding in plan whatever its angle, and the slope, a
//! mix of the two gradients, stays within the grade. Head on, at 1 in 3 and a
//! 1 m rounding, the lift is at most about 16 cm.
//!
//! The lift acts only at a crease. Between pieces of one straight edge there is none, however
//! far apart (see [`collinear`]). Where one face falls from a vertex the
//! other's edge also ends at (a joint of two pieces of the outline), that
//! round cone meets the other's plane smoothly though its direction differs;
//! a lift there would raise a bump at every outline vertex, so there is none.
//! Otherwise it scales with how differently the two faces fall, from nothing
//! below [`FLAT`] (the faint creases between pieces of a finely sampled curve)
//! to all of it at [`CREASE`]. It also fades in over the faces' rounded tops, from
//! nothing at either face's edge to all of it a rounding out: at the outline
//! the faces meet the pavement exactly.

/// One edge's face at a point, as the higher-wins envelope sees it (a cut
/// face negated).
#[derive(Clone, Copy, Debug)]
pub(super) struct Face {
    /// The edge's index in the outline: the higher wins a tie, as a scan in
    /// outline order keeps the last of equal maxima.
    pub index: u32,
    /// Height, negated for a cut face.
    pub height: f64,
    /// The outline edge's height at the steepest point, before the top rounding; not negated.
    /// This caps the toe's vertical blend where the whole face is lower than its rounding (#170).
    pub edge: f64,
    /// Plan unit vector from the edge's steepest point to the point: the way
    /// the face falls (zero on the edge itself).
    pub toward: [f64; 2],
    /// The edge, and whether the face falls from a point inside it (else
    /// from its end nearer `foot`, the steepest point).
    pub segment: [[f64; 3]; 2],
    pub inside: bool,
    pub foot: [f64; 2],
    /// Plan distance from that steepest point.
    pub distance: f64,
    /// The face's gradient.
    pub grade: f64,
    /// Its rounding, metres.
    pub rounding: f64,
}

/// Directions this far apart, radians, make no crease.
pub(super) const FLAT: f64 = std::f64::consts::PI / 18.0;
/// Directions this far apart, radians, make a full crease.
const CREASE: f64 = std::f64::consts::FRAC_PI_4;

/// Whether the two faces fall from pieces of one straight edge: both edges on one plan line,
/// with the same grade and rounding. A piece further along it falls from its end, a direction
/// that differs from the winner's by more than [`FLAT`] seen from beside the edge, yet its face
/// is the same line's: there is no crease between them, and a lift there would raise every
/// finely split straight edge by how finely it was split (#175).
fn collinear(a: &Face, b: &Face) -> bool {
    if a.grade.to_bits() != b.grade.to_bits() || a.rounding.to_bits() != b.rounding.to_bits() {
        return false;
    }
    let [p, q] = a.segment;
    let (dx, dy) = (q[0] - p[0], q[1] - p[1]);
    let length = dx.hypot(dy);
    if length <= 0.0 {
        return false;
    }
    b.segment
        .iter()
        .all(|r| (dx.mul_add(r[1] - p[1], -dy * (r[0] - p[0]))).abs() / length <= ON_LINE)
}

/// A vertex within this of a line lies on it, metres.
const ON_LINE: f64 = 1e-6;

/// Whether `a` falls from an end of its edge that `b`'s edge also ends at.
pub(super) fn joint(a: &Face, b: &Face) -> bool {
    if a.inside {
        return false;
    }
    let [from, to] = a.segment;
    let [x, y] = a.foot;
    let end = if (from[0] - x).hypot(from[1] - y) <= (to[0] - x).hypot(to[1] - y) {
        from
    } else {
        to
    };
    let same =
        |p: [f64; 3]| p[0].to_bits() == end[0].to_bits() && p[1].to_bits() == end[1].to_bits();
    b.segment.iter().any(|&p| same(p))
}

fn smoothstep(t: f64) -> f64 {
    let t = t.clamp(0.0, 1.0);
    t * t * 2.0f64.mul_add(-t, 3.0)
}

/// How far the winning face is lifted towards `rival`.
fn lift(winner: &Face, rival: &Face) -> f64 {
    let rounding = winner.rounding.min(rival.rounding);
    let gap = winner.height - rival.height;
    // The knee is never more than the two grades together times the
    // rounding: a rival further below lends nothing, whatever its angle.
    if rounding <= 0.0 || gap >= (winner.grade + rival.grade) * rounding {
        return 0.0;
    }
    let knee = knee(winner, rival);
    if knee <= 0.0 || gap >= knee {
        return 0.0;
    }
    (knee - gap).powi(2) / (4.0 * knee)
}

/// The height difference below which `winner` is lifted towards `rival`: zero at a joint,
/// without a rounding, and where the two fall within [`FLAT`] of one way. It is symmetric in
/// the two faces, so the blend is `|gap| < knee` either side of their tie, and across it the
/// surface is a parabola in the gap (#149 T2 traces lines across it).
pub(super) fn knee(winner: &Face, rival: &Face) -> f64 {
    let rounding = winner.rounding.min(rival.rounding);
    if rounding <= 0.0 || joint(winner, rival) || joint(rival, winner) || collinear(winner, rival) {
        return 0.0;
    }
    let along = winner.toward[0].mul_add(rival.toward[0], winner.toward[1] * rival.toward[1]);
    let crease = smoothstep((along.clamp(-1.0, 1.0).acos() - FLAT) / (CREASE - FLAT));
    // Nothing at either face's edge, all of it once both are a rounding out:
    // symmetric, so the lift is the same whichever wins where they tie, and
    // smooth, where a minimum of the distances would crease it.
    let fade = smoothstep(winner.distance / rounding) * smoothstep(rival.distance / rounding);
    // The faces' gradients differ by this much; a knee of it times the
    // rounding blends them over about a rounding in plan, whatever the angle.
    let difference = winner
        .grade
        .mul_add(winner.toward[0], -rival.grade * rival.toward[0])
        .hypot(
            winner
                .grade
                .mul_add(winner.toward[1], -rival.grade * rival.toward[1]),
        );
    difference * rounding * crease * fade
}

/// The highest face among `faces` that `keep`, lifted at its creases: the
/// face and its lifted height. `None` when none is kept.
pub(super) fn envelope(faces: &[Face], keep: impl Fn(&Face) -> bool) -> Option<(Face, f64)> {
    let winner = faces
        .iter()
        .filter(|face| keep(face))
        .max_by(|a, b| a.height.total_cmp(&b.height).then(a.index.cmp(&b.index)))?;
    let lifted = faces
        .iter()
        .filter(|face| keep(face))
        .map(|rival| lift(winner, rival))
        .fold(0.0, f64::max);
    Some((*winner, winner.height + lifted))
}
