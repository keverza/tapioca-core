//! Rounded creases between faces (#149): where two outline edges' faces meet
//! at an inside corner, the envelope takes the higher fill (lower cut) and
//! has a crease there, and every contour a sharp corner. The winning face is
//! lifted towards its rival by a quadratic smooth maximum whose knee is the
//! difference of their gradients times the section's rounding: the crease
//! rounds over about a rounding in plan whatever its angle, and the slope, a
//! mix of the two gradients, stays within the grade. Head on, at 1 in 3 and a
//! 1 m rounding, the lift is at most about 16 cm.
//!
//! The lift acts only at a crease. Where one face falls from a vertex the
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
    /// Height, negated for a cut face.
    pub height: f64,
    /// Plan unit vector from the edge's steepest point to the point: the way
    /// the face falls (zero on the edge itself).
    pub toward: [f64; 2],
    /// The edge's end the face falls from, bitwise in plan; `None` when it
    /// falls from a point inside the edge.
    pub from: Option<[u64; 2]>,
    /// The edge's two ends, bitwise in plan.
    pub ends: [[u64; 2]; 2],
    /// Plan distance from that steepest point.
    pub distance: f64,
    /// The face's gradient.
    pub grade: f64,
    /// Its rounding, metres.
    pub rounding: f64,
}

/// Directions this far apart, radians, make no crease.
const FLAT: f64 = std::f64::consts::PI / 18.0;
/// Directions this far apart, radians, make a full crease.
const CREASE: f64 = std::f64::consts::FRAC_PI_4;

/// An edge's two ends, bitwise in plan.
pub(super) fn ends([from, to]: [[f64; 3]; 2]) -> [[u64; 2]; 2] {
    [
        [from[0].to_bits(), from[1].to_bits()],
        [to[0].to_bits(), to[1].to_bits()],
    ]
}

/// The end of `segment` a face falls from to `(px, py)`: the end at its
/// steepest point, found back along `toward` over `distance`; `None` when that
/// point is inside the edge.
pub(super) fn joint(
    segment: [[f64; 3]; 2],
    inside: bool,
    toward: &[f64; 2],
    distance: f64,
    [px, py]: [f64; 2],
) -> Option<[u64; 2]> {
    if inside {
        return None;
    }
    let (x, y) = (px - toward[0] * distance, py - toward[1] * distance);
    let [from, to] = segment;
    let nearer = if (from[0] - x).hypot(from[1] - y) <= (to[0] - x).hypot(to[1] - y) {
        from
    } else {
        to
    };
    Some([nearer[0].to_bits(), nearer[1].to_bits()])
}

fn smoothstep(t: f64) -> f64 {
    let t = t.clamp(0.0, 1.0);
    t * t * 2.0f64.mul_add(-t, 3.0)
}

/// How far the winning face is lifted towards `rival`.
fn lift(winner: &Face, rival: &Face) -> f64 {
    let rounding = winner.rounding.min(rival.rounding);
    let joint = |a: &Face, b: &Face| a.from.is_some_and(|end| b.ends.contains(&end));
    if rounding <= 0.0 || joint(winner, rival) || joint(rival, winner) {
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
    let knee = difference * rounding * crease * fade;
    let gap = winner.height - rival.height;
    if knee <= 0.0 || gap >= knee {
        return 0.0;
    }
    (knee - gap).powi(2) / (4.0 * knee)
}

/// The highest face among `faces` that `keep`, lifted at its creases: the
/// face and its lifted height. `None` when none is kept.
pub(super) fn envelope(faces: &[Face], keep: impl Fn(&Face) -> bool) -> Option<(Face, f64)> {
    let winner = faces
        .iter()
        .filter(|face| keep(face))
        .max_by(|a, b| a.height.total_cmp(&b.height))?;
    let lifted = faces
        .iter()
        .filter(|face| keep(face))
        .map(|rival| lift(winner, rival))
        .fold(0.0, f64::max);
    Some((*winner, winner.height + lifted))
}
