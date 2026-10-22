//! Plan-view (xy) ring and polyline queries shared by the terrain field, edits
//! and volumes. Heights are carried, never used for the plan tests.
//!
//! Tolerance: `ON` is an absolute plan distance in metres below which a point is
//! on a ring; points closer than that are neither inside nor outside.

/// Plan distance treated as lying on a ring (metres).
pub(super) const ON: f64 = 1e-9;

/// `floor(t)` clamped to `0..=limit`; NaN gives 0.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the value is floored and clamped to 0..=limit (a u32) before the cast"
)]
pub(super) fn clamp_index(t: f64, limit: u32) -> u32 {
    if t.is_nan() {
        return 0;
    }
    t.floor().clamp(0.0, f64::from(limit)) as u32
}

/// Nearest point of a polyline (or ring) to `(x, y)`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct Nearest {
    /// Plan distance in metres.
    pub(super) distance: f64,
    /// Height interpolated linearly along the nearest segment.
    pub(super) z: f64,
    /// Index of the nearest segment's first vertex.
    pub(super) segment: usize,
    /// Signed plan offset: positive to the left of the segment's direction.
    pub(super) left: f64,
}

/// xyz vertices of a flat pool, tolerating a ragged tail.
pub(super) fn vertices(pool: &[f64]) -> &[[f64; 3]] {
    pool.as_chunks::<3>().0
}

/// Nearest point on the open polyline through `points`, or the closed ring when
/// `closed`. `None` for fewer than one segment.
pub(super) fn nearest(points: &[[f64; 3]], closed: bool, x: f64, y: f64) -> Option<Nearest> {
    let count = points.len();
    let segments = if closed {
        count
    } else {
        count.saturating_sub(1)
    };
    if count < 2 {
        return None;
    }
    let mut best: Option<Nearest> = None;
    for index in 0..segments {
        let (Some(&[ax, ay, az]), Some(&[bx, by, bz])) =
            (points.get(index), points.get((index + 1) % count))
        else {
            continue;
        };
        let (dx, dy) = (bx - ax, by - ay);
        let length2 = dx * dx + dy * dy;
        let t = if length2 > 0.0 {
            (((x - ax) * dx + (y - ay) * dy) / length2).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let (px, py) = (ax + t * dx, ay + t * dy);
        let distance = (x - px).hypot(y - py);
        let left = if length2 > 0.0 {
            (dx * (y - ay) - dy * (x - ax)) / length2.sqrt()
        } else {
            0.0
        };
        if best.is_none_or(|current| distance < current.distance) {
            best = Some(Nearest {
                distance,
                z: az + t * (bz - az),
                segment: index,
                left,
            });
        }
    }
    best
}

/// Even-odd point-in-ring test for a closed ring; points within [`ON`] of the
/// ring are reported as `None` (on the ring).
pub(super) fn inside(ring: &[[f64; 3]], x: f64, y: f64) -> Option<bool> {
    if nearest(ring, true, x, y).is_some_and(|near| near.distance <= ON) {
        return None;
    }
    let mut odd = false;
    let count = ring.len();
    for index in 0..count {
        let (Some(&[ax, ay, _]), Some(&[bx, by, _])) =
            (ring.get(index), ring.get((index + 1) % count))
        else {
            continue;
        };
        if (ay > y) != (by > y) && x < ax + (y - ay) * (bx - ax) / (by - ay) {
            odd = !odd;
        }
    }
    Some(odd)
}

#[cfg(test)]
mod tests {
    use super::{inside, nearest};

    const SQUARE: [[f64; 3]; 4] = [
        [0.0, 0.0, 0.0],
        [4.0, 0.0, 1.0],
        [4.0, 4.0, 2.0],
        [0.0, 4.0, 1.0],
    ];

    #[test]
    fn nearest_interpolates_height_and_signs_the_side() {
        let line = [[0.0, 0.0, 0.0], [10.0, 0.0, 5.0]];
        let near = nearest(&line, false, 4.0, 3.0);
        assert_eq!(
            near.map(|n| (n.distance, n.z, n.segment, n.left)),
            Some((3.0, 2.0, 0, 3.0))
        );
        assert_eq!(nearest(&line, false, 4.0, -2.0).map(|n| n.left), Some(-2.0));
        // Beyond the end: distance to the end vertex, its height.
        assert_eq!(
            nearest(&line, false, 13.0, 4.0).map(|n| (n.distance, n.z)),
            Some((5.0, 5.0))
        );
        assert_eq!(nearest(&line[..1], false, 0.0, 0.0), None);
    }

    #[test]
    fn inside_is_even_odd_with_the_ring_itself_excluded() {
        assert_eq!(inside(&SQUARE, 2.0, 2.0), Some(true));
        assert_eq!(inside(&SQUARE, 5.0, 2.0), Some(false));
        assert_eq!(inside(&SQUARE, 4.0, 2.0), None);
        assert_eq!(inside(&SQUARE, 0.0, 0.0), None);
        // The closing segment counts.
        assert_eq!(
            nearest(&SQUARE, true, -1.0, 2.0).map(|n| (n.distance, n.segment)),
            Some((1.0, 3))
        );
    }
}
