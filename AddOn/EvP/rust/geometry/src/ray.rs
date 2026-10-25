//! Ray queries over a triangle index (raycast plan E0): the occlusion
//! predicate and its directional batch.
//!
//! The arithmetic mirrors the C++ `QueryEngine::Occluded` path operation for
//! operation, so the two agree on every ray, not just on most:
//!
//! - the ray preamble normalises the direction and clamps the interval exactly
//!   as `Occluded` does (an empty interval or zero direction is unoccluded);
//! - [`PreparedRay::hits_box`] is the double-precision `MaxMult` slab test,
//!   whose far bound is widened by two ulps so traversal never culls a box the
//!   triangle test would accept;
//! - [`PreparedRay::hit_distance`] is the watertight triangle test (shear to
//!   the dominant axis, signed edge functions, back faces kept).
//!
//! The index is a trait so the scene and its acceleration structure live in
//! the `raycast` crate while the query semantics stay here. An index only
//! decides WHICH triangles a ray is tested against; the answer for a triangle
//! is decided here, so a different tree cannot change a result.

use std::ops::ControlFlow;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

#[cfg(test)]
mod tests;

/// One triangle's corners, world metres.
pub type Triangle = [[f64; 3]; 3];

/// Triangle indices at or above this are never tested (the C++ engine's
/// primitive-id range ends here), so an index refuses more triangles.
pub const MAX_TRIANGLES: usize = 0x7FFF_FFFF;

/// The slab test's far-bound widening: 1 + 2 ulp (written `1.0000000000000004`
/// in the C++ original, which rounds to this value).
const FAR_WIDEN: f64 = f64::from_bits(0x3FF0_0000_0000_0002);

/// A ray prepared once for every box and triangle it meets.
#[derive(Clone, Copy, Debug)]
pub struct PreparedRay {
    origin: [f64; 3],
    inverse: [f64; 3],
    negative: [bool; 3],
    min_t: f64,
    max_t: f64,
    // Watertight shear: the dominant axis and its two companions.
    kx: usize,
    ky: usize,
    kz: usize,
    sx: f64,
    sy: f64,
    sz: f64,
}

/// A triangle index: which triangles a ray can meet.
///
/// Implementations must be immutable while shared and visit, for a ray,
/// every triangle whose bounds [`PreparedRay::hits_box`] accepts along its
/// path (a conservative superset is allowed; a missing triangle is a wrong
/// answer). Visiting stops as soon as the visitor breaks.
pub trait TriangleIndex: Sync {
    /// Number of triangles; zero means no scene.
    fn triangle_count(&self) -> usize;

    /// Visit candidate triangles for `ray` (index, corners) until `visitor`
    /// returns `Break`, which is then returned.
    fn visit(
        &self,
        ray: &PreparedRay,
        visitor: &mut dyn FnMut(u32, &Triangle) -> ControlFlow<()>,
    ) -> ControlFlow<()>;
}

fn safe_max(a: f64, b: f64) -> f64 {
    if a > b { a } else { b }
}

fn safe_min(a: f64, b: f64) -> f64 {
    if a < b { a } else { b }
}

// 1/v, or a signed infinity when |v| < epsilon. -0.0 counts as positive, as in
// the C++ original (which tests `v < 0`, not the sign bit).
fn safe_inverse(v: f64) -> f64 {
    if v.abs() < f64::EPSILON {
        if v < 0.0 {
            -f64::INFINITY
        } else {
            f64::INFINITY
        }
    } else {
        1.0 / v
    }
}

fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn axis(v: [f64; 3], index: usize) -> f64 {
    match index {
        0 => v[0],
        1 => v[1],
        _ => v[2],
    }
}

impl PreparedRay {
    /// The occlusion ray from `origin` along `direction` over `[tmin, tmax)`,
    /// or `None` when nothing can be hit: a zero-length direction, or an
    /// empty or inverted interval. `tmin <= 0` (or NaN) starts at the origin;
    /// `tmax <= 0` (or NaN) is unbounded.
    #[must_use]
    pub fn occlusion(origin: [f64; 3], direction: [f64; 3], tmin: f64, tmax: f64) -> Option<Self> {
        let [x, y, z] = direction;
        let length = (x * x + y * y + z * z).sqrt();
        if length <= 0.0 {
            return None;
        }
        let dir = [x / length, y / length, z / length];
        let min_t = if tmin > 0.0 { tmin } else { 0.0 };
        let max_t = if tmax > 0.0 { tmax } else { f64::MAX };
        if max_t <= min_t {
            return None;
        }
        Some(Self::new(origin, dir, min_t, max_t))
    }

    fn new(origin: [f64; 3], dir: [f64; 3], min_t: f64, max_t: f64) -> Self {
        let mut kz = 0;
        let mut dominant = dir[0].abs();
        if dominant < dir[1].abs() {
            kz = 1;
            dominant = dir[1].abs();
        }
        if dominant < dir[2].abs() {
            kz = 2;
        }
        let mut kx = (kz + 1) % 3;
        let mut ky = (kx + 1) % 3;
        if axis(dir, kz) < 0.0 {
            std::mem::swap(&mut kx, &mut ky);
        }
        let dz = axis(dir, kz);
        Self {
            origin,
            inverse: dir.map(safe_inverse),
            negative: dir.map(|v| v < 0.0),
            min_t,
            max_t,
            kx,
            ky,
            kz,
            sx: axis(dir, kx) / dz,
            sy: axis(dir, ky) / dz,
            sz: 1.0 / dz,
        }
    }

    /// Whether the ray's interval meets the box `[lo, hi]`.
    #[must_use]
    pub fn hits_box(&self, lo: [f64; 3], hi: [f64; 3]) -> bool {
        let mut near = [0.0; 3];
        let mut far = [0.0; 3];
        for (i, (n, f)) in near.iter_mut().zip(far.iter_mut()).enumerate() {
            let (entry, exit) = if axis_flag(self.negative, i) {
                (axis(hi, i), axis(lo, i))
            } else {
                (axis(lo, i), axis(hi, i))
            };
            let origin = axis(self.origin, i);
            let inverse = axis(self.inverse, i);
            *n = (entry - origin) * inverse;
            *f = (exit - origin) * inverse * FAR_WIDEN;
        }
        let t0 = safe_max(near[2], safe_max(near[1], safe_max(near[0], self.min_t)));
        let t1 = safe_min(far[2], safe_min(far[1], safe_min(far[0], self.max_t)));
        t0 <= t1
    }

    /// Distance to `triangle` if the ray meets it at `min_t <= t <= limit`.
    /// Back faces are kept; degenerate triangles never hit.
    #[must_use]
    #[expect(
        clippy::many_single_char_names,
        reason = "the published watertight-test notation"
    )]
    pub fn hit_distance(&self, triangle: &Triangle, limit: f64) -> Option<f64> {
        let [p0, p1, p2] = *triangle;
        let a = sub(p0, self.origin);
        let b = sub(p1, self.origin);
        let c = sub(p2, self.origin);
        let (az, bz, cz) = (axis(a, self.kz), axis(b, self.kz), axis(c, self.kz));
        let ax = axis(a, self.kx) - self.sx * az;
        let ay = axis(a, self.ky) - self.sy * az;
        let bx = axis(b, self.kx) - self.sx * bz;
        let by = axis(b, self.ky) - self.sy * bz;
        let cx = axis(c, self.kx) - self.sx * cz;
        let cy = axis(c, self.ky) - self.sy * cz;
        // In double precision the C++ original's exact-zero recomputation
        // repeats the same operations, so it is omitted.
        let u = cx * by - cy * bx;
        let v = ax * cy - ay * cx;
        let w = bx * ay - by * ax;
        if (u < 0.0 || v < 0.0 || w < 0.0) && (u > 0.0 || v > 0.0 || w > 0.0) {
            return None;
        }
        let det = u + v + w;
        if det == 0.0 {
            return None;
        }
        let d = u * (self.sz * az) + v * (self.sz * bz) + w * (self.sz * cz);
        let t = d * (1.0 / det);
        // NaN fails both comparisons in the original and is accepted there;
        // the inputs are validated finite, so it cannot arise from a scene.
        if t > limit || t < self.min_t {
            return None;
        }
        Some(t)
    }

    /// The far end of the interval.
    #[must_use]
    pub fn max_t(&self) -> f64 {
        self.max_t
    }

    /// Whether the ray runs toward negative `axis` (0, 1 or 2): the near child
    /// of a split on that axis is the upper one.
    #[must_use]
    pub fn runs_negative(&self, axis_index: usize) -> bool {
        axis_flag(self.negative, axis_index)
    }
}

fn axis_flag(flags: [bool; 3], index: usize) -> bool {
    match index {
        0 => flags[0],
        1 => flags[1],
        _ => flags[2],
    }
}

/// Whether anything in `index` blocks `ray` strictly before its far end.
#[must_use]
pub fn any_hit(index: &dyn TriangleIndex, ray: &PreparedRay) -> bool {
    let limit = ray.max_t();
    index
        .visit(
            ray,
            &mut |_, triangle| match ray.hit_distance(triangle, limit) {
                Some(t) if t < limit => ControlFlow::Break(()),
                _ => ControlFlow::Continue(()),
            },
        )
        .is_break()
}

/// `QueryEngine::Occluded`: is anything between `origin + tmin * d` and
/// `origin + tmax * d`, with `d` the normalised `direction`?
#[must_use]
pub fn occluded(
    index: &dyn TriangleIndex,
    origin: [f64; 3],
    direction: [f64; 3],
    tmin: f64,
    tmax: f64,
) -> bool {
    if index.triangle_count() == 0 {
        return false;
    }
    PreparedRay::occlusion(origin, direction, tmin, tmax).is_some_and(|ray| any_hit(index, &ray))
}

/// Many origins, one direction (the sun-study step).
#[derive(Clone, Copy, Debug)]
pub struct DirectionalQuery<'a> {
    /// `3 * count` origin coordinates.
    pub origins: &'a [f64],
    /// Shared direction; normalised per ray as [`occluded`] does.
    pub direction: [f64; 3],
    /// Near bound (see [`PreparedRay::occlusion`]).
    pub tmin: f64,
    /// Far bound (see [`PreparedRay::occlusion`]).
    pub tmax: f64,
    /// 0 decides, 1 runs inline, n uses at most n threads.
    pub max_parallel: usize,
}

/// A directional batch whose arrays disagree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShapeError;

// Below this many rays a thread costs more than the work it takes away.
const INLINE_THRESHOLD: usize = 4096;
// The least work that justifies a thread when the count is ours to decide.
const MIN_RAYS_PER_THREAD: usize = 2048;
// Rays claimed per batch: ray costs are very unequal, so workers claim small
// contiguous batches instead of static shards.
const BATCH_RAYS: usize = 256;

/// The C++ `ChooseThreadCount`: an explicit `max_parallel` is obeyed exactly;
/// 0 uses the hardware threads, bounded by the work.
#[must_use]
pub fn choose_thread_count(count: usize, max_parallel: usize) -> usize {
    if count == 0 || max_parallel == 1 || count < INLINE_THRESHOLD {
        return 1;
    }
    let mut threads = max_parallel;
    if threads == 0 {
        threads = std::thread::available_parallelism().map_or(1, std::num::NonZero::get);
        threads = threads.min((count / MIN_RAYS_PER_THREAD).max(1));
    }
    threads.min(count).max(1)
}

/// Trace every origin along the query direction into `out` (1 occluded,
/// 0 clear). Returns `Ok(false)` when `is_cancelled` reported cancellation;
/// `out` is then partially written. Only the calling thread polls
/// `is_cancelled`. Results do not depend on the thread count.
///
/// # Errors
/// [`ShapeError`] when `origins` is not three coordinates per `out` entry.
pub fn occlude_directional(
    index: &dyn TriangleIndex,
    query: &DirectionalQuery<'_>,
    out: &mut [u8],
    is_cancelled: Option<&dyn Fn() -> bool>,
) -> Result<bool, ShapeError> {
    if Some(out.len()) != query.origins.len().checked_div(3)
        || !query.origins.len().is_multiple_of(3)
    {
        return Err(ShapeError);
    }
    let cancelled = || is_cancelled.is_some_and(|poll| poll());
    if cancelled() {
        return Ok(false);
    }
    if out.is_empty() {
        return Ok(true);
    }
    let threads = choose_thread_count(out.len(), query.max_parallel);
    let stopped = AtomicBool::new(false);
    let next = AtomicUsize::new(0);
    let chunks: Vec<std::sync::Mutex<&mut [u8]>> = out
        .chunks_mut(BATCH_RAYS)
        .map(std::sync::Mutex::new)
        .collect();
    // One batch per call; false when there is nothing left or work stopped.
    let claim = || {
        if stopped.load(Ordering::Relaxed) {
            return false;
        }
        let batch = next.fetch_add(1, Ordering::Relaxed);
        let Some(slot) = chunks.get(batch) else {
            return false;
        };
        // Each batch is claimed once, so the lock is never contended.
        let Ok(mut chunk) = slot.lock() else {
            stopped.store(true, Ordering::Relaxed);
            return false;
        };
        trace_batch(index, query, batch * BATCH_RAYS, &mut chunk);
        true
    };
    std::thread::scope(|scope| {
        for _ in 1..threads {
            scope.spawn(|| while claim() {});
        }
        // Only this thread polls the caller's (not necessarily Sync) callback.
        loop {
            if cancelled() {
                stopped.store(true, Ordering::Relaxed);
                break;
            }
            if !claim() {
                break;
            }
        }
    });
    Ok(!stopped.load(Ordering::Relaxed) && !cancelled())
}

fn trace_batch(
    index: &dyn TriangleIndex,
    query: &DirectionalQuery<'_>,
    first: usize,
    out: &mut [u8],
) {
    let origins = query.origins.get(first * 3..).unwrap_or_default();
    for (slot, &[x, y, z]) in out.iter_mut().zip(origins.as_chunks::<3>().0) {
        *slot = u8::from(occluded(
            index,
            [x, y, z],
            query.direction,
            query.tmin,
            query.tmax,
        ));
    }
}
