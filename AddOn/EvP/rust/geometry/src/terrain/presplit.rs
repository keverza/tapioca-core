//! Pre-splitting of properly crossing constraint segments.
//!
//! `spade` accepts only constraints that touch at vertices, so every proper
//! crossing (interiors intersect at one point) gets a vertex before
//! triangulation. The constraint builder resolves exact touching vertices on
//! each original segment before inserting its rounded crossing pieces, using
//! `spade`'s exact predicates. A crossing classified here in plain `f64` but not by
//! the exact predicates is reported by the triangulator as a conflict, never
//! silently skipped.

use std::cmp::Ordering;

use super::input::{Prepared, Segment};
use super::{Crossing, Error, Role};

/// A crossing before its vertex is allocated.
struct Found {
    segments: [usize; 2],
    position: [f64; 3],
    parameters: [f64; 2],
}

/// Every segment's vertex chain (start, crossings by parameter, end) plus the
/// crossing records, in `(first segment, second segment)` order.
pub(super) struct Split {
    pub pieces: Vec<Vec<u32>>,
    pub crossings: Vec<Crossing>,
}

pub(super) fn presplit(prepared: &mut Prepared) -> Result<Split, Error> {
    let found = find_crossings(prepared)?;
    let mut splits: Vec<Vec<(f64, u32)>> = vec![Vec::new(); prepared.segments.len()];
    let mut crossings = Vec::with_capacity(found.len());
    for crossing in found {
        let vertex = prepared.vertex_at(crossing.position)?;
        let refs = crossing
            .segments
            .map(|index| segment(prepared, index).map(|s| s.id));
        let [first, second] = [refs[0]?, refs[1]?];
        for (index, parameter) in crossing.segments.into_iter().zip(crossing.parameters) {
            splits
                .get_mut(index)
                .ok_or(Error::TriangulationDegenerate)?
                .push((parameter, vertex));
        }
        crossings.push(Crossing {
            vertex,
            segments: [first, second],
        });
    }
    let pieces = prepared
        .segments
        .iter()
        .zip(&mut splits)
        .map(|(segment, inner)| {
            inner.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)));
            let mut chain = vec![segment.ends[0]];
            chain.extend(inner.iter().map(|&(_, vertex)| vertex));
            chain.push(segment.ends[1]);
            chain.dedup();
            chain
        })
        .collect();
    Ok(Split { pieces, crossings })
}

fn segment(prepared: &Prepared, index: usize) -> Result<&Segment, Error> {
    prepared
        .segments
        .get(index)
        .ok_or(Error::TriangulationDegenerate)
}

/// Sweep segments by minimum x and test overlapping bounding boxes.
fn find_crossings(prepared: &Prepared) -> Result<Vec<Found>, Error> {
    let ends = prepared
        .segments
        .iter()
        .map(|segment| {
            Ok([
                prepared.position(segment.ends[0])?,
                prepared.position(segment.ends[1])?,
            ])
        })
        .collect::<Result<Vec<_>, Error>>()?;
    let mut order: Vec<usize> = (0..ends.len()).collect();
    order.sort_by(|&a, &b| min_x(&ends, a).total_cmp(&min_x(&ends, b)).then(a.cmp(&b)));
    let mut found = Vec::new();
    for (rank, &first) in order.iter().enumerate() {
        let limit = max_x(&ends, first);
        for &second in order.iter().skip(rank + 1) {
            if min_x(&ends, second) > limit {
                break;
            }
            let pair = if first < second {
                [first, second]
            } else {
                [second, first]
            };
            if let Some(crossing) = crossing(prepared, &ends, pair)? {
                found.push(crossing);
            }
        }
    }
    found.sort_by_key(|crossing| crossing.segments);
    Ok(found)
}

fn min_x(ends: &[[[f64; 3]; 2]], index: usize) -> f64 {
    ends.get(index)
        .map_or(f64::INFINITY, |[a, b]| a[0].min(b[0]))
}

fn max_x(ends: &[[[f64; 3]; 2]], index: usize) -> f64 {
    ends.get(index)
        .map_or(f64::NEG_INFINITY, |[a, b]| a[0].max(b[0]))
}

/// A proper crossing of two segments that share no vertex, with heights.
///
/// The point is placed on the lower-indexed segment. Both segments' linearly
/// interpolated heights must agree within the request's z tolerance; the lower
/// segment's height is kept. Two crossing rings refuse: a ring that crosses a
/// ring (or itself) has no consistent inside.
fn crossing(
    prepared: &Prepared,
    ends: &[[[f64; 3]; 2]],
    pair: [usize; 2],
) -> Result<Option<Found>, Error> {
    let [first, second] = pair.map(|index| segment(prepared, index));
    let (first, second) = (first?, second?);
    if first.ends.iter().any(|end| second.ends.contains(end)) {
        return Ok(None);
    }
    let ([p0, p1], [q0, q1]) = match (ends.get(pair[0]), ends.get(pair[1])) {
        (Some(left), Some(right)) => (*left, *right),
        _ => return Err(Error::TriangulationDegenerate),
    };
    if !(opposite(orient(p0, p1, q0), orient(p0, p1, q1))
        && opposite(orient(q0, q1, p0), orient(q0, q1, p1)))
    {
        return Ok(None);
    }
    if first.role != Role::Breakline && second.role != Role::Breakline {
        return Err(Error::HoleClassificationFailed);
    }
    let denominator = cross(sub(p1, p0), sub(q1, q0));
    let along_first = cross(sub(q0, p0), sub(q1, q0)) / denominator;
    let along_second = cross(sub(q0, p0), sub(p1, p0)) / denominator;
    if !(along_first.is_finite() && along_second.is_finite()) {
        return Err(Error::TriangulationConstraint {
            constraint: second.id.constraint,
        });
    }
    let along_first = along_first.clamp(0.0, 1.0);
    let along_second = along_second.clamp(0.0, 1.0);
    let z_first = p0[2] + along_first * (p1[2] - p0[2]);
    let z_second = q0[2] + along_second * (q1[2] - q0[2]);
    if (z_first - z_second).abs() > prepared.z_tolerance() {
        return Err(Error::TriangulationConstraint {
            constraint: second.id.constraint,
        });
    }
    Ok(Some(Found {
        segments: pair,
        position: [
            p0[0] + along_first * (p1[0] - p0[0]),
            p0[1] + along_first * (p1[1] - p0[1]),
            z_first,
        ],
        parameters: [along_first, along_second],
    }))
}

fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 2] {
    [a[0] - b[0], a[1] - b[1]]
}

fn cross(a: [f64; 2], b: [f64; 2]) -> f64 {
    a[0].mul_add(b[1], -(a[1] * b[0]))
}

/// Sign of the plain `f64` orientation; zero (either sign) is collinear.
fn orient(a: [f64; 3], b: [f64; 3], c: [f64; 3]) -> Ordering {
    let value = cross(sub(b, a), sub(c, a));
    if value > 0.0 {
        Ordering::Greater
    } else if value < 0.0 {
        Ordering::Less
    } else {
        Ordering::Equal
    }
}

fn opposite(left: Ordering, right: Ordering) -> bool {
    matches!(
        (left, right),
        (Ordering::Less, Ordering::Greater) | (Ordering::Greater, Ordering::Less)
    )
}
