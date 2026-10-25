use super::*;

/// Every triangle, in order: the conservative superset any index may visit.
struct BruteForce(Vec<Triangle>);

impl TriangleIndex for BruteForce {
    fn triangle_count(&self) -> usize {
        self.0.len()
    }

    fn visit(
        &self,
        _ray: &PreparedRay,
        visitor: &mut dyn FnMut(u32, &Triangle) -> ControlFlow<()>,
    ) -> ControlFlow<()> {
        for (index, triangle) in (0u32..).zip(&self.0) {
            visitor(index, triangle)?;
        }
        ControlFlow::Continue(())
    }
}

/// A unit square in the plane z = `height`, as two triangles.
fn floor(height: f64, wind_down: bool) -> Vec<Triangle> {
    let (a, b, c, d) = (
        [0.0, 0.0, height],
        [1.0, 0.0, height],
        [1.0, 1.0, height],
        [0.0, 1.0, height],
    );
    if wind_down {
        vec![[a, c, b], [a, d, c]]
    } else {
        vec![[a, b, c], [a, c, d]]
    }
}

const UP: [f64; 3] = [0.0, 0.0, 1.0];

#[test]
fn a_roof_blocks_the_sky_from_either_winding() {
    for wind_down in [false, true] {
        let index = BruteForce(floor(3.0, wind_down));
        assert!(
            occluded(&index, [0.3, 0.6, 0.0], UP, 0.0, 0.0),
            "back faces are kept"
        );
        assert!(
            !occluded(&index, [1.3, 0.6, 0.0], UP, 0.0, 0.0),
            "beside the roof"
        );
        assert!(
            !occluded(&index, [0.3, 0.6, 0.0], [0.0, 0.0, -1.0], 0.0, 0.0),
            "facing away"
        );
    }
}

#[test]
fn the_interval_is_closed_below_and_open_above() {
    let index = BruteForce(floor(3.0, false));
    let origin = [0.3, 0.6, 0.0];
    assert!(
        occluded(&index, origin, UP, 3.0 - 1e-9, 0.0),
        "tmin just before the hit"
    );
    assert!(
        !occluded(&index, origin, UP, 3.0 + 1e-9, 0.0),
        "past the hit"
    );
    assert!(
        !occluded(&index, origin, UP, 0.0, 3.0 - 1e-9),
        "tmax just before the hit"
    );
    assert!(occluded(&index, origin, UP, 0.0, 3.0 + 1e-9));
    assert!(
        !occluded(&index, origin, UP, 2.0, 1.0),
        "inverted interval is clear"
    );
    assert!(
        !occluded(&index, origin, [0.0; 3], 0.0, 0.0),
        "zero direction is clear"
    );
    assert!(
        occluded(&index, origin, [0.0, 0.0, 7.5], 0.0, 3.5),
        "the direction is normalised"
    );
    assert!(
        occluded(&index, origin, UP, f64::NAN, f64::NAN),
        "NaN bounds fall back to defaults"
    );
}

#[test]
fn a_shared_edge_does_not_leak() {
    // The diagonal of the square is shared; a ray straight through it must hit.
    let index = BruteForce(floor(3.0, false));
    for t in [0.0, 0.25, 0.5, 0.75, 1.0] {
        assert!(
            occluded(&index, [t, t, 0.0], UP, 0.0, 0.0),
            "diagonal at {t}"
        );
    }
}

#[test]
fn degenerate_and_empty_scenes_never_block() {
    let line = [[0.0, 0.0, 3.0], [1.0, 1.0, 3.0], [2.0, 2.0, 3.0]];
    assert!(!occluded(
        &BruteForce(vec![line]),
        [0.5, 0.5, 0.0],
        UP,
        0.0,
        0.0
    ));
    assert!(!occluded(
        &BruteForce(Vec::new()),
        [0.5, 0.5, 0.0],
        UP,
        0.0,
        0.0
    ));
}

#[test]
fn georeferenced_coordinates_keep_their_precision() {
    let offset = [2_600_000.0, 1_200_000.0, 400.0];
    let shifted: Vec<Triangle> = floor(3.0, false)
        .into_iter()
        .map(|t| t.map(|p| [p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]]))
        .collect();
    let index = BruteForce(shifted);
    assert!(occluded(
        &index,
        [offset[0] + 0.3, offset[1] + 0.6, offset[2]],
        UP,
        0.0,
        0.0
    ));
    assert!(!occluded(
        &index,
        [offset[0] + 1.000_001, offset[1] + 0.6, offset[2]],
        UP,
        0.0,
        0.0
    ));
}

#[test]
fn the_box_test_accepts_what_the_triangle_test_hits() {
    let ray = PreparedRay::occlusion([0.5, 0.5, 0.0], [0.0, 0.0, 1.0], 0.0, 0.0);
    let Some(ray) = ray else {
        unreachable!("a valid ray");
    };
    assert!(
        ray.hits_box([0.0, 0.0, 3.0], [1.0, 1.0, 3.0]),
        "a flat box on the path"
    );
    assert!(
        ray.hits_box([0.5, 0.5, 3.0], [1.0, 1.0, 3.0]),
        "the ray on the box's corner"
    );
    assert!(!ray.hits_box([0.6, 0.0, 3.0], [1.0, 1.0, 3.0]));
    assert!(
        !ray.hits_box([0.0, 0.0, -2.0], [1.0, 1.0, -1.0]),
        "behind the origin"
    );
}

fn city() -> Vec<Triangle> {
    let mut triangles = Vec::new();
    let mut seed = 0x2545_f491_4f6c_dd1du64;
    let mut next = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        f64::from(u32::try_from(seed >> 40).unwrap_or(0)) / f64::from(1u32 << 24)
    };
    for _ in 0..300 {
        let base = [next() * 40.0 - 20.0, next() * 40.0 - 20.0, next() * 12.0];
        triangles.push([
            base,
            [
                base[0] + next() * 3.0,
                base[1] + next() * 3.0,
                base[2] + next(),
            ],
            [
                base[0] + next() * 3.0,
                base[1] - next() * 3.0,
                base[2] + next() * 4.0,
            ],
        ]);
    }
    triangles
}

#[test]
fn the_batch_does_not_depend_on_the_thread_count() {
    let index = BruteForce(city());
    let mut origins = Vec::new();
    for j in 0..80 {
        for i in 0..80 {
            origins.extend([f64::from(i) * 0.5 - 20.0, f64::from(j) * 0.5 - 20.0, 0.1]);
        }
    }
    let query = DirectionalQuery {
        origins: &origins,
        direction: [0.3, -0.2, 0.7],
        tmin: 0.001,
        tmax: 0.0,
        max_parallel: 1,
    };
    let mut serial = vec![0u8; 6400];
    assert_eq!(
        occlude_directional(&index, &query, &mut serial, None),
        Ok(true)
    );
    let one_by_one: Vec<u8> = origins
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[x, y, z]| {
            u8::from(occluded(
                &index,
                [x, y, z],
                query.direction,
                query.tmin,
                query.tmax,
            ))
        })
        .collect();
    assert_eq!(serial, one_by_one);
    assert!(
        serial.contains(&1) && serial.contains(&0),
        "the corpus must shade some origins"
    );
    for max_parallel in [0, 2, 7] {
        let mut parallel = vec![9u8; 6400];
        let query = DirectionalQuery {
            max_parallel,
            ..query
        };
        assert_eq!(
            occlude_directional(&index, &query, &mut parallel, None),
            Ok(true)
        );
        assert_eq!(parallel, serial, "max_parallel {max_parallel}");
    }
}

#[test]
fn the_batch_refuses_bad_shapes_and_honours_cancellation() {
    let index = BruteForce(floor(3.0, false));
    let origins = [0.5, 0.5, 0.0, 0.2, 0.2];
    let query = DirectionalQuery {
        origins: &origins,
        direction: UP,
        tmin: 0.0,
        tmax: 0.0,
        max_parallel: 1,
    };
    assert_eq!(
        occlude_directional(&index, &query, &mut [0u8; 2], None),
        Err(ShapeError)
    );
    let origins = [0.5, 0.5, 0.0];
    let query = DirectionalQuery {
        origins: &origins,
        ..query
    };
    assert_eq!(
        occlude_directional(&index, &query, &mut [0u8; 2], None),
        Err(ShapeError)
    );
    let mut out = [7u8; 1];
    assert_eq!(
        occlude_directional(&index, &query, &mut out, Some(&|| true)),
        Ok(false)
    );
    assert_eq!(out, [7], "cancelled before any work");
    assert_eq!(
        occlude_directional(&index, &query, &mut out, Some(&|| false)),
        Ok(true)
    );
    assert_eq!(out, [1]);
}

#[test]
fn thread_choice_matches_the_cpp_policy() {
    assert_eq!(choose_thread_count(0, 8), 1);
    assert_eq!(choose_thread_count(100_000, 1), 1);
    assert_eq!(choose_thread_count(4095, 8), 1);
    assert_eq!(choose_thread_count(4096, 8), 8);
    assert_eq!(
        choose_thread_count(5000, 0).min(2),
        choose_thread_count(5000, 0),
        "bounded by work"
    );
}
