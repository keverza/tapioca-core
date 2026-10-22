//! Recover refinement ancestry by fixed-edge topology, not point collinearity.
//!
//! A rounded midpoint on an oblique survey edge need not lie exactly on the
//! original geometric line. Reinserting its endpoints as a constraint or using
//! exact point location would lose the split path. Original vertices are graph
//! junctions; every added fixed-edge vertex has exactly two fixed neighbours.

use std::collections::{BTreeMap, BTreeSet};

use spade::Triangulation;

use super::{Cdt, ConstraintEdge, Error};
use crate::terrain::classify::key;

pub(super) struct Paths {
    edges: BTreeMap<[u32; 2], Vec<u32>>,
    pub hosts: BTreeMap<u32, [u32; 2]>,
}

impl Paths {
    pub(super) fn new(original: &Cdt, refined: &Cdt) -> Result<Self, Error> {
        let count = u32::try_from(original.num_vertices()).map_err(|_| Error::Shape)?;
        let mut adjacent: BTreeMap<u32, Vec<u32>> = BTreeMap::new();
        for edge in refined
            .undirected_edges()
            .filter(|edge| edge.is_constraint_edge())
        {
            let [a, b] = edge
                .vertices()
                .map(|vertex| u32::try_from(vertex.index()).map_err(|_| Error::Shape));
            let (a, b) = (a?, b?);
            adjacent.entry(a).or_default().push(b);
            adjacent.entry(b).or_default().push(a);
        }
        let original_edges: BTreeSet<_> = original
            .undirected_edges()
            .map(|edge| {
                let [a, b] = edge
                    .vertices()
                    .map(|vertex| u32::try_from(vertex.index()).map_err(|_| Error::Shape));
                Ok(key([a?, b?]))
            })
            .collect::<Result<_, Error>>()?;
        let mut result = Self {
            edges: BTreeMap::new(),
            hosts: BTreeMap::new(),
        };
        for (&start, neighbours) in adjacent.range(..count) {
            for &next in neighbours {
                let path = trace(&adjacent, count, [start, next])?;
                let end = path.last().copied().ok_or(Error::TriangulationDegenerate)?;
                if start >= end {
                    continue;
                }
                let edge = [start, end];
                if !original_edges.contains(&edge) || result.edges.contains_key(&edge) {
                    return Err(Error::TriangulationDegenerate);
                }
                for &vertex in path.iter().filter(|&&vertex| vertex >= count) {
                    if result.hosts.insert(vertex, edge).is_some() {
                        return Err(Error::TriangulationDegenerate);
                    }
                }
                result.edges.insert(edge, path);
            }
        }
        if result.edges.len() != original_edges.len() {
            return Err(Error::TriangulationDegenerate);
        }
        Ok(result)
    }

    pub(super) fn rebuild_rows(
        &self,
        original: &[ConstraintEdge],
    ) -> Result<Vec<ConstraintEdge>, Error> {
        let mut rows = Vec::new();
        for row in original {
            let path = self
                .edges
                .get(&key(row.vertices))
                .ok_or(Error::BreaklineUnenforced {
                    constraint: row.segment.constraint,
                })?;
            let forward = row.vertices[0] < row.vertices[1];
            let mut pieces: Vec<_> = path
                .windows(2)
                .filter_map(|pair| match *pair {
                    [a, b] => Some(if forward { [a, b] } else { [b, a] }),
                    _ => None,
                })
                .collect();
            if !forward {
                pieces.reverse();
            }
            rows.extend(
                pieces
                    .into_iter()
                    .map(|vertices| ConstraintEdge { vertices, ..*row }),
            );
        }
        Ok(rows)
    }
}

fn trace(
    adjacent: &BTreeMap<u32, Vec<u32>>,
    count: u32,
    [start, next]: [u32; 2],
) -> Result<Vec<u32>, Error> {
    let mut path = vec![start, next];
    let mut previous = start;
    let mut current = next;
    while current >= count {
        let Some(&[a, b]) = adjacent.get(&current).map(Vec::as_slice) else {
            return Err(Error::TriangulationDegenerate);
        };
        let next = if a == previous {
            b
        } else if b == previous {
            a
        } else {
            return Err(Error::TriangulationDegenerate);
        };
        if path.len() > adjacent.len() {
            return Err(Error::TriangulationDegenerate);
        }
        previous = current;
        current = next;
        path.push(current);
    }
    Ok(path)
}
