//! Sun-study C ABI (`SunStudyRustFfi.h`). Envelope and panic containment are shared
//! with the geometry exports; the analysis itself lives in `tapioca-sunstudy`.

use crate::boundary::{Failure, call, memory_range, overlaps};

/// Read the sun-study ABI version (currently 1), independent of the aggregate
/// `tapioca_version`, so the C++ wrapper can refuse a mismatched archive.
///
/// Returns status 0 on success, 1 for malformed arguments, or 3 for a caught
/// unwinding panic. On failure the version output is unchanged.
///
/// # Safety
/// `version` must point to one aligned, writable `u32`. If `error_capacity` is
/// nonzero, `error` must point to that many writable bytes. The regions must not
/// overlap. Null/alignment/size checks cannot prove an allocation is valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_version(
    version: *mut u32,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: foreign allocations/lifetimes are guaranteed by the caller; call
    // checks the error range and contains unwinding panics from the operation.
    unsafe {
        call(error, error_capacity, || {
            let output = memory_range(version.cast_const(), 1)?;
            if overlaps(output, memory_range(error.cast_const(), error_capacity)?) {
                return Err(Failure::Overlap);
            }
            // SAFETY: representation checked; caller guarantees the allocation.
            version.write(tapioca_sunstudy::ABI_VERSION);
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
