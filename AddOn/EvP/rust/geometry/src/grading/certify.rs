//! KKT certificate for a primal-feasible point: do non-negative multipliers on
//! its tight edges balance the gradient?
//!
//! Stationarity at unknown `k` reads `sum_(e: u = k) lambda_e -
//! sum_(e: v = k) lambda_e = -g_k`. Read `lambda_e` as flow on an uncapacitated
//! arc `v -> u`: every node needs net inflow `-g_k`, the ground's balance is free
//! (it absorbs its component's sum), and a component without the ground must
//! already balance. Such multipliers exist exactly when a maximum flow from the
//! surplus nodes to the deficit nodes saturates them.
//!
//! Where tight edges close cycles the multipliers are not unique and a spanning
//! tree's flows may be negative at an optimum; this certificate does not depend
//! on that choice. Max-flow is Dinic's algorithm with arcs in edge order.

use super::feasible::Edge;
use std::collections::VecDeque;

struct Arc {
    to: usize,
    capacity: f64,
}

/// Source, sink and the residual below which an arc is saturated.
#[derive(Clone, Copy)]
struct Ends {
    source: usize,
    sink: usize,
    epsilon: f64,
}

struct Network {
    arcs: Vec<Arc>,
    out: Vec<Vec<usize>>,
}

impl Network {
    fn new(nodes: usize) -> Self {
        Self {
            arcs: Vec::new(),
            out: vec![Vec::new(); nodes],
        }
    }

    fn add(&mut self, from: usize, to: usize, capacity: f64) {
        let index = self.arcs.len();
        self.arcs.push(Arc { to, capacity });
        self.arcs.push(Arc {
            to: from,
            capacity: 0.0,
        });
        if let Some(list) = self.out.get_mut(from) {
            list.push(index);
        }
        if let Some(list) = self.out.get_mut(to) {
            list.push(index + 1);
        }
    }

    /// Breadth-first levels over arcs with residual above `epsilon`.
    fn levels(&self, ends: Ends) -> Option<Vec<usize>> {
        let Ends {
            source,
            sink,
            epsilon,
        } = ends;
        let mut level = vec![usize::MAX; self.out.len()];
        let mut queue = VecDeque::from([source]);
        if let Some(slot) = level.get_mut(source) {
            *slot = 0;
        }
        while let Some(node) = queue.pop_front() {
            let depth = level.get(node).copied().unwrap_or(usize::MAX);
            for &index in self.out.get(node).map_or(&[][..], Vec::as_slice) {
                let Some(arc) = self.arcs.get(index) else {
                    continue;
                };
                if arc.capacity > epsilon && level.get(arc.to) == Some(&usize::MAX) {
                    if let Some(slot) = level.get_mut(arc.to) {
                        *slot = depth + 1;
                    }
                    queue.push_back(arc.to);
                }
            }
        }
        (level.get(sink) != Some(&usize::MAX)).then_some(level)
    }

    /// One augmenting path along strictly increasing levels, iterative DFS.
    fn augment(&mut self, ends: Ends, level: &[usize], cursor: &mut [usize]) -> f64 {
        let Ends {
            source,
            sink,
            epsilon,
        } = ends;
        let mut path: Vec<usize> = Vec::new();
        let mut node = source;
        loop {
            if node == sink {
                let pushed = path
                    .iter()
                    .filter_map(|&index| self.arcs.get(index).map(|arc| arc.capacity))
                    .fold(f64::INFINITY, f64::min);
                for &index in &path {
                    if let Some(arc) = self.arcs.get_mut(index) {
                        arc.capacity -= pushed;
                    }
                    if let Some(back) = self.arcs.get_mut(index ^ 1) {
                        back.capacity += pushed;
                    }
                }
                return pushed;
            }
            let depth = level.get(node).copied().unwrap_or(usize::MAX);
            let arcs = self.out.get(node).map_or(&[][..], Vec::as_slice);
            let mut advanced = false;
            while let Some(&index) = cursor.get(node).and_then(|&at| arcs.get(at)) {
                let usable = self.arcs.get(index).is_some_and(|arc| {
                    arc.capacity > epsilon && level.get(arc.to).copied() == Some(depth + 1)
                });
                if usable {
                    path.push(index);
                    node = self.arcs.get(index).map_or(sink, |arc| arc.to);
                    advanced = true;
                    break;
                }
                if let Some(at) = cursor.get_mut(node) {
                    *at += 1;
                }
            }
            if advanced {
                continue;
            }
            // Dead end: retreat and skip the arc that led here.
            let Some(index) = path.pop() else { return 0.0 };
            node = self.arcs.get(index ^ 1).map_or(source, |back| back.to);
            if let Some(at) = cursor.get_mut(node) {
                *at += 1;
            }
        }
    }

    fn max_flow(&mut self, ends: Ends) -> f64 {
        let mut total = 0.0;
        while let Some(level) = self.levels(ends) {
            let mut cursor = vec![0_usize; self.out.len()];
            loop {
                let pushed = self.augment(ends, &level, &mut cursor);
                if pushed <= ends.epsilon {
                    break;
                }
                total += pushed;
            }
        }
        total
    }
}

/// Whether multipliers `>= 0` on the `tight` edges satisfy stationarity for
/// `gradient` (one value per unknown) to within `tolerance`.
pub(super) fn certify(edges: &[Edge], tight: &[bool], gradient: &[f64], tolerance: f64) -> bool {
    let nodes = gradient.len() + 1;
    // Required net inflow per node; the ground's is set from its component.
    let mut need: Vec<f64> = std::iter::once(0.0)
        .chain(gradient.iter().map(|g| -g))
        .collect();
    // Components of the tight-edge graph (undirected), ground first.
    let mut component = vec![usize::MAX; nodes];
    let mut adjacent: Vec<Vec<usize>> = vec![Vec::new(); nodes];
    for (e, &flag) in edges.iter().zip(tight) {
        if flag {
            if let Some(list) = adjacent.get_mut(e.u as usize) {
                list.push(e.v as usize);
            }
            if let Some(list) = adjacent.get_mut(e.v as usize) {
                list.push(e.u as usize);
            }
        }
    }
    let mut sums = Vec::new();
    for root in 0..nodes {
        if component.get(root) != Some(&usize::MAX) {
            continue;
        }
        let id = sums.len();
        let mut sum = 0.0;
        let mut queue = VecDeque::from([root]);
        if let Some(slot) = component.get_mut(root) {
            *slot = id;
        }
        while let Some(node) = queue.pop_front() {
            sum += need.get(node).copied().unwrap_or(0.0);
            for &other in adjacent.get(node).map_or(&[][..], Vec::as_slice) {
                if component.get(other) == Some(&usize::MAX) {
                    if let Some(slot) = component.get_mut(other) {
                        *slot = id;
                    }
                    queue.push_back(other);
                }
            }
        }
        sums.push(sum);
    }
    // The ground absorbs its component; any other component must balance.
    let ground = component.first().copied().unwrap_or(0);
    if let Some(slot) = need.first_mut() {
        *slot = -sums.get(ground).copied().unwrap_or(0.0);
    }
    if sums
        .iter()
        .enumerate()
        .any(|(id, sum)| id != ground && sum.abs() > tolerance)
    {
        return false;
    }
    let (source, sink) = (nodes, nodes + 1);
    let mut network = Network::new(nodes + 2);
    for (e, &flag) in edges.iter().zip(tight) {
        if flag {
            network.add(e.v as usize, e.u as usize, f64::INFINITY);
        }
    }
    let mut supply = 0.0;
    for (node, &required) in need.iter().enumerate() {
        if required > tolerance {
            network.add(node, sink, required);
        } else if required < -tolerance {
            network.add(source, node, -required);
            supply += -required;
        }
    }
    let flow = network.max_flow(Ends {
        source,
        sink,
        epsilon: tolerance * 1e-3,
    });
    supply - flow <= tolerance * (1.0 + supply.sqrt())
}

#[cfg(test)]
mod tests {
    use super::{Edge, certify};

    fn edge(u: u32, v: u32) -> Edge {
        Edge {
            u,
            v,
            c: 0.0,
            permanent: false,
        }
    }

    #[test]
    fn a_cycle_balances_where_a_tree_alone_would_need_a_negative_flow() {
        // Tight x1 - x2 <= c, x3 - x2 <= c, x1 - x3 <= c; gradient (-2, 0, 2).
        // The spanning tree {1-2, 3-2} would need lambda(3-2) = -2, yet
        // lambda(1-3) = 2 alone is a valid certificate.
        let edges = [edge(1, 2), edge(3, 2), edge(1, 3)];
        assert!(certify(&edges, &[true; 3], &[-2.0, 0.0, 2.0], 1e-9));
        // Without the cycle edge only the tree remains: not certified.
        assert!(!certify(
            &edges,
            &[true, true, false],
            &[-2.0, 0.0, 2.0],
            1e-9
        ));
        // Reversed gradient needs negative multipliers on every arc.
        assert!(!certify(&edges, &[true; 3], &[2.0, 0.0, -2.0], 1e-9));
    }

    #[test]
    fn the_ground_absorbs_its_component_and_free_components_must_balance() {
        // x1 <= c (edge 1 -> ground): node 1 may push up against the bound.
        let bound = [edge(1, 0)];
        assert!(certify(&bound, &[true], &[-3.0], 1e-9));
        assert!(!certify(&bound, &[true], &[3.0], 1e-9));
        // An isolated unknown with a nonzero gradient is not stationary.
        assert!(!certify(&[], &[], &[1.0], 1e-9));
        assert!(certify(&[], &[], &[0.0], 1e-9));
    }
}
