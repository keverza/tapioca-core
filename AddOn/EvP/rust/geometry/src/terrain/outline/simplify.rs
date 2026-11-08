//! The outline with its collinear runs merged, for the field preview (#175).
//!
//! The paved outline is split every half metre: the build grades each edge from its two
//! vertices, and the mesher and the toe tracing need the vertices that dense. A face over a
//! straight edge whose height runs linearly along it is the maximum of the faces over its pieces,
//! so for evaluating heights the pieces only cost: a point within the reach meets dozens of
//! them, each searched, faced and blended. Merged where they are collinear within a tolerance
//! (in plan and in height) and share their grade, a straight path side is one edge. The heights
//! differ from the dense outline's only by the tolerance and by the crease lift between pieces
//! that meet tangentially -- far below a preview grid's own interpolation. The build never uses
//! this.

use std::collections::HashMap;

use super::{Edge, Faces, Operator};

/// Plan and height tolerance a merged edge keeps its pieces' vertices within, metres.
pub(in crate::terrain) const PREVIEW_MERGE: f64 = 1e-3;

impl Operator {
    /// The outline with its collinear runs merged within `tolerance` (see the module).
    pub(in crate::terrain) fn merged_edges(&self, tolerance: f64) -> Vec<Edge> {
        merge(&self.edges, tolerance)
    }

    /// These faces over `edges` (the merged outline) instead of the outline itself.
    pub(in crate::terrain) fn faces_over<'a>(&'a self, edges: &'a [Edge]) -> Faces<'a> {
        Faces::new(&self.base, &self.surface, edges, self.grading)
    }
}

/// A plan point's exact key.
fn key(p: [f64; 3]) -> (u64, u64) {
    (p[0].to_bits(), p[1].to_bits())
}

/// `edges` chained through the vertices exactly two of them share, each chain's runs merged
/// while the grade is the same and every vertex between lies within `tolerance` of the run's
/// straight line, in plan and in height. A vertex more or fewer edges meet ends a chain.
fn merge(edges: &[Edge], tolerance: f64) -> Vec<Edge> {
    let mut at: HashMap<(u64, u64), Vec<usize>> = HashMap::with_capacity(edges.len() * 2);
    for (index, edge) in edges.iter().enumerate() {
        for end in edge.segment {
            at.entry(key(end)).or_default().push(index);
        }
    }
    let degree = |p: [f64; 3]| at.get(&key(p)).map_or(0, Vec::len);
    let mut used = vec![false; edges.len()];
    let mut out = Vec::with_capacity(edges.len());
    // Walk from every unused edge: first back to its chain's start (a vertex not of degree two,
    // or itself once round a loop), then forward, merging as it goes.
    for first in 0..edges.len() {
        if used.get(first).copied().unwrap_or(true) {
            continue;
        }
        let Some(start_edge) = edges.get(first) else {
            continue;
        };
        // Back to the start.
        let (mut edge, mut from) = (first, start_edge.segment[0]);
        let mut steps = 0_usize;
        while degree(from) == 2 && steps < edges.len() {
            let Some(previous) = at
                .get(&key(from))
                .and_then(|list| list.iter().copied().find(|&other| other != edge))
            else {
                break;
            };
            if previous == first {
                break; // a closed loop: start anywhere
            }
            let Some(prev_edge) = edges.get(previous) else {
                break;
            };
            from = if key(prev_edge.segment[0]) == key(from) {
                prev_edge.segment[1]
            } else {
                prev_edge.segment[0]
            };
            edge = previous;
            steps += 1;
        }
        // Forward from `from` along `edge`, merging runs.
        let mut run: Vec<[f64; 3]> = vec![from];
        let mut grade = None;
        while let Some(current) = edges.get(edge) {
            if used.get(edge).copied().unwrap_or(true) {
                break;
            }
            if let Some(flag) = used.get_mut(edge) {
                *flag = true;
            }
            let to = if key(current.segment[0]) == key(from) {
                current.segment[1]
            } else {
                current.segment[0]
            };
            let same = grade.is_none_or(|g| g == current.grade);
            run.push(to);
            if !same || !straight(&run, tolerance) {
                // Close the run before this piece; this piece starts the next.
                run.pop();
                if let (Some(&a), Some(&b), Some(g)) = (run.first(), run.last(), grade) {
                    out.push(Edge {
                        segment: [a, b],
                        grade: g,
                    });
                }
                run = vec![from, to];
            }
            grade = Some(current.grade);
            from = to;
            if degree(to) != 2 {
                break;
            }
            let Some(next) = at
                .get(&key(to))
                .and_then(|list| list.iter().copied().find(|&other| other != edge))
            else {
                break;
            };
            edge = next;
        }
        if let (Some(&a), Some(&b), Some(g)) = (run.first(), run.last(), grade)
            && run.len() >= 2
        {
            out.push(Edge {
                segment: [a, b],
                grade: g,
            });
        }
    }
    out
}

/// Whether every vertex of `run` lies within `tolerance` of the straight segment between its
/// ends: in plan, and in height from the height linear along it.
fn straight(run: &[[f64; 3]], tolerance: f64) -> bool {
    let (Some(&a), Some(&b)) = (run.first(), run.last()) else {
        return true;
    };
    let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
    let length2 = dx.mul_add(dx, dy * dy);
    if length2 <= 0.0 {
        return false;
    }
    run.iter().all(|p| {
        let along = (dx.mul_add(p[0] - a[0], dy * (p[1] - a[1])) / length2).clamp(0.0, 1.0);
        let (fx, fy, fz) = (
            dx.mul_add(along, a[0]),
            dy.mul_add(along, a[1]),
            (b[2] - a[2]).mul_add(along, a[2]),
        );
        (p[0] - fx).hypot(p[1] - fy) <= tolerance && (p[2] - fz).abs() <= tolerance
    })
}
