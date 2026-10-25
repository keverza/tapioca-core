//! A binned surface-area-heuristic bounding-volume hierarchy over triangles.
//!
//! Deterministic: the build depends only on the triangles and their order
//! (fixed-order reductions, stable partitions, lowest axis and split on equal
//! cost), never on threads or hashing. Nodes are laid out depth first, so a
//! node's left child is the next node.
//!
//! Bounds are the exact coordinate minima and maxima of each triangle, so
//! together with the widened slab test in `geometry::ray` a ray that hits a
//! triangle always reaches its leaf.

use std::ops::ControlFlow;

use tapioca_geometry::ray::{PreparedRay, Triangle};

/// Leaves hold at most this many triangles unless the depth limit is reached.
const LEAF_SIZE: usize = 4;
/// Centroid bins per axis.
const BINS: usize = 16;
/// `BINS` as a real.
const BINS_REAL: f64 = 16.0;
/// The last bin index as a real.
const LAST_BIN_REAL: f64 = 15.0;
/// Depth bound; it also bounds the traversal stack.
const MAX_DEPTH: usize = 64;
/// Traversal stack: one pending far child per level plus the root.
const STACK: usize = MAX_DEPTH + 2;

#[derive(Clone, Copy, Debug)]
struct Bounds {
    lo: [f64; 3],
    hi: [f64; 3],
}

impl Bounds {
    const EMPTY: Self = Self {
        lo: [f64::INFINITY; 3],
        hi: [f64::NEG_INFINITY; 3],
    };

    fn of(triangle: &Triangle) -> Self {
        let mut bounds = Self::EMPTY;
        for point in triangle {
            bounds.grow_point(*point);
        }
        bounds
    }

    fn grow_point(&mut self, point: [f64; 3]) {
        for ((lo, hi), value) in self.lo.iter_mut().zip(self.hi.iter_mut()).zip(point) {
            *lo = lo.min(value);
            *hi = hi.max(value);
        }
    }

    fn grow(&mut self, other: &Self) {
        self.grow_point(other.lo);
        self.grow_point(other.hi);
    }

    fn centroid(&self) -> [f64; 3] {
        [
            f64::midpoint(self.lo[0], self.hi[0]),
            f64::midpoint(self.lo[1], self.hi[1]),
            f64::midpoint(self.lo[2], self.hi[2]),
        ]
    }

    fn area(&self) -> f64 {
        let [dx, dy, dz] = [
            self.hi[0] - self.lo[0],
            self.hi[1] - self.lo[1],
            self.hi[2] - self.lo[2],
        ];
        if dx < 0.0 {
            0.0
        } else {
            2.0 * (dx * dy + dy * dz + dz * dx)
        }
    }
}

#[derive(Clone, Copy, Debug)]
enum Kind {
    Leaf { first: usize, count: usize },
    Inner { right: usize, axis: usize },
}

#[derive(Clone, Copy, Debug)]
struct Node {
    bounds: Bounds,
    kind: Kind,
}

/// The hierarchy; leaves address a triangle order returned by [`Bvh::build`].
#[derive(Debug)]
pub(super) struct Bvh {
    nodes: Vec<Node>,
}

struct Builder<'a> {
    prims: &'a [Bounds],
    centroids: Vec<[f64; 3]>,
    nodes: Vec<Node>,
}

#[derive(Clone, Copy)]
struct Split {
    axis: usize,
    // Bins below this index go left.
    bin: usize,
    cost: f64,
}

fn coordinate(point: [f64; 3], axis: usize) -> f64 {
    match axis {
        0 => point[0],
        1 => point[1],
        _ => point[2],
    }
}

// A triangle count as a SAH weight; scenes hold fewer than 2^31 triangles,
// so the conversion is exact.
fn weight(count: usize) -> f64 {
    f64::from(u32::try_from(count).unwrap_or(u32::MAX))
}

fn bin_of(value: f64, lo: f64, scale: f64) -> usize {
    // Truncation toward zero of a nonnegative finite value, clamped.
    let raw = ((value - lo) * scale).floor();
    if raw <= 0.0 {
        0
    } else if raw >= LAST_BIN_REAL {
        BINS - 1
    } else {
        #[expect(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "bounded to [0, BINS - 1] above"
        )]
        let bin = raw as usize;
        bin
    }
}

impl Builder<'_> {
    fn bounds_of(&self, refs: &[usize]) -> (Bounds, Bounds) {
        let mut bounds = Bounds::EMPTY;
        let mut centres = Bounds::EMPTY;
        for &i in refs {
            if let (Some(prim), Some(centre)) = (self.prims.get(i), self.centroids.get(i)) {
                bounds.grow(prim);
                centres.grow_point(*centre);
            }
        }
        (bounds, centres)
    }

    fn best_split(&self, refs: &[usize], centres: &Bounds) -> Option<Split> {
        let mut best: Option<Split> = None;
        for axis in 0..3 {
            let lo = coordinate(centres.lo, axis);
            let extent = coordinate(centres.hi, axis) - lo;
            if extent <= 0.0 {
                continue;
            }
            let scale = BINS_REAL / extent;
            let mut counts = [0usize; BINS];
            let mut boxes = [Bounds::EMPTY; BINS];
            for &i in refs {
                let (Some(prim), Some(centre)) = (self.prims.get(i), self.centroids.get(i)) else {
                    continue;
                };
                let bin = bin_of(coordinate(*centre, axis), lo, scale);
                if let (Some(count), Some(bounds)) = (counts.get_mut(bin), boxes.get_mut(bin)) {
                    *count += 1;
                    bounds.grow(prim);
                }
            }
            // Right-to-left sweep, then left-to-right, both in fixed order.
            let mut right_area = [0.0; BINS];
            let mut right_count = [0usize; BINS];
            let mut acc = Bounds::EMPTY;
            let mut n = 0;
            for bin in (1..BINS).rev() {
                if let (Some(b), Some(c)) = (boxes.get(bin), counts.get(bin)) {
                    acc.grow(b);
                    n += c;
                }
                if let (Some(a), Some(c)) = (right_area.get_mut(bin), right_count.get_mut(bin)) {
                    *a = acc.area();
                    *c = n;
                }
            }
            let mut left = Bounds::EMPTY;
            let mut left_count = 0;
            for bin in 1..BINS {
                if let (Some(b), Some(c)) = (boxes.get(bin - 1), counts.get(bin - 1)) {
                    left.grow(b);
                    left_count += c;
                }
                let (Some(&ra), Some(&rc)) = (right_area.get(bin), right_count.get(bin)) else {
                    continue;
                };
                if left_count == 0 || rc == 0 {
                    continue;
                }
                let cost = left.area() * weight(left_count) + ra * weight(rc);
                if best.is_none_or(|b| cost < b.cost) {
                    best = Some(Split { axis, bin, cost });
                }
            }
        }
        best
    }

    // Stable partition of `refs` by `goes_left`; returns the left count.
    fn partition(refs: &mut [usize], goes_left: impl Fn(usize) -> bool) -> usize {
        let (left, right): (Vec<usize>, Vec<usize>) = refs.iter().partition(|&&i| goes_left(i));
        let count = left.len();
        for (slot, value) in refs.iter_mut().zip(left.into_iter().chain(right)) {
            *slot = value;
        }
        count
    }

    fn build(&mut self, refs: &mut [usize], first: usize, depth: usize) {
        let (bounds, centres) = self.bounds_of(refs);
        let index = self.nodes.len();
        self.nodes.push(Node {
            bounds,
            kind: Kind::Leaf {
                first,
                count: refs.len(),
            },
        });
        if refs.len() <= LEAF_SIZE || depth >= MAX_DEPTH {
            return;
        }
        let leaf_cost = bounds.area() * weight(refs.len());
        let split = self.best_split(refs, &centres);
        let (axis, left) = match split {
            Some(split) if split.cost < leaf_cost || refs.len() > LEAF_SIZE * 4 => {
                let lo = coordinate(centres.lo, split.axis);
                let scale = BINS_REAL / (coordinate(centres.hi, split.axis) - lo);
                let centroids = &self.centroids;
                let left = Self::partition(refs, |i| {
                    centroids
                        .get(i)
                        .is_some_and(|c| bin_of(coordinate(*c, split.axis), lo, scale) < split.bin)
                });
                (split.axis, left)
            }
            Some(_) => return,
            // Identical centroids: any split is valid; halve in input order.
            None => (0, refs.len() / 2),
        };
        let (left_refs, right_refs) = refs.split_at_mut(left);
        self.build(left_refs, first, depth + 1);
        let right = self.nodes.len();
        self.build(right_refs, first + left, depth + 1);
        if let Some(node) = self.nodes.get_mut(index) {
            node.kind = Kind::Inner { right, axis };
        }
    }
}

impl Bvh {
    /// Build over `triangles`; returns the tree and the triangle order its
    /// leaves address (positions into `triangles`).
    pub(super) fn build(triangles: &[Triangle]) -> (Self, Vec<usize>) {
        let prims: Vec<Bounds> = triangles.iter().map(Bounds::of).collect();
        let mut builder = Builder {
            centroids: prims.iter().map(Bounds::centroid).collect(),
            prims: &prims,
            nodes: Vec::with_capacity(triangles.len() / 2 + 1),
        };
        let mut order: Vec<usize> = (0..triangles.len()).collect();
        if !order.is_empty() {
            builder.build(&mut order, 0, 0);
        }
        (
            Self {
                nodes: builder.nodes,
            },
            order,
        )
    }

    pub(super) fn node_count(&self) -> usize {
        self.nodes.len()
    }

    /// Visit the leaves (`first`, `count` into the leaf order) whose path
    /// `ray` reaches, near child first, until `leaf` breaks.
    pub(super) fn visit(
        &self,
        ray: &PreparedRay,
        leaf: &mut dyn FnMut(usize, usize) -> ControlFlow<()>,
    ) -> ControlFlow<()> {
        if self.nodes.is_empty() {
            return ControlFlow::Continue(());
        }
        let mut stack = [0usize; STACK];
        let mut pending: usize = 1;
        while let Some(top) = pending.checked_sub(1) {
            pending = top;
            let Some(node) = stack.get(top).and_then(|&i| self.nodes.get(i)) else {
                break;
            };
            if !ray.hits_box(node.bounds.lo, node.bounds.hi) {
                continue;
            }
            match node.kind {
                Kind::Leaf { first, count } => leaf(first, count)?,
                Kind::Inner { right, axis } => {
                    let left = stack.get(top).map_or(0, |&i| i + 1);
                    let (near, far) = if ray.runs_negative(axis) {
                        (right, left)
                    } else {
                        (left, right)
                    };
                    // Depth is bounded by MAX_DEPTH, so the stack cannot overflow.
                    if let Some(slot) = stack.get_mut(pending) {
                        *slot = far;
                        pending += 1;
                    }
                    if let Some(slot) = stack.get_mut(pending) {
                        *slot = near;
                        pending += 1;
                    }
                }
            }
        }
        ControlFlow::Continue(())
    }
}
