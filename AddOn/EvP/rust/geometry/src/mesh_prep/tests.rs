use super::*;

fn request<'a>(vertices: &'a [f64], triangles: &'a [u32]) -> AnalysisRequest<'a> {
    AnalysisRequest {
        mesh: MeshView {
            guid: "",
            elem_type: 0,
            vertices,
            triangles,
            normals: &[],
            tri_material: &[],
            tri_wire_edges: &[],
            tri_source_body: &[],
            tri_source_pgon: &[],
            tri_surface_attribute: &[],
        },
        visible_edges: &[],
        edge_ranges: &[],
    }
}

#[test]
fn independent_triangle_bounds_and_barycentric_bits() -> Result<(), Error> {
    let mut input = request(
        &[
            10.0, 20.0, 3.0, 11.0, 20.0, 3.0, 10.0, 21.0, 3.0, 100.0, 100.0, -2.0,
        ],
        &[0, 1, 2],
    );
    let edges = [wire_edge_key(0, 1), wire_edge_key(1, 2)];
    input.visible_edges = &edges;
    let result = analyze_mesh(&input)?;
    assert_eq!(
        result.bounds,
        Aabb {
            mn: [10.0, 20.0, -2.0],
            mx: [100.0, 100.0, 3.0]
        }
    );
    assert_eq!(
        result.smooth_normals,
        [0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
    );
    assert_eq!(result.wire_masks, [5]);
    assert_eq!(result.roundoff_zero, [0]);
    assert!(!result.normals_usable);
    Ok(())
}

#[test]
fn empty_and_zero_area_keep_vertices_and_classification() -> Result<(), Error> {
    let empty = analyze_mesh(&request(&[], &[]))?;
    assert!(empty.bounds.mn.iter().all(|v| *v == f64::INFINITY));
    assert!(empty.bounds.mx.iter().all(|v| *v == f64::NEG_INFINITY));
    assert_eq!(empty.smooth_normals, [] as [f32; 0]);
    let result = analyze_mesh(&request(&[0.0; 3], &[0, 0, 0]))?;
    assert_eq!(result.roundoff_zero, [1]);
    assert_eq!(result.smooth_normals, [0.0; 3]);
    Ok(())
}

#[test]
fn thresholds_preserve_small_surfaces_and_tiny_normal_fallback() -> Result<(), Error> {
    for (height, zero) in [
        (4.0 * f64::EPSILON, 1),
        (8.0 * f64::EPSILON, 1),
        (16.0 * f64::EPSILON, 0),
        (1e-10, 0),
    ] {
        let result = analyze_mesh(&request(
            &[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, height, 0.0],
            &[0, 1, 2],
        ))?;
        assert_eq!(result.roundoff_zero, [zero]);
    }
    let tiny = analyze_mesh(&request(
        &[0.0, 0.0, 0.0, 1e-8, 0.0, 0.0, 0.0, 1e-8, 0.0],
        &[0, 1, 2],
    ))?;
    assert_eq!(tiny.roundoff_zero, [0]);
    assert!(tiny.smooth_normals.iter().all(|v| *v >= 0.0 && *v < 1e-12));
    assert!(!roundoff_zero_triangle([[0.0; 3], [f64::NAN; 3], [0.0; 3]]));
    Ok(())
}

#[test]
fn ranges_are_local_sorted_pools_not_one_global_polygon() -> Result<(), Error> {
    let mut input = request(
        &[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
        &[0, 1, 2, 0, 1, 2],
    );
    let edges = [wire_edge_key(1, 2), wire_edge_key(0, 1)];
    input.visible_edges = &edges;
    assert!(matches!(analyze_mesh(&input), Err(Error::WireKeys)));
    input.edge_ranges = &[
        EdgeRange {
            offset: 0,
            count: 1,
        },
        EdgeRange {
            offset: 1,
            count: 1,
        },
    ];
    assert_eq!(analyze_mesh(&input)?.wire_masks, [1, 4]);
    input.edge_ranges = &[EdgeRange {
        offset: u64::MAX,
        count: 2,
    }; 2];
    assert!(matches!(analyze_mesh(&input), Err(Error::Shape)));
    Ok(())
}

#[test]
fn analysis_refuses_invalid_geometry_and_overflow() {
    for (vertices, triangles) in [
        (&[0.0, 1.0][..], &[][..]),
        (&[0.0; 3], &[1, 0, 0]),
        (&[f64::NAN; 3], &[]),
    ] {
        assert!(matches!(
            analyze_mesh(&request(vertices, triangles)),
            Err(Error::Mesh)
        ));
    }
    assert!(matches!(
        analyze_mesh(&request(
            &[0.0, 0.0, 0.0, 1e30, 0.0, 0.0, 0.0, 1e30, 0.0],
            &[0, 1, 2]
        )),
        Err(Error::Arithmetic)
    ));
    assert!(normals_are_usable(&[0.0, 0.0, 1.0], 3));
    assert!(!normals_are_usable(&[0.0; 3], 3));
}

fn corner(source_vertex: i32, normal: [f64; 3]) -> WeldCorner {
    WeldCorner {
        source_vertex,
        position: [1.0, 2.0, 3.0],
        normal,
    }
}

#[test]
fn welding_retains_first_values_all_contributors_and_body_resets() -> Result<(), Error> {
    let a = corner(1, [0.0, 0.0, 1.0]);
    let b = WeldCorner {
        position: [99.0; 3],
        normal: [0.000_049, 0.0, 1.0],
        ..a
    };
    let corners = [a, b, corner(1, [1.0, 0.0, 0.0]), a];
    let result = weld_corners(&WeldRequest {
        corners: &corners,
        body_offsets: &[0, 3, 3, 4],
    })?;
    assert_eq!(result.corner_vertices, [0, 0, 1, 2]);
    assert_eq!(result.positions, [1.0, 2.0, 3.0].repeat(3));
    assert_eq!(
        result.normals,
        [0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    );
    Ok(())
}

#[test]
fn welding_halfway_rounds_away_and_never_welds_by_position() -> Result<(), Error> {
    let corners = [
        corner(1, [0.000_05, 0.0, 1.0]),
        corner(1, [-0.000_05, 0.0, 1.0]),
        corner(2, [0.000_05, 0.0, 1.0]),
    ];
    assert_eq!(
        weld_corners(&WeldRequest {
            corners: &corners,
            body_offsets: &[0, 3]
        })?
        .corner_vertices,
        [0, 1, 2]
    );
    assert_eq!(
        weld_corners(&WeldRequest {
            corners: &[],
            body_offsets: &[0]
        })?
        .positions,
        [] as [f64; 0]
    );
    Ok(())
}

#[test]
fn welding_refuses_every_invalid_corner_even_deduplicated() {
    let a = corner(1, [0.0, 0.0, 1.0]);
    for offsets in [&[][..], &[1, 1], &[0, 2], &[0, 1, 0, 1]] {
        assert!(matches!(
            weld_corners(&WeldRequest {
                corners: &[a],
                body_offsets: offsets
            }),
            Err(Error::Shape)
        ));
    }
    for bad in [
        WeldCorner {
            position: [f64::NAN; 3],
            ..a
        },
        corner(1, [f64::INFINITY; 3]),
        corner(1, [f64::MAX; 3]),
        corner(1, [214_748.364_8, 0.0, 1.0]),
    ] {
        assert!(
            weld_corners(&WeldRequest {
                corners: &[a, bad],
                body_offsets: &[0, 2]
            })
            .is_err()
        );
    }
}
