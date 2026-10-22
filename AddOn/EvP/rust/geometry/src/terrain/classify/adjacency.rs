//! Deterministic flat edge/face incidence, including ALL incident faces.
//! No per-edge allocation or manifoldness assumption. Sort by undirected edge,
//! then face index: queries preserve the former `BTreeMap<Vec<face>>` traversal.
use super::{face_edges, key};

pub(super) struct Adjacency {
    // Each entry stores the original face-corner ordinal, not just face: this
    // distinguishes repeated edges while preserving (face, local edge) order.
    entries: Vec<([u32; 2], usize)>,
    spans: Vec<[[usize; 2]; 3]>,
}

impl Adjacency {
    pub(super) fn new(faces: &[[u32; 3]]) -> Self {
        let mut entries = Vec::with_capacity(faces.len().saturating_mul(3));
        for (corner, edge) in faces
            .iter()
            .flat_map(|vertices| face_edges(*vertices))
            .enumerate()
        {
            entries.push((key(edge), corner));
        }
        entries.sort_unstable();
        let mut spans = vec![[[0; 2]; 3]; faces.len()];
        let mut start = 0;
        for group in entries.chunk_by(|left, right| left.0 == right.0) {
            let end = start + group.len();
            for &(_, corner) in group {
                if let Some(span) = spans
                    .get_mut(corner / 3)
                    .and_then(|face| face.get_mut(corner % 3))
                {
                    *span = [start, end];
                }
            }
            start = end;
        }
        Self { entries, spans }
    }

    pub(super) fn faces(&self, edge: [u32; 2]) -> impl Iterator<Item = usize> + '_ {
        let edge = key(edge);
        let start = self.entries.partition_point(|entry| entry.0 < edge);
        self.entries
            .get(start..)
            .unwrap_or_default()
            .iter()
            .take_while(move |entry| entry.0 == edge)
            .map(|entry| entry.1 / 3)
    }

    /// Cached incidence span for a known face/local edge. Winding and flood
    /// traversal visit this form O(faces) times; no repeated binary search.
    pub(super) fn faces_at(&self, face: usize, edge: usize) -> impl Iterator<Item = usize> + '_ {
        let [start, end] = self
            .spans
            .get(face)
            .and_then(|spans| spans.get(edge))
            .copied()
            .unwrap_or_default();
        self.entries
            .get(start..end)
            .unwrap_or_default()
            .iter()
            .map(|entry| entry.1 / 3)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    #[test]
    fn flat_incidence_retains_empty_boundary_shared_and_nonmanifold_groups() {
        assert_eq!(Adjacency::new(&[]).faces([1, 2]).count(), 0);
        let adjacent = Adjacency::new(&[[0, 1, 2], [1, 0, 3], [0, 1, 4], [0, 0, 5]]);
        assert_eq!(adjacent.faces([1, 0]).collect::<Vec<_>>(), vec![0, 1, 2]);
        assert_eq!(adjacent.faces([0, 2]).collect::<Vec<_>>(), vec![0]);
        assert_eq!(adjacent.faces([0, 5]).collect::<Vec<_>>(), vec![3, 3]);
        assert_eq!(adjacent.faces([0, 0]).collect::<Vec<_>>(), vec![3]);
        assert_eq!(adjacent.faces([8, 9]).count(), 0);
    }

    #[test]
    fn seeded_flat_queries_match_the_prior_ordered_multimap_exactly() {
        let mut seed = 0x5a3a_2026_u32;
        for count in [0, 1, 3, 64, 512] {
            let mut number = || {
                seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                seed % 32
            };
            let faces: Vec<_> = (0..count).map(|_| [number(), number(), number()]).collect();
            let mut oracle: BTreeMap<[u32; 2], Vec<usize>> = BTreeMap::new();
            for (face, vertices) in faces.iter().enumerate() {
                for edge in face_edges(*vertices) {
                    oracle.entry(key(edge)).or_default().push(face);
                }
            }
            let adjacent = Adjacency::new(&faces);
            for first in 0..34 {
                for second in 0..34 {
                    let expected = oracle
                        .get(&key([first, second]))
                        .cloned()
                        .unwrap_or_default();
                    assert_eq!(
                        adjacent.faces([first, second]).collect::<Vec<_>>(),
                        expected
                    );
                }
            }
            for (index, face) in faces.iter().enumerate() {
                for (slot, edge) in face_edges(*face).into_iter().enumerate() {
                    let expected = oracle.get(&key(edge)).cloned().unwrap_or_default();
                    assert_eq!(adjacent.faces_at(index, slot).collect::<Vec<_>>(), expected);
                }
            }
            assert_eq!(adjacent.faces_at(faces.len(), 0).count(), 0);
            assert_eq!(adjacent.faces_at(0, 3).count(), 0);
        }
    }
}
