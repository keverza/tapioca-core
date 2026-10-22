use super::*;

fn invoke(request: &TerrainSurfaceRequest, output: &TerrainOutput, error: &mut [u8]) -> i32 {
    // SAFETY: test descriptors/pools are live; intentional alias/stale/capacity
    // cases must be rejected by preflight before any foreign borrow or write.
    unsafe {
        tapioca_terrain_surface(
            &raw const *request,
            &raw const *output,
            error.as_mut_ptr(),
            error.len(),
        )
    }
}

const POINTS: [f64; 18] = [
    0., 0., 1., 10., 0., 1., 10., 10., 2., 0., 10., 1., 0., 0., 1., 10., 10., 2.,
];
const TRIANGLES: [u32; 9] = [0, 1, 2, 4, 5, 3, 0, 2, 1]; // last face is downward

#[test]
fn surface_layout_query_fill_support_and_atomic_refusals() {
    assert_eq!(size_of::<TerrainSurfaceRequest>(), 312);
    assert_eq!(std::mem::offset_of!(TerrainSurfaceRequest, provenance), 32);
    let counts = id::EntityCounts {
        vertices: 6,
        edges: 0,
        faces: 3,
        patches: 0,
    };
    let rows = rows(counts);
    let mut request = TerrainSurfaceRequest {
        points: view(&POINTS),
        triangles: view(&TRIANGLES),
        provenance: provenance(&rows, counts, 5),
    };
    let mut counts = TerrainCounts::default();
    let mut output = query(&mut counts);
    let mut error = [0_u8; 256];
    // SAFETY: live disjoint POD/pools and output storage for the call.
    let status = invoke(&request, &output, &mut error);
    assert_eq!(status, 0);
    assert_eq!(
        counts,
        TerrainCounts {
            points: 12,
            triangles: 6,
            mappings: 8
        }
    );
    let mut points = [99.; 15];
    let mut triangles = [99; 9];
    let sentinel = TerrainMapping {
        kind: 99,
        index: 99,
        values: [99; 6],
    };
    let mut mappings = [sentinel; 9];
    output.points = points.as_mut_ptr();
    output.point_capacity = points.len();
    output.triangles = triangles.as_mut_ptr();
    output.triangle_capacity = triangles.len();
    output.mappings = mappings.as_mut_ptr();
    output.mapping_capacity = mappings.len();
    output.query_only = 0;
    // SAFETY: all full capacities are live and disjoint, including unused tails.
    assert_eq!(invoke(&request, &output, &mut error), 0);
    assert_eq!(points.get(..12), POINTS.get(..12));
    assert_eq!(triangles, [0, 1, 2, 0, 2, 3, 99, 99, 99]);
    assert_eq!(mappings[4].kind, 18);
    assert_eq!((mappings[4].index, mappings[4].values[0]), (0, 4));
    assert_eq!((mappings[6].kind, mappings[6].values[0]), (19, 0));
    assert_eq!(mappings[8], sentinel);
    let before = (points, triangles, mappings, counts);
    output.mapping_capacity = 1;
    // SAFETY: well-formed but too-small output; no write is allowed.
    assert_ne!(invoke(&request, &output, &mut error), 0);
    assert_eq!((points, triangles, mappings, counts), before);
    output.mapping_capacity = mappings.len();
    request.provenance.expected_revision += 1;
    // SAFETY: stale attribution is a semantic refusal with live memory.
    assert_eq!(invoke(&request, &output, &mut error), 2);
    assert_eq!((points, triangles, mappings, counts), before);
    request.provenance.expected_revision -= 1;
    output.points = request.points.data.cast_mut();
    // SAFETY: deliberately aliased input/output must refuse before borrowing.
    assert_eq!(invoke(&request, &output, &mut error), 1);
    assert_eq!((points, triangles, mappings, counts), before);
}
