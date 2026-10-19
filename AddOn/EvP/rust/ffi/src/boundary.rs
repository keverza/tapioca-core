//! Call-scoped C ABI envelope; normal calls allocate nothing. See GeometryRustFfi.h.

use std::panic::{AssertUnwindSafe, catch_unwind};

const OK: i32 = 0;
const ERR_ARG: i32 = 1;
const ERR_INTERNAL: i32 = 3;
const ABI_VERSION: u32 = 1;

enum Failure {
    Argument(&'static str),
    Overlap,
    Internal,
}

impl Failure {
    fn status(&self) -> i32 {
        match self {
            Self::Argument(_) | Self::Overlap => ERR_ARG,
            Self::Internal => ERR_INTERNAL,
        }
    }

    fn message(&self) -> &'static str {
        match self {
            Self::Argument(message) => message,
            Self::Overlap => "Version and error buffers overlap",
            Self::Internal => "Rust panic contained at the Tapioca C ABI boundary",
        }
    }
}

// These checks reject malformed pointer representations, not dangling/foreign
// allocations. Actual validity, exclusive writes and lifetime are caller duties.
fn memory_range<T>(pointer: *const T, count: usize) -> Result<Option<(usize, usize)>, Failure> {
    if count == 0 {
        return Ok(None);
    }
    if pointer.is_null() || !pointer.is_aligned() {
        return Err(Failure::Argument("Null or misaligned nonempty buffer"));
    }
    let bytes = count
        .checked_mul(size_of::<T>())
        .filter(|bytes| *bytes <= isize::MAX.unsigned_abs())
        .ok_or(Failure::Argument(
            "Buffer length exceeds the addressable range",
        ))?;
    let end = pointer
        .addr()
        .checked_add(bytes)
        .ok_or(Failure::Argument("Buffer address range overflows"))?;
    Ok(Some((pointer.addr(), end)))
}

fn overlaps(left: Option<(usize, usize)>, right: Option<(usize, usize)>) -> bool {
    matches!((left, right), (Some((a, b)), Some((c, d))) if a < d && c < b)
}

// SAFETY: caller has validated the buffer range and guarantees a writable
// allocation of capacity bytes, exclusive to this call. No reference is retained.
unsafe fn write_message(buffer: *mut u8, capacity: usize, message: &str) {
    if capacity == 0 {
        return;
    }
    let mut length = message.len().min(capacity - 1);
    while !message.is_char_boundary(length) {
        length -= 1;
    }
    // SAFETY: source is a valid UTF-8 prefix, destination has capacity bytes;
    // the static/internal message does not overlap caller memory, length < capacity.
    unsafe {
        std::ptr::copy_nonoverlapping(message.as_ptr(), buffer, length);
        buffer.add(length).write(0);
    }
}

// SAFETY: a nonempty error buffer must be live/writable for capacity bytes and
// must not alias any operation input/output. Null is accepted only at capacity 0.
unsafe fn call(
    error: *mut u8,
    capacity: usize,
    operation: impl FnOnce() -> Result<(), Failure>,
) -> i32 {
    if memory_range(error.cast_const(), capacity).is_err() {
        return ERR_ARG;
    }
    let result = match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(result) => result,
        Err(payload) => {
            // A user-defined panic payload can panic again in Drop. On this
            // exceptional path leak the payload rather than unwind through C.
            std::mem::forget(payload);
            Err(Failure::Internal)
        }
    };
    let (status, message) = match result {
        Ok(()) => (OK, ""),
        // A diagnostic write would itself overwrite the failed version output.
        Err(Failure::Overlap) => return ERR_ARG,
        Err(failure) => (failure.status(), failure.message()),
    };
    // SAFETY: memory_range accepted the error range; allocation/lifetime remain
    // caller guarantees. No borrowed slice or pointer survives this call.
    unsafe { write_message(error, capacity, message) };
    status
}

/// Read the aggregate C ABI version (currently 1).
///
/// Returns status 0 on success, 1 for malformed arguments, or 3 for a caught
/// unwinding panic. On failure the version output is unchanged. The error buffer
/// is cleared on success and receives a NUL-terminated UTF-8 prefix on failure.
/// A null buffer with zero capacity discards the message, not the status.
///
/// # Safety
/// `version` must point to one aligned, writable `u32`. If `error_capacity` is
/// nonzero, `error` must point to that many writable bytes. The two regions must
/// not overlap or alias other live references during the call. They remain
/// caller-owned. Null/alignment/size checks cannot prove an allocation is valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_version(
    version: *mut u32,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: foreign allocations/lifetimes are guaranteed by the caller; call
    // checks the error range and contains unwinding panics from the operation.
    unsafe {
        call(error, error_capacity, || {
            let output = memory_range(version.cast_const(), 1)?;
            let message = memory_range(error.cast_const(), error_capacity)?;
            if overlaps(output, message) {
                return Err(Failure::Overlap);
            }
            // SAFETY: version's representation was checked and the caller
            // guarantees its live/exclusive allocation; no write precedes failure.
            version.write(ABI_VERSION);
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests;
