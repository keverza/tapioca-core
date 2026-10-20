use super::*;
use std::ptr::{null, null_mut};

fn view<T>(values: &[T]) -> SliceView<T> {
    SliceView {
        data: values.as_ptr(),
        count: values.len(),
    }
}

struct AnalysisFixture {
    summary: MeshAnalysisSummary,
    normals: [f32; 12],
    zero: [u8; 2],
    masks: [u8; 2],
}
impl AnalysisFixture {
    fn new() -> Self {
        Self {
            summary: MeshAnalysisSummary {
                bounds: Aabb {
                    mn: [42.0; 3],
                    mx: [42.0; 3],
                },
                normals_usable: 42,
            },
            normals: [42.0; 12],
            zero: [42; 2],
            masks: [42; 2],
        }
    }
    fn output(&mut self) -> MeshAnalysisOutput {
        MeshAnalysisOutput {
            summary: &raw mut self.summary,
            smooth_normals: self.normals.as_mut_ptr(),
            scalar_capacity: self.normals.len(),
            roundoff_zero: self.zero.as_mut_ptr(),
            wire_masks: self.masks.as_mut_ptr(),
            triangle_capacity: self.zero.len(),
        }
    }
    fn unchanged(&self) {
        assert_eq!(self.summary.normals_usable, 42);
        assert_eq!(self.normals, [42.0; 12]);
        assert_eq!(self.zero, [42; 2]);
        assert_eq!(self.masks, [42; 2]);
    }
}
fn request() -> MeshAnalysisRequest {
    MeshAnalysisRequest {
        positions: view(&[0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]),
        normals: view(&[]),
        triangles: view(&[0, 1, 2]),
        visible_edges: view(&[]),
        edge_ranges: view(&[]),
    }
}

#[test]
fn analysis_publishes_all_prefixes_and_preserves_tails() {
    let mut fixture = AnalysisFixture::new();
    let mut error = [99; 64];
    // SAFETY: owned live disjoint input/output arrays and descriptors.
    let status = unsafe {
        tapioca_analyze_mesh(
            &request(),
            &fixture.output(),
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(status, 0);
    assert_eq!(fixture.summary.normals_usable, 0);
    assert_eq!(
        fixture.normals,
        [
            0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 42.0, 42.0, 42.0
        ]
    );
    assert_eq!(fixture.zero, [0, 42]);
    assert_eq!(fixture.masks, [0, 42]);
    assert_eq!(error.first(), Some(&0));
}

#[test]
fn analysis_failures_preserve_all_outputs() {
    let mut fixture = AnalysisFixture::new();
    let normal = fixture.output();
    for input in [
        MeshAnalysisRequest {
            positions: SliceView {
                data: null(),
                count: 3,
            },
            ..request()
        },
        MeshAnalysisRequest {
            triangles: view(&[0, 1, 99]),
            ..request()
        },
        MeshAnalysisRequest {
            positions: view(&[f64::NAN; 9]),
            ..request()
        },
        MeshAnalysisRequest {
            edge_ranges: view(&[prep::EdgeRange {
                offset: u64::MAX,
                count: 1,
            }]),
            ..request()
        },
    ] {
        // SAFETY: malformed pointers are null and rejected before dereference;
        // other input/output allocations remain valid and disjoint.
        let status =
            unsafe { tapioca_analyze_mesh(&raw const input, &raw const normal, null_mut(), 0) };
        assert_eq!(status, 1);
        fixture.unchanged();
    }
    for output in [
        MeshAnalysisOutput {
            scalar_capacity: 8,
            ..normal
        },
        MeshAnalysisOutput {
            triangle_capacity: 0,
            ..normal
        },
        MeshAnalysisOutput {
            summary: null_mut(),
            ..normal
        },
        MeshAnalysisOutput {
            scalar_capacity: usize::MAX,
            ..normal
        },
    ] {
        // SAFETY: malformed ranges are rejected, no invalid allocation is read.
        let status = unsafe { tapioca_analyze_mesh(&request(), &raw const output, null_mut(), 0) };
        assert_eq!(status, 1);
        fixture.unchanged();
    }
}

#[test]
fn analysis_aliases_and_error_overlap_never_write() {
    let mut fixture = AnalysisFixture::new();
    let normal = fixture.output();
    let input = request();
    for output in [
        MeshAnalysisOutput {
            wire_masks: normal.roundoff_zero,
            ..normal
        },
        MeshAnalysisOutput {
            smooth_normals: input.positions.data.cast_mut().cast(),
            ..normal
        },
        MeshAnalysisOutput {
            summary: (&raw const input).cast_mut().cast(),
            ..normal
        },
    ] {
        // SAFETY: intentional alias is detected before writing or borrowing.
        let status =
            unsafe { tapioca_analyze_mesh(&raw const input, &raw const output, null_mut(), 0) };
        assert_eq!(status, 1);
        fixture.unchanged();
    }
    // Even an otherwise malformed range cannot let diagnostics corrupt an output.
    let output = MeshAnalysisOutput {
        summary: null_mut(),
        ..normal
    };
    // SAFETY: error aliases live output; preflight returns without writing it.
    let status =
        unsafe { tapioca_analyze_mesh(&raw const input, &raw const output, normal.wire_masks, 2) };
    assert_eq!(status, 1);
    fixture.unchanged();
    // Full-capacity tail overlap, not just the used prefix, must refuse too.
    let mut shared = [42; 3];
    let output = MeshAnalysisOutput {
        roundoff_zero: shared.as_mut_ptr(),
        wire_masks: shared.as_mut_ptr().wrapping_add(1),
        triangle_capacity: 2,
        ..normal
    };
    // This overlaps only if roundoff capacity includes its second unused element.
    // SAFETY: overlapping regions fit the live shared allocation and must refuse.
    let status =
        unsafe { tapioca_analyze_mesh(&raw const input, &raw const output, null_mut(), 0) };
    assert_eq!(status, 1);
    assert_eq!(shared, [42; 3]);
    fixture.unchanged();
}

#[test]
fn weld_success_and_refusal_are_atomic() {
    let corners = [prep::WeldCorner {
        source_vertex: 7,
        position: [1.0, 2.0, 3.0],
        normal: [0.0, 0.0, 1.0],
    }; 2];
    let input = WeldRequest {
        corners: view(&corners),
        body_offsets: view(&[0, 2]),
    };
    let mut count = 42;
    let mut positions = [42.0; 6];
    let mut normals = [42.0; 6];
    let mut map = [42; 2];
    let output = WeldOutput {
        vertex_count: &raw mut count,
        positions: positions.as_mut_ptr(),
        normals: normals.as_mut_ptr(),
        scalar_capacity: 6,
        corner_vertices: map.as_mut_ptr(),
        corner_capacity: 2,
    };
    for invalid in [
        WeldOutput {
            corner_capacity: 1,
            ..output
        },
        WeldOutput {
            scalar_capacity: 5,
            ..output
        },
        WeldOutput {
            normals: positions.as_mut_ptr().cast(),
            ..output
        },
    ] {
        // SAFETY: live allocations, deliberately insufficient/overlapping outputs
        // are rejected before access; no foreign allocation validity claim.
        let status =
            unsafe { tapioca_weld_corners(&raw const input, &raw const invalid, null_mut(), 0) };
        assert_eq!(status, 1);
        assert_eq!(count, 42);
        assert_eq!(positions, [42.0; 6]);
        assert_eq!(normals, [42.0; 6]);
        assert_eq!(map, [42; 2]);
    }
    // SAFETY: owned live disjoint arrays/descriptors, sufficient capacities.
    let status =
        unsafe { tapioca_weld_corners(&raw const input, &raw const output, null_mut(), 0) };
    assert_eq!(status, 0);
    assert_eq!(count, 1);
    assert_eq!(positions, [1.0, 2.0, 3.0, 42.0, 42.0, 42.0]);
    assert_eq!(normals, [0.0, 0.0, 1.0, 42.0, 42.0, 42.0]);
    assert_eq!(map, [0, 0]);
}

#[test]
fn empty_batches_require_only_summary_count_and_offsets() {
    let mut count = 42;
    let output = WeldOutput {
        vertex_count: &raw mut count,
        positions: null_mut(),
        normals: null_mut(),
        scalar_capacity: 0,
        corner_vertices: null_mut(),
        corner_capacity: 0,
    };
    let input = WeldRequest {
        corners: view(&[]),
        body_offsets: view(&[0]),
    };
    // SAFETY: zero-capacity arrays are never accessed; count is live writable.
    let status =
        unsafe { tapioca_weld_corners(&raw const input, &raw const output, null_mut(), 0) };
    assert_eq!(status, 0);
    assert_eq!(count, 0);
    let mut summary = AnalysisFixture::new().summary;
    let output = MeshAnalysisOutput {
        summary: &raw mut summary,
        smooth_normals: null_mut(),
        scalar_capacity: 0,
        roundoff_zero: null_mut(),
        wire_masks: null_mut(),
        triangle_capacity: 0,
    };
    let input = MeshAnalysisRequest {
        positions: view(&[]),
        triangles: view(&[]),
        ..request()
    };
    // SAFETY: empty arrays and one live disjoint output record.
    let status =
        unsafe { tapioca_analyze_mesh(&raw const input, &raw const output, null_mut(), 0) };
    assert_eq!(status, 0);
    assert_eq!(summary.normals_usable, 0);
    assert_eq!(summary.bounds.mn, [f64::INFINITY; 3]);
}

#[test]
fn repr_c_x64_layout_matches_the_independent_c_translation_unit() {
    assert_eq!(size_of::<prep::WeldCorner>(), 56);
    assert_eq!(std::mem::offset_of!(prep::WeldCorner, position), 8);
    assert_eq!(size_of::<MeshAnalysisSummary>(), 56);
    assert_eq!(
        std::mem::offset_of!(MeshAnalysisSummary, normals_usable),
        48
    );
    assert_eq!(size_of::<prep::EdgeRange>(), 16);
}

#[test]
fn all_analysis_input_views_and_descriptors_are_protected_from_diagnostics() {
    let mut fixture = AnalysisFixture::new();
    let output = fixture.output();
    let mut positions = [42.0_f64; 9];
    let mut normals = [42.0_f32; 9];
    let mut triangles = [0_u32; 3];
    let mut keys = [0_u64];
    let mut ranges = [prep::EdgeRange {
        offset: 0,
        count: 1,
    }];
    let mut input = MeshAnalysisRequest {
        positions: view(&positions),
        normals: view(&normals),
        triangles: view(&triangles),
        visible_edges: view(&keys),
        edge_ranges: view(&ranges),
    };
    let errors = [
        positions.as_mut_ptr().cast(),
        normals.as_mut_ptr().cast(),
        triangles.as_mut_ptr().cast(),
        keys.as_mut_ptr().cast(),
        ranges.as_mut_ptr().cast(),
        (&raw mut input).cast(),
    ];
    for error in errors {
        // SAFETY: intentional error/input alias; all backing storage remains live
        // and preflight refuses before forming foreign borrows or writing.
        let status = unsafe { tapioca_analyze_mesh(&raw const input, &raw const output, error, 1) };
        assert_eq!(status, 1);
        fixture.unchanged();
        assert_eq!(positions, [42.0; 9]);
        assert_eq!(normals, [42.0; 9]);
        assert_eq!(triangles, [0; 3]);
        assert_eq!(keys, [0]);
        assert_eq!(ranges.first().map(|v| v.count), Some(1));
        assert_eq!(input.positions.count, 9);
    }
}

#[test]
fn weld_overlap_in_unused_capacity_wins_over_another_bad_range() {
    let corners = [prep::WeldCorner {
        source_vertex: 1,
        position: [1.0; 3],
        normal: [0.0, 0.0, 1.0],
    }];
    let input = WeldRequest {
        corners: view(&corners),
        body_offsets: view(&[0, 1]),
    };
    let mut positions = [42.0_f64; 6];
    let mut count = 42;
    let mut map = [42];
    let output = WeldOutput {
        vertex_count: &raw mut count,
        positions: positions.as_mut_ptr(),
        normals: null_mut(),
        scalar_capacity: 6,
        corner_vertices: map.as_mut_ptr(),
        corner_capacity: 1,
    };
    let error = positions.as_mut_ptr().wrapping_add(3).cast();
    // SAFETY: error overlaps the unused position tail, while normals is malformed.
    // Complete preflight must suppress even the error write, no invalid access.
    let status = unsafe { tapioca_weld_corners(&raw const input, &raw const output, error, 8) };
    assert_eq!(status, 1);
    assert_eq!(positions, [42.0; 6]);
    assert_eq!(count, 42);
    assert_eq!(map, [42]);
}
