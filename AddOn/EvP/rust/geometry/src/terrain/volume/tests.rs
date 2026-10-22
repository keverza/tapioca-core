use super::*;
use crate::terrain::edit::{Edit, EditRequest, Effect, Mode, Target, Transition, edit_terrain};

fn close(left: f64, right: f64, tolerance: f64) -> bool {
    (left - right).abs() <= tolerance
}

/// `side x side` vertices at `spacing`, heights `z(x, y)`.
fn grid(side: u32, spacing: f64, z: impl Fn(f64, f64) -> f64) -> (Vec<f64>, Vec<u32>) {
    let mut points = Vec::new();
    for row in 0..side {
        for column in 0..side {
            let (x, y) = (f64::from(column) * spacing, f64::from(row) * spacing);
            points.extend([x, y, z(x, y)]);
        }
    }
    let mut triangles = Vec::new();
    for row in 0..side - 1 {
        for column in 0..side - 1 {
            let a = row * side + column;
            triangles.extend([a, a + 1, a + side + 1, a, a + side + 1, a + side]);
        }
    }
    (points, triangles)
}

fn field(side: u32, spacing: f64, z: impl Fn(f64, f64) -> f64) -> Result<Field, VolumeError> {
    let (points, triangles) = grid(side, spacing, z);
    Field::new(&points, &triangles).map_err(|_| VolumeError::Shape)
}

fn request<'a>(existing: &'a Field, proposed: Proposed<'a>, cell: f64) -> VolumeRequest<'a> {
    VolumeRequest {
        existing,
        proposed,
        grid: Grid {
            cell,
            subdivisions: 1,
        },
        domain: None,
        property: None,
    }
}

#[test]
fn a_plane_offset_is_area_times_offset() -> Result<(), VolumeError> {
    let existing = field(11, 1.0, |_, _| 0.0)?;
    let report = cut_fill(&request(&existing, Proposed::Plane(2.0), 1.0))?;
    assert_eq!(report.cells, [10, 10]);
    assert!(close(report.site.fill, 200.0, 1e-9) && close(report.site.cut, 0.0, 0.0));
    assert!(close(report.site.net, 200.0, 1e-9) && close(report.site.area, 100.0, 1e-9));
    assert_eq!((report.site.samples, report.site.excluded), (100, 0));
    assert_eq!(report.context, Volumes::default());
    assert_eq!(report.edits, Vec::new());
    Ok(())
}

#[test]
fn mixed_cut_and_fill_net_to_their_difference() -> Result<(), VolumeError> {
    let existing = field(11, 1.0, |x, _| x - 5.0)?;
    let report = cut_fill(&request(&existing, Proposed::Plane(0.0), 1.0))?;
    // Centres at x = i + 0.5: fill 10 * (4.5 + 3.5 + 2.5 + 1.5 + 0.5), cut the same.
    assert!(close(report.site.fill, 125.0, 1e-9) && close(report.site.cut, 125.0, 1e-9));
    assert!(close(report.site.net, 0.0, 1e-9));
    Ok(())
}

#[test]
fn partial_cells_keep_the_area_bookkeeping_exact() -> Result<(), VolumeError> {
    let existing = field(22, 0.5, |_, _| 1.0)?; // [0, 10.5]^2
    let report = cut_fill(&request(&existing, Proposed::Plane(3.0), 2.0))?;
    assert_eq!(report.cells, [6, 6]); // 10.5 / 2 = 5.25 -> 6, last cells 0.5 m
    assert!(close(report.site.area, 10.5 * 10.5, 1e-9));
    assert!(close(report.site.fill, 2.0 * 10.5 * 10.5, 1e-9));
    // 10 / 0.1 is 99.999...: within 1e-12 of 100, so exactly 100 cells.
    let ten = field(11, 1.0, |_, _| 0.0)?;
    assert_eq!(
        cut_fill(&request(&ten, Proposed::Plane(1.0), 0.1))?.cells,
        [100, 100]
    );
    Ok(())
}

#[test]
fn subdivisions_sample_finer_with_the_same_area() -> Result<(), VolumeError> {
    let existing = field(11, 1.0, |_, _| 0.0)?;
    let peak = field(11, 1.0, |x, y| {
        (3.0 - (x - 5.0).abs() - (y - 5.0).abs()).max(0.0)
    })?;
    let coarse = cut_fill(&request(&existing, Proposed::Field(&peak), 2.0))?;
    let fine = cut_fill(&VolumeRequest {
        grid: Grid {
            cell: 2.0,
            subdivisions: 4,
        },
        ..request(&existing, Proposed::Field(&peak), 2.0)
    })?;
    assert_eq!(fine.site.samples, coarse.site.samples * 16);
    assert!(close(fine.site.area, coarse.site.area, 1e-9));
    // Finer sampling converges to the exact integral of the piecewise-linear mesh:
    // sum over faces of plan area times the mean corner height.
    let (points, triangles) = grid(11, 1.0, |x, y| {
        (3.0 - (x - 5.0).abs() - (y - 5.0).abs()).max(0.0)
    });
    let at = |v: u32| {
        let start = v as usize * 3;
        match points.get(start..start + 3) {
            Some(&[x, y, z]) => [x, y, z],
            _ => [f64::NAN; 3],
        }
    };
    let exact: f64 = triangles
        .as_chunks::<3>()
        .0
        .iter()
        .map(|&[a, b, c]| {
            let ([ax, ay, az], [bx, by, bz], [cx, cy, cz]) = (at(a), at(b), at(c));
            0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) * (az + bz + cz) / 3.0
        })
        .sum();
    let finest = cut_fill(&VolumeRequest {
        grid: Grid {
            cell: 1.0,
            subdivisions: 16,
        },
        ..request(&existing, Proposed::Field(&peak), 1.0)
    })?;
    let (coarse_error, fine_error) = (
        (coarse.site.fill - exact).abs(),
        (finest.site.fill - exact).abs(),
    );
    assert!(fine_error < coarse_error, "{fine_error} vs {coarse_error}");
    assert!(
        fine_error <= 1e-3 * exact,
        "{} vs {exact}",
        finest.site.fill
    );
    Ok(())
}

#[test]
fn outside_the_property_line_is_context_not_dropped() -> Result<(), VolumeError> {
    let existing = field(11, 1.0, |_, _| 0.0)?;
    let ring = [2.0, 2.0, 0.0, 8.0, 2.0, 0.0, 8.0, 8.0, 0.0, 2.0, 8.0, 0.0];
    let report = cut_fill(&VolumeRequest {
        property: Some(&ring),
        ..request(&existing, Proposed::Plane(1.0), 1.0)
    })?;
    assert_eq!((report.site.samples, report.context.samples), (36, 64));
    assert!(close(report.site.fill, 36.0, 1e-9) && close(report.context.fill, 64.0, 1e-9));
    Ok(())
}

#[test]
fn samples_off_either_surface_are_excluded_and_counted() -> Result<(), VolumeError> {
    let existing = field(11, 1.0, |_, _| 0.0)?;
    let small = field(6, 1.0, |_, _| 1.0)?; // [0, 5]^2
    let report = cut_fill(&request(&existing, Proposed::Field(&small), 1.0))?;
    assert_eq!((report.site.samples, report.site.excluded), (25, 75));
    assert!(close(report.site.fill, 25.0, 1e-9));
    Ok(())
}

#[test]
fn edited_volumes_are_attributed_per_edit_and_reconcile_exactly() -> Result<(), VolumeError> {
    let (points, triangles) = grid(21, 1.0, |x, y| 0.1 * x + 0.05 * y);
    let edits = [
        Edit {
            target: Target::Point {
                position: [6.0, 10.0, 1.5],
                radius: 1.0,
            },
            mode: Mode::Relative,
            effect: Effect::Both,
            transition: Transition::Falloff { blur: 3.0 },
            max_distance: 10.0,
            strength: 1.0,
        },
        Edit {
            target: Target::Point {
                position: [13.0, 10.0, -1.0],
                radius: 0.0,
            },
            mode: Mode::Relative,
            effect: Effect::Both,
            transition: Transition::Falloff { blur: 4.0 },
            max_distance: 10.0,
            strength: 1.0,
        },
    ];
    let edited = edit_terrain(&EditRequest {
        points: &points,
        triangles: &triangles,
        edits: &edits,
        clip: None,
        protected: &[],
        smoothing: None,
    })
    .map_err(|_| VolumeError::Shape)?;
    let existing = Field::new(&points, &triangles).map_err(|_| VolumeError::Shape)?;
    let proposed = Proposed::Edited {
        heights: &edited.heights,
        owners: &edited.owners,
    };
    let report = cut_fill(&VolumeRequest {
        grid: Grid {
            cell: 0.5,
            subdivisions: 2,
        },
        ..request(&existing, proposed, 0.5)
    })?;
    assert_eq!(
        report.edits.iter().map(|row| row.edit).collect::<Vec<_>>(),
        [0, 1]
    );
    let net: f64 = report.edits.iter().map(|row| row.net).sum();
    assert!(
        close(net, report.site.net, 1e-9),
        "{net} vs {}",
        report.site.net
    );
    assert!(
        report
            .edits
            .first()
            .is_some_and(|row| row.fill > 0.0 && close(row.cut, 0.0, 1e-12))
    );
    assert!(
        report
            .edits
            .get(1)
            .is_some_and(|row| row.cut > 0.0 && close(row.fill, 0.0, 1e-12))
    );
    // The same volumes from the edited surface as an independent field.
    let mut moved = points;
    for (slot, z) in moved.iter_mut().skip(2).step_by(3).zip(&edited.heights) {
        *slot = *z;
    }
    let surface = Field::new(&moved, &triangles).map_err(|_| VolumeError::Shape)?;
    let independent = cut_fill(&VolumeRequest {
        grid: Grid {
            cell: 0.5,
            subdivisions: 2,
        },
        ..request(&existing, Proposed::Field(&surface), 0.5)
    })?;
    assert!(close(independent.site.cut, report.site.cut, 1e-9));
    assert!(close(independent.site.fill, report.site.fill, 1e-9));
    Ok(())
}

#[test]
fn malformed_requests_are_refused_by_name() -> Result<(), VolumeError> {
    let existing = field(3, 1.0, |_, _| 0.0)?;
    let base = request(&existing, Proposed::Plane(1.0), 1.0);
    let refuse = |request: VolumeRequest<'_>| cut_fill(&request).map(|_| ());
    assert_eq!(
        refuse(VolumeRequest {
            grid: Grid {
                cell: 0.0,
                subdivisions: 1
            },
            ..base
        }),
        Err(VolumeError::Grid)
    );
    assert_eq!(
        refuse(VolumeRequest {
            grid: Grid {
                cell: 1.0,
                subdivisions: 0
            },
            ..base
        }),
        Err(VolumeError::Grid)
    );
    assert_eq!(
        refuse(VolumeRequest {
            domain: Some([0.0, 0.0, 0.0, 1.0]),
            ..base
        }),
        Err(VolumeError::Domain)
    );
    assert_eq!(
        refuse(VolumeRequest {
            proposed: Proposed::Plane(f64::NAN),
            ..base
        }),
        Err(VolumeError::Domain)
    );
    let heights = [0.0; 2];
    let owners = [None; 9];
    assert_eq!(
        refuse(VolumeRequest {
            proposed: Proposed::Edited {
                heights: &heights,
                owners: &owners
            },
            ..base
        }),
        Err(VolumeError::Shape)
    );
    let ring = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0];
    assert_eq!(
        refuse(VolumeRequest {
            property: Some(&ring),
            ..base
        }),
        Err(VolumeError::Property)
    );
    for error in [
        VolumeError::Grid,
        VolumeError::Domain,
        VolumeError::Shape,
        VolumeError::Property,
    ] {
        assert!(error.message().starts_with("Volume"));
    }
    Ok(())
}
