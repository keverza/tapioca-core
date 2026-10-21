//! The `spade` constrained triangulation: insertion and constraint chains.
//!
//! No `spade` type leaves this file. Every vertex is inserted, in output order,
//! before any constraint, so adding a constraint never creates a vertex and a
//! constraint edge, once added, is never split or flipped by a later one.

use spade::handles::FixedVertexHandle;
use spade::{ConstrainedDelaunayTriangulation, Point2, Triangulation};

use super::input::Prepared;
use super::presplit::Split;
use super::{ConstraintEdge, Error};

type Cdt = ConstrainedDelaunayTriangulation<Point2<f64>>;

/// All inner faces (counter-clockwise output vertex triples) and one row per
/// constraint edge, in segment order and along each segment's direction.
pub(super) struct Built {
    pub faces: Vec<[u32; 3]>,
    pub rows: Vec<ConstraintEdge>,
}

pub(super) fn constrain(prepared: &Prepared, split: &Split) -> Result<Built, Error> {
    let mut cdt = Cdt::new();
    let mut handles = Vec::with_capacity(prepared.positions.len());
    let mut owners = Vec::with_capacity(prepared.positions.len());
    for (vertex, position) in (0_u32..).zip(&prepared.positions) {
        let handle = cdt
            .insert(Point2::new(position[0], position[1]))
            .map_err(|_| Error::Coordinate)?;
        if handle.index() != owners.len() {
            // `spade` merged positions that are distinct here.
            return Err(Error::TriangulationDegenerate);
        }
        handles.push(handle);
        owners.push(vertex);
    }
    if cdt.num_inner_faces() == 0 {
        return Err(Error::TriangulationDegenerate);
    }
    let mut rows = Vec::new();
    for (segment, chain) in prepared.segments.iter().zip(&split.pieces) {
        for piece in chain.windows(2) {
            let &[from, to] = piece else {
                return Err(Error::TriangulationDegenerate);
            };
            let path = add_piece(&mut cdt, &handles, &owners, [from, to]).ok_or(
                Error::TriangulationConstraint {
                    constraint: segment.id.constraint,
                },
            )?;
            rows.extend(path.windows(2).filter_map(|edge| match *edge {
                [a, b] => Some(ConstraintEdge {
                    segment: segment.id,
                    vertices: [a, b],
                    kept: false,
                }),
                _ => None,
            }));
        }
    }
    let faces = cdt
        .inner_faces()
        .map(|face| {
            let [a, b, c] = face.vertices().map(|vertex| owner(&owners, vertex.fix()));
            Ok([a?, b?, c?])
        })
        .collect::<Result<Vec<_>, Error>>()?;
    Ok(Built { faces, rows })
}

fn owner(owners: &[u32], handle: FixedVertexHandle) -> Result<u32, Error> {
    owners
        .get(handle.index())
        .copied()
        .ok_or(Error::TriangulationDegenerate)
}

/// Constrain one pre-split piece and return its vertex path from `from` to `to`.
///
/// `try_add_constraint` refuses (returns no edge) instead of panicking when the
/// piece would cross an existing constraint. The returned edges are reordered
/// into a path by walking from `from`; a gap or a path that does not reach `to`
/// is a refusal, as is any path edge that is not then a constraint edge.
fn add_piece(
    cdt: &mut Cdt,
    handles: &[FixedVertexHandle],
    owners: &[u32],
    piece: [u32; 2],
) -> Option<Vec<u32>> {
    let [from, to] = piece.map(|vertex| {
        usize::try_from(vertex)
            .ok()
            .and_then(|index| handles.get(index))
            .copied()
    });
    let (from, to) = (from?, to?);
    let edges: Vec<[FixedVertexHandle; 2]> = cdt
        .try_add_constraint(from, to)
        .into_iter()
        .map(|edge| {
            let edge = cdt.directed_edge(edge);
            [edge.from().fix(), edge.to().fix()]
        })
        .collect();
    let mut path = vec![from];
    let mut current = from;
    while current != to && path.len() <= edges.len() {
        current = edges.iter().find_map(|&[a, b]| {
            let next = if a == current {
                b
            } else if b == current {
                a
            } else {
                return None;
            };
            (!path.contains(&next)).then_some(next)
        })?;
        path.push(current);
    }
    let complete = current == to
        && path.len() == edges.len() + 1
        && path
            .windows(2)
            .all(|pair| matches!(*pair, [a, b] if cdt.exists_constraint(a, b)));
    if !complete {
        return None;
    }
    path.iter()
        .map(|&handle| owner(owners, handle).ok())
        .collect()
}
