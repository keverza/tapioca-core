//! Exact cut/fill between two piecewise-linear surfaces that share one plan
//! triangulation (no grid).
//!
//! When the base surface is affine inside every triangle (its edges are
//! constraints of the triangulation, as [`super::super::region`] guarantees),
//! `diff = proposed - base` is linear on each triangle, so its positive and
//! negative parts integrate in closed form: with one corner of opposite sign `a`
//! against `b` and `c`, that corner's part is `A a^3 / (3 (a - b)(a - c))`, the
//! rest is `A (a + b + c) / 3` minus it. This is exact for the triangulated
//! surfaces, not for any analytic field they sample.
//!
//! Signs follow the grid sampler: `fill` and `cut` are magnitudes and
//! `net = fill - cut`. Triangles are summed in index order.

use super::VolumeError;

/// Volumes over a set of triangles, cubic and square metres.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TriangleVolumes {
    /// Excavated volume.
    pub cut: f64,
    /// Placed volume.
    pub fill: f64,
    /// `fill - cut`.
    pub net: f64,
    /// Plan area.
    pub area: f64,
    /// Triangles summed.
    pub triangles: u32,
}

/// Borrowed exact request.
#[derive(Clone, Copy, Debug)]
pub struct ExactRequest<'a> {
    /// Flat xyz proposed vertices, world metres.
    pub points: &'a [f64],
    /// Base height per vertex, metres.
    pub base: &'a [f64],
    /// Counter-clockwise (in plan) vertex triples.
    pub triangles: &'a [u32],
    /// Per triangle, whether it is site (inside the property line) rather than
    /// context; everything is site when `None`.
    pub inside: Option<&'a [bool]>,
}

/// Site and context volumes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ExactReport {
    /// Inside the property line, or everything.
    pub site: TriangleVolumes,
    /// Outside it.
    pub context: TriangleVolumes,
}

/// `[fill, cut]` of the linear difference over one plan triangle.
#[must_use]
pub fn triangle_cut_fill(corners: [[f64; 2]; 3], difference: [f64; 3]) -> [f64; 2] {
    let [[ax, ay], [bx, by], [cx, cy]] = corners;
    let area = (0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))).abs();
    let positive = |values: [f64; 3]| -> f64 {
        let [a, b, c] = values;
        let above = values.iter().filter(|&&v| v > 0.0).count();
        match above {
            0 => 0.0,
            3 => area * (a + b + c) / 3.0,
            _ => {
                // The lone corner on the other side of zero, then the other two.
                let lone = |p: f64, q: f64, r: f64| area * p * p * p / (3.0 * (p - q) * (p - r));
                let total = area * (a + b + c) / 3.0;
                if above == 1 {
                    if a > 0.0 {
                        lone(a, b, c)
                    } else if b > 0.0 {
                        lone(b, c, a)
                    } else {
                        lone(c, a, b)
                    }
                } else if a <= 0.0 {
                    total - lone(a, b, c)
                } else if b <= 0.0 {
                    total - lone(b, c, a)
                } else {
                    total - lone(c, a, b)
                }
            }
        }
    };
    let [a, b, c] = difference;
    [positive(difference), positive([-a, -b, -c])]
}

fn add(volumes: &mut TriangleVolumes, area: f64, [fill, cut]: [f64; 2]) {
    volumes.fill += fill;
    volumes.cut += cut;
    volumes.net = volumes.fill - volumes.cut;
    volumes.area += area;
    volumes.triangles += 1;
}

/// Exact cut/fill over every triangle.
///
/// # Errors
/// [`VolumeError::Shape`] when `base` or `inside` do not match the points or
/// triangles, or a triangle names a missing vertex; [`VolumeError::Domain`] for
/// a non-finite coordinate or height.
pub fn exact_cut_fill(request: &ExactRequest<'_>) -> Result<ExactReport, VolumeError> {
    let points = request.points.as_chunks::<3>();
    let corners = request.triangles.as_chunks::<3>();
    if !points.1.is_empty()
        || !corners.1.is_empty()
        || request.base.len() != points.0.len()
        || request
            .inside
            .is_some_and(|flags| flags.len() != corners.0.len())
    {
        return Err(VolumeError::Shape);
    }
    if request
        .points
        .iter()
        .chain(request.base)
        .any(|value| !value.is_finite())
    {
        return Err(VolumeError::Domain);
    }
    let mut report = ExactReport::default();
    for (position, triple) in corners.0.iter().enumerate() {
        let mut plan = [[0.0; 2]; 3];
        let mut difference = [0.0; 3];
        for ((slot, delta), &vertex) in plan.iter_mut().zip(&mut difference).zip(triple) {
            let (Some(&[x, y, z]), Some(&base)) = (
                points.0.get(vertex as usize),
                request.base.get(vertex as usize),
            ) else {
                return Err(VolumeError::Shape);
            };
            *slot = [x, y];
            *delta = z - base;
        }
        let [[ax, ay], [bx, by], [cx, cy]] = plan;
        let area = (0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))).abs();
        let site = request
            .inside
            .is_none_or(|flags| flags.get(position).copied().unwrap_or(false));
        let target = if site {
            &mut report.site
        } else {
            &mut report.context
        };
        add(target, area, triangle_cut_fill(plan, difference));
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(left: f64, right: f64, tolerance: f64) -> bool {
        (left - right).abs() <= tolerance
    }

    const UNIT: [[f64; 2]; 3] = [[0.0, 0.0], [2.0, 0.0], [0.0, 2.0]];

    #[test]
    fn one_signed_triangle_integrates_to_its_mean_times_area() {
        assert_eq!(triangle_cut_fill(UNIT, [1.0, 1.0, 1.0]), [2.0, 0.0]);
        assert_eq!(triangle_cut_fill(UNIT, [-3.0, 0.0, 0.0]), [0.0, 2.0]);
        assert_eq!(triangle_cut_fill(UNIT, [0.0, 0.0, 0.0]), [0.0, 0.0]);
    }

    #[test]
    fn a_sign_change_splits_at_the_zero_line_exactly() {
        // diff = x - 1 over the triangle: fill on x > 1 is the small corner
        // triangle (area 0.5, mean height 1/3), cut is the rest.
        let [fill, cut] = triangle_cut_fill(UNIT, [-1.0, 1.0, -1.0]);
        assert!(close(fill, 0.5 / 3.0, 1e-15), "{fill}");
        assert!(close(fill - cut, 2.0 * (-1.0 + 1.0 - 1.0) / 3.0, 1e-15));
        // Two positive corners: the mirror of the case above.
        let [fill, cut] = triangle_cut_fill(UNIT, [1.0, -1.0, 1.0]);
        assert!(close(cut, 0.5 / 3.0, 1e-15) && close(fill - cut, 2.0 / 3.0, 1e-15));
        // Every rotation of the corners gives the same parts.
        for difference in [[2.0, -1.0, 0.5], [-1.0, 0.5, 2.0], [0.5, 2.0, -1.0]] {
            let [fill, cut] = triangle_cut_fill(UNIT, difference);
            assert!(close(fill - cut, 2.0 * 1.5 / 3.0, 1e-14));
            // The lone negative corner: A p^3 / (3 (p - q)(p - r)), p = 1, q = -2, r = -0.5.
            assert!(close(cut, 2.0 / (3.0 * 3.0 * 1.5), 1e-14), "{cut}");
        }
    }

    #[test]
    fn site_and_context_split_by_flag_and_shapes_refuse() -> Result<(), VolumeError> {
        let points = [0.0, 0.0, 1.0, 2.0, 0.0, 1.0, 0.0, 2.0, 1.0, 2.0, 2.0, 1.0];
        let base = [0.0; 4];
        let triangles = [0, 1, 2, 1, 3, 2];
        let flags = [true, false];
        let report = exact_cut_fill(&ExactRequest {
            points: &points,
            base: &base,
            triangles: &triangles,
            inside: Some(&flags),
        })?;
        assert!(close(report.site.fill, 2.0, 1e-15) && close(report.context.fill, 2.0, 1e-15));
        assert_eq!((report.site.triangles, report.context.triangles), (1, 1));
        let refused = exact_cut_fill(&ExactRequest {
            points: &points,
            base: &base[..3],
            triangles: &triangles,
            inside: None,
        });
        assert_eq!(refused, Err(VolumeError::Shape));
        Ok(())
    }
}
