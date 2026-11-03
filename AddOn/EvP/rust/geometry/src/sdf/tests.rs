use super::*;

fn point(x: f64, y: f64, z: f64) -> Vec3 {
    Vec3 { x, y, z }
}

fn operation(id: u64, primitive: Primitive) -> StackOperation {
    StackOperation {
        id,
        primitive,
        operation: BooleanOperation::Union,
        profile: BlendProfile::Sharp,
        blend_radius: 0.0,
        material_id: i32::try_from(id).unwrap_or(i32::MAX),
        enabled: true,
    }
}

fn sphere(id: u64, center: Vec3, radius: f64) -> StackOperation {
    operation(id, Primitive::Sphere { center, radius })
}

fn capsule(id: u64, a: Vec3, b: Vec3, radius: f64) -> StackOperation {
    operation(id, Primitive::Capsule { a, b, radius })
}

fn entry(operation: StackOperation) -> StackEntry {
    StackEntry::Operation(operation)
}

fn compile(entries: &[StackEntry]) -> Result<PreparedField, Error> {
    PreparedField::compile(entries, CompileOptions::default())
}

fn close(actual: f64, expected: f64, tolerance: f64) {
    assert!(
        (actual - expected).abs() <= tolerance,
        "actual {actual:.17e}, expected {expected:.17e}"
    );
}

#[test]
fn analytic_primitives_match_closed_form_values() -> Result<(), Error> {
    let sphere = compile(&[entry(sphere(1, point(1.0, 2.0, 3.0), 2.0))])?;
    close(sphere.evaluate(point(1.0, 2.0, 3.0))?.distance, -2.0, 0.0);
    close(sphere.evaluate(point(4.0, 2.0, 3.0))?.distance, 1.0, 0.0);

    let capsule = compile(&[entry(capsule(
        1,
        point(-1.0, 0.0, 0.0),
        point(1.0, 0.0, 0.0),
        0.5,
    ))])?;
    close(capsule.evaluate(point(0.0, 0.0, 0.0))?.distance, -0.5, 0.0);
    close(capsule.evaluate(point(2.0, 0.0, 0.0))?.distance, 0.5, 0.0);

    let rounded_box = Primitive::RoundedBox {
        frame: Frame::IDENTITY,
        half: point(2.0, 1.0, 0.5),
        radius: 0.2,
    };
    let rounded_box = compile(&[entry(operation(1, rounded_box))])?;
    close(
        rounded_box.evaluate(point(0.0, 0.0, 0.0))?.distance,
        -0.5,
        1e-15,
    );
    close(
        rounded_box.evaluate(point(3.0, 0.0, 0.0))?.distance,
        1.0,
        1e-15,
    );

    let extrude = Primitive::Extrude {
        frame: Frame::IDENTITY,
        profile: vec![[-1.0, -1.0], [1.0, -1.0], [1.0, 1.0], [-1.0, 1.0]],
        half_height: 2.0,
    };
    let extrude = compile(&[entry(operation(1, extrude))])?;
    close(extrude.evaluate(point(0.0, 0.0, 0.0))?.distance, -1.0, 0.0);
    close(extrude.evaluate(point(2.0, 0.0, 0.0))?.distance, 1.0, 0.0);
    close(
        extrude.evaluate(point(2.0, 0.0, 3.0))?.distance,
        2.0_f64.sqrt(),
        1e-15,
    );
    Ok(())
}

#[test]
fn rigid_frames_rotate_metric_primitives_without_hidden_scaling() -> Result<(), Error> {
    let frame = Frame {
        origin: point(10.0, 20.0, 30.0),
        axes: [
            point(0.0, 1.0, 0.0),
            point(-1.0, 0.0, 0.0),
            point(0.0, 0.0, 1.0),
        ],
    };
    let field = compile(&[entry(operation(
        1,
        Primitive::RoundedBox {
            frame,
            half: point(1.0, 2.0, 3.0),
            radius: 0.0,
        },
    ))])?;
    close(field.evaluate(frame.origin)?.distance, -1.0, 0.0);
    close(field.evaluate(point(10.0, 22.0, 30.0))?.distance, 1.0, 0.0);
    close(field.evaluate(point(12.0, 20.0, 30.0))?.distance, 0.0, 0.0);
    let bounds = field.bounds().ok_or(Error::EmptyStack)?;
    assert_eq!(bounds.min, point(8.0, 19.0, 27.0));
    assert_eq!(bounds.max, point(12.0, 21.0, 33.0));
    Ok(())
}

#[test]
fn first_enabled_operation_and_subtract_only_groups_refuse() {
    let mut subtract = sphere(2, point(0.0, 0.0, 0.0), 1.0);
    subtract.operation = BooleanOperation::Subtract;
    assert_eq!(
        compile(&[entry(subtract.clone())]),
        Err(Error::FirstOperationNotUnion(2))
    );

    let mut disabled = sphere(1, point(0.0, 0.0, 0.0), 1.0);
    disabled.enabled = false;
    assert_eq!(
        compile(&[entry(disabled), entry(subtract.clone())]),
        Err(Error::FirstOperationNotUnion(2))
    );

    let group = StackEntry::Group(GroupHeader {
        id: 10,
        enabled: true,
        isolated: true,
        profile: BlendProfile::Round,
        blend_radius: 0.2,
    });
    assert_eq!(
        compile(&[
            entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)),
            group,
            entry(subtract)
        ]),
        Err(Error::FirstOperationNotUnion(2))
    );
}

#[test]
fn isolated_subtraction_cannot_cut_another_group() -> Result<(), Error> {
    let base = entry(sphere(1, point(0.0, 0.0, 0.0), 2.0));
    let cutter = capsule(3, point(1.5, 0.0, 0.0), point(1.5, 0.0, 1.0), 0.75);
    let mut cutter = cutter;
    cutter.operation = BooleanOperation::Subtract;
    let make = |isolated| {
        vec![
            base.clone(),
            StackEntry::Group(GroupHeader {
                id: 10,
                enabled: true,
                isolated,
                profile: BlendProfile::Sharp,
                blend_radius: 0.0,
            }),
            entry(sphere(2, point(5.0, 0.0, 0.0), 1.0)),
            entry(cutter.clone()),
        ]
    };
    let query = point(1.5, 0.0, 0.0);
    assert!(compile(&make(true))?.evaluate(query)?.distance < 0.0);
    assert!(compile(&make(false))?.evaluate(query)?.distance > 0.0);
    Ok(())
}

#[test]
fn disabled_and_empty_groups_skip_and_solo_truncates() -> Result<(), Error> {
    let mut subtract = sphere(2, point(0.0, 0.0, 0.0), 1.0);
    subtract.operation = BooleanOperation::Subtract;
    let disabled_group = StackEntry::Group(GroupHeader {
        id: 10,
        enabled: false,
        isolated: true,
        profile: BlendProfile::Sharp,
        blend_radius: 0.0,
    });
    let field = compile(&[
        entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)),
        disabled_group,
        entry(subtract.clone()),
    ])?;
    close(field.evaluate(point(0.0, 0.0, 0.0))?.distance, -2.0, 0.0);

    let solo = PreparedField::compile(
        &[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(subtract)],
        CompileOptions {
            solo_operation: Some(1),
            max_operations: 256,
        },
    )?;
    close(solo.evaluate(point(0.0, 0.0, 0.0))?.distance, -2.0, 0.0);
    assert_eq!(
        PreparedField::compile(
            &[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0))],
            CompileOptions {
                solo_operation: Some(99),
                max_operations: 256,
            },
        ),
        Err(Error::SoloOperationUnavailable)
    );
    Ok(())
}

#[test]
fn sharp_and_round_profiles_have_pinned_blend_values() -> Result<(), Error> {
    for (profile, expected) in [
        (BlendProfile::Sharp, -2.0 - 0.4 / 6.0),
        (BlendProfile::Round, -2.1),
    ] {
        let mut second = sphere(2, point(0.0, 0.0, 0.0), 2.0);
        second.profile = profile;
        second.blend_radius = 0.4;
        let field = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(second)])?;
        close(
            field.evaluate(point(0.0, 0.0, 0.0))?.distance,
            expected,
            1e-15,
        );
    }
    Ok(())
}

#[test]
fn material_owner_is_hard_ordered_and_survives_every_operation() -> Result<(), Error> {
    for boolean in [
        BooleanOperation::Union,
        BooleanOperation::Subtract,
        BooleanOperation::Intersect,
    ] {
        let mut later = sphere(2, point(0.0, 0.0, 0.0), 2.0);
        later.operation = boolean;
        later.profile = BlendProfile::Round;
        later.blend_radius = 0.4;
        let field = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(later)])?;
        assert_eq!(field.evaluate(point(2.0, 0.0, 0.0))?.material_id, 2);
    }

    let mut cutter = sphere(2, point(1.5, 0.0, 0.0), 1.0);
    cutter.operation = BooleanOperation::Subtract;
    let field = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(cutter)])?;
    assert_eq!(field.evaluate(point(-2.0, 0.0, 0.0))?.material_id, 1);
    assert_eq!(field.evaluate(point(0.5, 0.0, 0.0))?.material_id, 2);
    Ok(())
}

#[test]
fn batch_and_identity_pruning_are_bit_exact() -> Result<(), Error> {
    let mut second = sphere(2, point(1.0, 0.0, 0.0), 1.5);
    second.profile = BlendProfile::Round;
    second.blend_radius = 0.3;
    let field = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(second)])?;
    let points = (0..64)
        .map(|index| {
            let value = f64::from(index) / 16.0;
            point(value - 2.0, 0.25 * value, -0.1 * value)
        })
        .collect::<Vec<_>>();
    let batch = field.evaluate_batch(&points)?;
    for (&point, sample) in points.iter().zip(batch) {
        let scalar = field.evaluate(point)?;
        assert_eq!(sample.distance.to_bits(), scalar.distance.to_bits());
        assert_eq!(sample.material_id, scalar.material_id);
    }
    let region = Bounds {
        min: point(-2.0, -1.0, -1.0),
        max: point(2.0, 2.0, 1.0),
    };
    let pruned = field.pruned(region)?;
    for point in points {
        assert_eq!(
            field.evaluate(point)?.distance.to_bits(),
            pruned.evaluate(point)?.distance.to_bits()
        );
    }
    Ok(())
}

#[test]
fn seeded_intervals_and_lipschitz_bounds_are_conservative() -> Result<(), Error> {
    let mut second = sphere(2, point(1.0, 0.5, 0.0), 1.25);
    second.profile = BlendProfile::Round;
    second.blend_radius = 0.4;
    let field = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(second)])?;
    let region = Bounds {
        min: point(-3.0, -3.0, -3.0),
        max: point(4.0, 4.0, 4.0),
    };
    let interval = field.interval(region)?;
    let mut random = XorShift(0x140_2026);
    for _ in 0..256 {
        let a = random.point(region);
        let b = random.point(region);
        let sample = field.evaluate(a)?.distance;
        assert!(interval.contains(sample), "{sample} outside {interval:?}");
        let difference = (sample - field.evaluate(b)?.distance).abs();
        let distance = primitive::length(primitive::sub(a, b));
        assert!(difference <= field.lipschitz() * distance + 1e-12);
    }
    assert!(field.capabilities().bounds);
    assert!(field.capabilities().interval);
    assert!(field.capabilities().batch);
    Ok(())
}

#[test]
fn gradients_match_finite_differences_and_sdf_direction() -> Result<(), Error> {
    let field = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0))])?;
    let gradient = field.gradient(point(1.0, 2.0, 2.0), 1e-5)?;
    close(gradient.x, 1.0 / 3.0, 1e-15);
    close(gradient.y, 2.0 / 3.0, 1e-15);
    close(gradient.z, 2.0 / 3.0, 1e-15);

    let mut second = sphere(2, point(1.0, 0.0, 0.0), 1.5);
    second.profile = BlendProfile::Round;
    second.blend_radius = 0.4;
    let smooth = compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 2.0)), entry(second)])?;
    let at = point(1.0, 1.0, 0.5);
    let step = 1e-5;
    let gradient = smooth.gradient(at, step)?;
    let central = |axis: usize| -> Result<f64, Error> {
        let mut plus = at;
        let mut minus = at;
        match axis {
            0 => {
                plus.x += step;
                minus.x -= step;
            }
            1 => {
                plus.y += step;
                minus.y -= step;
            }
            _ => {
                plus.z += step;
                minus.z -= step;
            }
        }
        Ok((smooth.evaluate(plus)?.distance - smooth.evaluate(minus)?.distance) / (2.0 * step))
    };
    // Tetrahedral and central stencils have different O(step) truncation at a blend.
    close(gradient.x, central(0)?, 5e-6);
    close(gradient.y, central(1)?, 5e-6);
    close(gradient.z, central(2)?, 5e-6);
    Ok(())
}

#[test]
fn invalid_inputs_and_budgets_fail_closed() {
    let empty: [StackEntry; 0] = [];
    assert_eq!(compile(&empty), Err(Error::EmptyStack));
    assert_eq!(
        compile(&[entry(sphere(1, point(0.0, 0.0, 0.0), 0.0))]),
        Err(Error::InvalidPrimitive(1))
    );
    let bow_tie = Primitive::Extrude {
        frame: Frame::IDENTITY,
        profile: vec![[-1.0, -1.0], [1.0, 1.0], [-1.0, 1.0], [1.0, -1.0]],
        half_height: 1.0,
    };
    assert_eq!(
        compile(&[entry(operation(1, bow_tie))]),
        Err(Error::InvalidPrimitive(1))
    );
    let repeated = Primitive::Extrude {
        frame: Frame::IDENTITY,
        profile: vec![[0.0, 0.0], [1.0, 0.0], [0.0, 1.0], [0.0, 0.0]],
        half_height: 1.0,
    };
    assert_eq!(
        compile(&[entry(operation(1, repeated))]),
        Err(Error::InvalidPrimitive(1))
    );
    assert_eq!(
        PreparedField::compile(
            &[entry(sphere(1, point(0.0, 0.0, 0.0), 1.0))],
            CompileOptions {
                solo_operation: None,
                max_operations: 0,
            },
        ),
        Err(Error::OperationBudget)
    );
}

struct XorShift(u32);

impl XorShift {
    fn next(&mut self) -> f64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        f64::from(self.0) / f64::from(u32::MAX)
    }

    fn point(&mut self, bounds: Bounds) -> Vec3 {
        let x = bounds.min.x + (bounds.max.x - bounds.min.x) * self.next();
        let y = bounds.min.y + (bounds.max.y - bounds.min.y) * self.next();
        let z = bounds.min.z + (bounds.max.z - bounds.min.z) * self.next();
        point(x, y, z)
    }
}
