//! #149 T2: the faces' creases (ridges and valleys) as breaklines.
//!
//! Where two outline edges' faces meet, the envelope takes the higher fill (the
//! lower cut) and the design surface folds along the line where they tie: a
//! valley or a ridge, sharp without a rounding and the middle of the blend with
//! one ([`super::blend`]). Between offset rings the mesh interpolated across it
//! like it did across the toe. Here the line is traced from the faces the mesh
//! is graded with ([`Faces::winner`]) and handed to the rebuild as breaklines.
//!
//! **Which ties are creases.** Only those [`Faces::creased`] accepts: the two
//! faces fall at least the blend's flat angle apart and neither falls from a
//! vertex the other's edge ends at. That leaves out the joints between pieces of
//! one straight or finely curved outline, and the round cone at an outside
//! corner, which meets both planes smoothly.
//!
//! **Tracing.** The toe's grid ([`super::trace`]): each node is labelled with the
//! edge whose face moves the ground there; the fine grid is evaluated only in
//! the coarse cells whose corners two edges' faces on one side win. A cell edge
//! whose ends two faces win is bisected to where the first stops winning; a cell
//! with two creased edges joins them, one with three or four (where three faces
//! meet) joins each to the mean of its crossings. A crease ends a cell short of
//! the toe or the outline, and where it stops being one.
//!
//! Plan metres; the lines' z is zero (a breakline's height is the design's).

use std::collections::{BTreeMap, BTreeSet};

use super::trace::{Clearance, Edge, Grid, chains, parallel, push_piece, simplify};
use super::{Faces, Winner};
use crate::terrain::plan;

/// The faces at one node: the ground kept (or the pavement), or moved by one edge's face.
#[derive(Clone, Copy, Debug)]
enum Label {
    Kept,
    Moved(Winner),
}

impl Label {
    const fn winner(self) -> Option<Winner> {
        match self {
            Self::Kept => None,
            Self::Moved(winner) => Some(winner),
        }
    }
}

/// Two samples two different edges' faces on one side win, falling at least half the blend's
/// flat angle apart: that side and the two edges. Between two nodes a step apart the faces of
/// a smooth outline (pieces of one curve, a round cone) turn by less; across a crease by its
/// whole angle. Only these are refined and bisected: along the paths' finely sampled curves
/// two edges win either side of nearly every cell edge.
fn rivals(a: Option<Label>, b: Option<Label>) -> Option<(bool, [u32; 2])> {
    let (first, second) = (a?.winner()?, b?.winner()?);
    let along = first.toward[0].mul_add(second.toward[0], first.toward[1] * second.toward[1]);
    (first.cut == second.cut
        && first.index != second.index
        && along.clamp(-1.0, 1.0).acos() >= super::blend::FLAT / 2.0)
        .then_some((first.cut, [first.index, second.index]))
}

/// The edge and side whose face wins a sample, if one does.
fn won(label: Option<Label>) -> Option<(bool, u32)> {
    label?.winner().map(|winner| (winner.cut, winner.index))
}

/// A crossing's key: a cell edge, or the centre of a cell where three or more creases meet.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Key {
    Edge(Edge),
    Centre(u32, u32),
}

/// The creases of `faces` over `ground` inside `region` (closed xyz ring), clear of
/// `obstacles` (open xyz polylines, the toe among them), `spots` (xyz) and the ring;
/// sampled every `step` metres, `clear` metres off what exists, simplified to `tolerance`.
/// `kept` says, where it knows, whether the design keeps the ground at a node.
#[expect(
    clippy::too_many_arguments,
    reason = "the faces, their ground, the ring, everything to keep clear of and the tracing settings are one input"
)]
pub(super) fn lines(
    faces: &Faces<'_>,
    ground: &(dyn Fn(f64, f64) -> Option<f64> + Sync),
    region: &[f64],
    obstacles: &[&[f64]],
    spots: &[f64],
    kept: &(dyn Fn(f64, f64) -> Option<bool> + Sync),
    step: f64,
    clear: f64,
    tolerance: f64,
) -> Vec<Vec<f64>> {
    let ring = plan::vertices(region);
    if ring.len() < 3 || step.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return Vec::new();
    }
    let evaluate = |x: f64, y: f64| -> Option<Label> {
        if plan::inside(ring, x, y) != Some(true) {
            return None;
        }
        if kept(x, y) == Some(true) {
            return Some(Label::Kept);
        }
        let base = ground(x, y)?;
        Some(faces.winner(base, x, y).map_or(Label::Kept, Label::Moved))
    };
    let mut grid = Grid::new(ring, step, &evaluate);
    let cells = grid.active(&|[sw, se, ne, nw]| {
        [(sw, se), (se, ne), (ne, nw), (nw, sw), (sw, ne), (se, nw)]
            .iter()
            .any(|&(a, b)| rivals(a, b).is_some())
    });
    // Every cell edge two faces win, its crossing bisected and kept where it is a crease.
    let candidates: Vec<Edge> = cells
        .iter()
        .flat_map(|&(i, j)| {
            [
                Edge::H(i, j),
                Edge::V(i + 1, j),
                Edge::H(i, j + 1),
                Edge::V(i, j),
            ]
        })
        .collect::<BTreeSet<_>>()
        .into_iter()
        .filter(|&edge| {
            let ((ia, ja), (ib, jb)) = edge.ends();
            rivals(grid.at(ia, ja), grid.at(ib, jb)).is_some()
        })
        .collect();
    let found = parallel(&candidates, &|&edge| {
        let ((ia, ja), (ib, jb)) = edge.ends();
        let (cut, pair) = rivals(grid.at(ia, ja), grid.at(ib, jb))?;
        let point = grid.bisect(edge, &|start, mid| won(mid) == won(Some(start)))?;
        let base = ground(point[0], point[1])?;
        faces.creased(base, point, cut, pair).then_some(point)
    });
    let mut points: BTreeMap<Key, [f64; 2]> = candidates
        .iter()
        .zip(found)
        .filter_map(|(&edge, point)| point.map(|p| (Key::Edge(edge), p)))
        .collect();
    let mut links: BTreeMap<Key, Vec<Key>> = BTreeMap::new();
    let mut link = |p: Key, q: Key| {
        links.entry(p).or_default().push(q);
        links.entry(q).or_default().push(p);
    };
    for &(i, j) in &cells {
        let creased: Vec<Key> = [
            Edge::H(i, j),
            Edge::V(i + 1, j),
            Edge::H(i, j + 1),
            Edge::V(i, j),
        ]
        .into_iter()
        .map(Key::Edge)
        .filter(|key| points.contains_key(key))
        .collect();
        match creased.as_slice() {
            [p, q] => link(*p, *q),
            [_, _, _, ..] => {
                let crossings: Vec<[f64; 2]> = creased
                    .iter()
                    .filter_map(|key| points.get(key).copied())
                    .collect();
                let count = f64::from(u32::try_from(crossings.len()).unwrap_or(u32::MAX));
                let centre = crossings
                    .iter()
                    .fold([0.0, 0.0], |[x, y], p| [x + p[0] / count, y + p[1] / count]);
                let key = Key::Centre(i, j);
                points.insert(key, centre);
                for &p in &creased {
                    link(p, key);
                }
            }
            _ => {}
        }
    }
    // Each crease clear of what exists and of the creases kept before it: where creases meet,
    // the later ones stop short of the junction rather than share its vertex.
    let mut out: Vec<Vec<f64>> = Vec::new();
    let existing = Clearance::new(obstacles, spots, ring, clear, clear);
    for mut line in chains(&points, &links) {
        // The crease runs on to the toe or the pavement: a cell short of it, the last
        // triangle would bridge the fold.
        extend(&mut line, &evaluate, step, clear);
        // The creases kept so far are few: their own small index, rebuilt per crease.
        let kept: Vec<&[f64]> = out.iter().map(Vec::as_slice).collect();
        let creases = Clearance::new(&kept, &[], &[], clear, clear);
        let mut found = Vec::new();
        for piece in existing.split(&line) {
            for piece in creases.split(&piece) {
                push_piece(&simplify(&piece, tolerance), clear, &mut found);
            }
        }
        out.extend(found);
    }
    out
}

/// Each open end of `line` carried straight on to `clear` before the ground is kept (the
/// toe, or the pavement), when that is within two grid steps; a crease of two planes is
/// straight, and one of two cones nearly so over that run.
fn extend(
    line: &mut Vec<[f64; 2]>,
    evaluate: &dyn Fn(f64, f64) -> Option<Label>,
    step: f64,
    clear: f64,
) {
    let moved = |p: [f64; 2]| won(evaluate(p[0], p[1])).is_some();
    let closed = matches!((line.first(), line.last()), (Some(a), Some(b)) if a.map(f64::to_bits) == b.map(f64::to_bits));
    if line.len() < 2 || closed {
        return;
    }
    let reach = |end: [f64; 2], before: [f64; 2]| -> Option<[f64; 2]> {
        let (dx, dy) = (end[0] - before[0], end[1] - before[1]);
        let length = dx.hypot(dy);
        if length <= 0.0 {
            return None;
        }
        let (ux, uy) = (dx / length, dy / length);
        let at = |s: f64| [ux.mul_add(s, end[0]), uy.mul_add(s, end[1])];
        if !moved(end) || moved(at(2.0 * step)) {
            return None;
        }
        let (mut lo, mut hi) = (0.0_f64, 2.0 * step);
        for _ in 0..super::trace::BISECT {
            let mid = lo.midpoint(hi);
            if moved(at(mid)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        let short = lo - 1.5 * clear;
        (short > clear).then(|| at(short))
    };
    if let (Some(&end), Some(&before)) = (line.last(), line.iter().rev().nth(1))
        && let Some(point) = reach(end, before)
    {
        line.push(point);
    }
    if let (Some(&end), Some(&before)) = (line.first(), line.get(1))
        && let Some(point) = reach(end, before)
    {
        line.insert(0, point);
    }
}
