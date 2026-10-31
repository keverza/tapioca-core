//! Fixed-size inspector slope conversion using the existing geometry grade type.

use super::edit::{TerrainSlope, slope};
use crate::borrow::BorrowContext;
use crate::boundary::{Failure, call, memory_range};
use crate::pools;
use tapioca_geometry::slope::{Slope, Unit};

/// One authored grade and requested display form; no retained state or allocation.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SlopeRequest {
    /// Existing terrain slope POD, including its zero reserved word.
    pub value: TerrainSlope,
    /// Positive reference run for new ratio/height forms (metres for height).
    pub run: f64,
    /// Same 1..4 unit codes as `TerrainSlope`.
    pub target_kind: u32,
    /// Zero.
    pub reserved: u32,
}

fn convert(request: SlopeRequest) -> Result<TerrainSlope, Failure> {
    if request.reserved != 0 || request.value.reserved != 0 {
        return Err(Failure::Argument("SlopeReserved: expected zero"));
    }
    if !request.value.b.is_finite() || (request.value.kind <= 2 && request.value.b.abs() > 0.0) {
        return Err(Failure::Argument(
            "SlopeParts: unused second value must be zero",
        ));
    }
    let unit = match request.target_kind {
        1 => Unit::Angle,
        2 => Unit::Percent,
        3 => Unit::RiseRun(request.run),
        4 => Unit::Height(request.run),
        _ => return Err(Failure::Argument("SlopeUnit: expected 1..4")),
    };
    let converted = slope(request.value)?
        .converted(unit)
        .map_err(|error| Failure::Argument(error.message()))?;
    let (a, b) = match converted {
        Slope::Angle { radians } => (radians, 0.0),
        Slope::Percent(percent) => (percent, 0.0),
        Slope::RiseRun { rise, run } => (rise, run),
        Slope::Height {
            rise_metres,
            run_metres,
        } => (rise_metres, run_metres),
    };
    Ok(TerrainSlope {
        a,
        b,
        kind: request.target_kind,
        reserved: 0,
    })
}

/// Convert one inspector grade. All output remains unchanged on refusal.
///
/// # Safety
/// Request is live initialized immutable POD. Output is live aligned exclusively
/// writable storage, disjoint from request and error. No borrow survives the call.
/// Shape checks cannot prove foreign allocation validity; OOM/abort/UB are not caught.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_slope_convert(
    request: *const SlopeRequest,
    output: *mut TerrainSlope,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: shared envelope contains unwind; preflight runs before any read/write.
    unsafe {
        call(error, capacity, || {
            let error_range = memory_range(error.cast_const(), capacity)?;
            pools::preflight(
                &[memory_range(request, 1)],
                &[memory_range(output.cast_const(), 1)],
                error_range,
            )?;
            let context = BorrowContext {
                error_range,
                output_range: memory_range(output.cast_const(), 1)?,
            };
            let result = convert(context.record(request)?)?;
            // SAFETY: exclusive aligned output was validated, all input copied.
            output.write(result);
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> SlopeRequest {
        SlopeRequest {
            value: TerrainSlope {
                a: 50.0,
                b: 0.0,
                kind: 2,
                reserved: 0,
            },
            run: 3.0,
            target_kind: 3,
            reserved: 0,
        }
    }

    #[test]
    fn conversion_is_atomic_and_preserves_authored_values() {
        assert_eq!(size_of::<SlopeRequest>(), 40);
        assert_eq!(std::mem::offset_of!(SlopeRequest, target_kind), 32);
        let mut input = request();
        let sentinel = TerrainSlope {
            a: 77.0,
            b: 88.0,
            kind: 4,
            reserved: 0,
        };
        let mut output = sentinel;
        // SAFETY: disjoint live stack POD; empty diagnostic is permitted.
        let status = unsafe {
            tapioca_slope_convert(&raw const input, &raw mut output, std::ptr::null_mut(), 0)
        };
        assert_eq!(status, 0);
        assert_eq!(
            output,
            TerrainSlope {
                a: 1.5,
                b: 3.0,
                kind: 3,
                reserved: 0
            }
        );
        for invalid in [0, 5, u32::MAX] {
            input.target_kind = invalid;
            output = sentinel;
            // SAFETY: same valid storage; only the unit code is malformed.
            let status = unsafe {
                tapioca_slope_convert(&raw const input, &raw mut output, std::ptr::null_mut(), 0)
            };
            assert_eq!(status, 1);
            assert_eq!(output, sentinel);
        }
        input = request();
        input.value.reserved = 1;
        assert!(convert(input).is_err());
        input = request();
        input.value.b = f64::NAN;
        assert!(convert(input).is_err());
    }

    #[test]
    fn aliased_output_and_error_do_not_overwrite_input() {
        let input = request();
        let before = input.value;
        // SAFETY: pointers are live; aliasing is intentionally refused before write.
        let status = unsafe {
            tapioca_slope_convert(
                &raw const input,
                std::ptr::addr_of!(input.value).cast_mut(),
                std::ptr::null_mut(),
                0,
            )
        };
        assert_eq!(status, 1);
        assert_eq!(input.value, before);
        let mut output = before;
        // SAFETY: valid request/output; diagnostic aliases output and must remain untouched.
        let status = unsafe {
            tapioca_slope_convert(
                &raw const input,
                &raw mut output,
                std::ptr::addr_of_mut!(output).cast(),
                size_of::<TerrainSlope>(),
            )
        };
        assert_eq!(status, 1);
        assert_eq!(output, before);
    }

    #[test]
    fn malformed_storage_and_authored_parts_leave_output_unchanged() {
        let sentinel = request().value;
        let mut output = sentinel;
        // SAFETY: the null request is refused before dereference; output is live.
        let status = unsafe {
            tapioca_slope_convert(std::ptr::null(), &raw mut output, std::ptr::null_mut(), 0)
        };
        assert_eq!(status, 1);
        assert_eq!(output, sentinel);
        for bad in [
            TerrainSlope {
                a: f64::NAN,
                ..sentinel
            },
            TerrainSlope {
                a: f64::INFINITY,
                ..sentinel
            },
            TerrainSlope { b: 1.0, ..sentinel },
            TerrainSlope {
                kind: 0,
                ..sentinel
            },
            TerrainSlope {
                reserved: 1,
                ..sentinel
            },
            TerrainSlope {
                a: 0.0,
                b: 0.0,
                kind: 3,
                reserved: 0,
            },
        ] {
            let input = SlopeRequest {
                value: bad,
                ..request()
            };
            // SAFETY: valid, disjoint stack storage; authored value alone is invalid.
            let status = unsafe {
                tapioca_slope_convert(&raw const input, &raw mut output, std::ptr::null_mut(), 0)
            };
            assert_eq!(status, 1);
            assert_eq!(output, sentinel);
        }
    }
}
