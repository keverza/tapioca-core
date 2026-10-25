use super::*;
use tapioca_geometry::ray::{DirectionalQuery, occlude_directional, occluded};

struct Rng(u64);

impl Rng {
    fn unit(&mut self) -> f64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        f64::from(u32::try_from(self.0 >> 40).unwrap_or(0)) / f64::from(1u32 << 24)
    }
}

/// Every triangle, in input order: the oracle a tree must agree with.
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

fn quad(
    vertices: &mut Vec<f64>,
    triangles: &mut Vec<u32>,
    offset: [f64; 3],
    corners: [[f64; 3]; 4],
) {
    let base = u32::try_from(vertices.len() / 3).unwrap_or(0);
    for c in corners {
        vertices.extend([c[0] + offset[0], c[1] + offset[1], c[2] + offset[2]]);
    }
    triangles.extend([base, base + 1, base + 2, base, base + 2, base + 3]);
}

#[expect(
    clippy::many_single_char_names,
    reason = "box coordinates read as x, y, w, d, h"
)]
/// Random boxes as quads, plus a coincident and a degenerate triangle, around
/// `offset` (survey coordinates when it is large).
fn corpus(rng: &mut Rng, boxes: usize, offset: [f64; 3]) -> (Vec<f64>, Vec<u32>) {
    let mut vertices = Vec::new();
    let mut triangles = Vec::new();
    for _ in 0..boxes {
        let (x, y) = (rng.unit() * 60.0 - 30.0, rng.unit() * 60.0 - 30.0);
        let (w, d, h) = (
            0.5 + rng.unit() * 5.0,
            0.5 + rng.unit() * 5.0,
            0.5 + rng.unit() * 15.0,
        );
        let (v, t) = (&mut vertices, &mut triangles);
        quad(
            v,
            t,
            offset,
            [[x, y, h], [x + w, y, h], [x + w, y + d, h], [x, y + d, h]],
        );
        quad(
            v,
            t,
            offset,
            [[x, y, 0.0], [x + w, y, 0.0], [x + w, y, h], [x, y, h]],
        );
        quad(
            v,
            t,
            offset,
            [[x, y, 0.0], [x, y + d, 0.0], [x, y + d, h], [x, y, h]],
        );
    }
    if boxes > 0 {
        triangles.extend_from_within(0..6);
        triangles.extend([0, 1, 1]);
    }
    (vertices, triangles)
}

fn point(vertices: &[f64], index: u32) -> [f64; 3] {
    let i = usize::try_from(index).unwrap_or(0) * 3;
    match vertices.get(i..i + 3) {
        Some(&[x, y, z]) => [x, y, z],
        _ => [0.0; 3],
    }
}

fn source(vertices: &[f64], triangles: &[u32]) -> Vec<Triangle> {
    triangles
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[a, b, c]| [point(vertices, a), point(vertices, b), point(vertices, c)])
        .collect()
}

fn build(vertices: &[f64], triangles: &[u32]) -> Result<Scene, SceneError> {
    Scene::build(&SceneInput {
        vertices,
        triangles,
        version: 1,
    })
}

#[test]
fn malformed_scenes_are_refused() {
    let vertices = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0];
    assert_eq!(
        build(&vertices[..8], &[0, 1, 2]).err(),
        Some(SceneError::VertexShape)
    );
    assert_eq!(
        build(&vertices, &[0, 1]).err(),
        Some(SceneError::TriangleShape)
    );
    assert_eq!(
        build(&vertices, &[0, 1, 3]).err(),
        Some(SceneError::IndexOutOfRange)
    );
    let mut nan = vertices;
    nan[4] = f64::NAN;
    assert_eq!(
        build(&nan, &[0, 1, 2]).err(),
        Some(SceneError::NonFiniteVertex)
    );
    assert!(build(&vertices, &[0, 1, 2]).is_ok());
    let empty = build(&[], &[]);
    assert!(
        empty.as_ref().is_ok_and(|s| s.triangle_count() == 0),
        "an empty scene is valid"
    );
    assert!(empty.is_ok_and(|s| !occluded(&s, [0.0; 3], [0.0, 0.0, 1.0], 0.0, 0.0)));
}

#[test]
fn the_tree_answers_exactly_what_every_triangle_answers() {
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    let cases = [
        (1, [0.0; 3]),
        (40, [0.0; 3]),
        (400, [0.0; 3]),
        (200, [2_600_000.0, 1_200_000.0, 450.0]),
    ];
    for (boxes, offset) in cases {
        let (vertices, triangles) = corpus(&mut rng, boxes, offset);
        let Ok(scene) = build(&vertices, &triangles) else {
            unreachable!("a valid corpus");
        };
        let oracle = BruteForce(source(&vertices, &triangles));
        assert_eq!(scene.triangle_count(), oracle.triangle_count());
        let (mut rays, mut hits) = (0usize, 0usize);
        for _ in 0..4000 {
            let origin = [
                offset[0] + rng.unit() * 70.0 - 35.0,
                offset[1] + rng.unit() * 70.0 - 35.0,
                offset[2] + rng.unit() * 6.0,
            ];
            let direction = [
                rng.unit() * 2.0 - 1.0,
                rng.unit() * 2.0 - 1.0,
                rng.unit() * 2.0 - 1.0,
            ];
            let (tmin, tmax) = match rays % 4 {
                0 => (0.0, 0.0),
                1 => (0.001, 0.0),
                2 => (2.0, 0.0),
                _ => (0.0, rng.unit() * 20.0),
            };
            let expected = occluded(&oracle, origin, direction, tmin, tmax);
            assert_eq!(
                occluded(&scene, origin, direction, tmin, tmax),
                expected,
                "{origin:?} {direction:?} {tmin} {tmax}"
            );
            rays += 1;
            hits += usize::from(expected);
        }
        assert!(
            hits > 0 && hits < rays,
            "the corpus must have both outcomes ({boxes} boxes)"
        );
    }
}

#[test]
fn axis_aligned_rays_along_faces_and_edges_agree() {
    let mut rng = Rng(0x51ed_270b_27a3_ba95);
    let (vertices, triangles) = corpus(&mut rng, 60, [0.0; 3]);
    let Ok(scene) = build(&vertices, &triangles) else {
        unreachable!("a valid corpus");
    };
    let oracle = BruteForce(source(&vertices, &triangles));
    for &[x, y, z] in vertices.as_chunks::<3>().0.iter().take(200) {
        for direction in [
            [0.0, 0.0, 1.0],
            [1.0, 0.0, 0.0],
            [0.0, -1.0, 0.0],
            [-0.0, 0.0, -1.0],
        ] {
            let origin = [x, y, z - 0.5];
            assert_eq!(
                occluded(&scene, origin, direction, 0.0, 0.0),
                occluded(&oracle, origin, direction, 0.0, 0.0),
                "{origin:?} {direction:?}"
            );
        }
    }
}

#[test]
fn the_build_is_deterministic_and_the_batch_matches_single_rays() {
    let mut rng = Rng(0x1234_5678_9abc_def1);
    let (vertices, triangles) = corpus(&mut rng, 300, [0.0; 3]);
    let input = SceneInput {
        vertices: &vertices,
        triangles: &triangles,
        version: 3,
    };
    let (Ok(a), Ok(b)) = (Scene::build(&input), Scene::build(&input)) else {
        unreachable!("a valid corpus");
    };
    assert_eq!(a.ids, b.ids);
    assert_eq!(a.node_count(), b.node_count());
    assert!(a.node_count() > 100, "a real tree, not one leaf");
    assert_eq!(a.version(), 3);
    let mut origins = Vec::new();
    for j in 0..90 {
        for i in 0..90 {
            origins.extend([f64::from(i) * 0.7 - 31.0, f64::from(j) * 0.7 - 31.0, 0.1]);
        }
    }
    let query = DirectionalQuery {
        origins: &origins,
        direction: [0.4, 0.25, 0.6],
        tmin: 0.001,
        tmax: 0.0,
        max_parallel: 0,
    };
    let mut out = vec![0u8; 8100];
    assert_eq!(occlude_directional(&a, &query, &mut out, None), Ok(true));
    for (&[x, y, z], &bit) in origins.as_chunks::<3>().0.iter().zip(&out) {
        assert_eq!(
            bit,
            u8::from(occluded(
                &a,
                [x, y, z],
                query.direction,
                query.tmin,
                query.tmax
            ))
        );
    }
}
