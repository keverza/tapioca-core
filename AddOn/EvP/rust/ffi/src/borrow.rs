//! Shared call-scoped foreign borrows. Memory ownership/validity remains a caller obligation.

use crate::boundary::{Failure, memory_range, overlaps};

/// Call-scoped array view. Counts are scalar elements, never bytes or xyz tuples.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SliceView<T> {
    /// Immutable caller-owned data; null is accepted only when count is zero.
    pub data: *const T,
    /// Number of scalar elements available at data.
    pub count: usize,
}

pub(crate) struct BorrowContext {
    pub(crate) error_range: Option<(usize, usize)>,
    pub(crate) output_range: Option<(usize, usize)>,
}

impl BorrowContext {
    // SAFETY: nonempty views must point to live, initialized, immutable storage
    // lasting at least as long as this call-local context. Shape/alias checks do
    // not prove allocation validity. Returned borrows cannot outlive the context.
    pub(crate) unsafe fn slice<T: Copy>(&self, view: SliceView<T>) -> Result<&[T], Failure> {
        let range = memory_range(view.data, view.count)?;
        if overlaps(range, self.error_range) || overlaps(range, self.output_range) {
            return Err(Failure::Overlap);
        }
        if view.count == 0 {
            return Ok(&[]);
        }
        // SAFETY: checked null/alignment/overflow and writable-output overlap;
        // the caller guarantees initialized immutable allocation and lifetime.
        Ok(unsafe { std::slice::from_raw_parts(view.data, view.count) })
    }

    // SAFETY: pointer is live initialized immutable storage for one T, or a
    // malformed shape that is rejected before dereference. No borrow is retained.
    pub(crate) unsafe fn record<T: Copy>(&self, data: *const T) -> Result<T, Failure> {
        // SAFETY: forwards the foreign record contract; slice checks shape/overlap.
        unsafe { self.slice(SliceView { data, count: 1 })? }
            .first()
            .copied()
            .ok_or(Failure::Argument("MissingDescriptor"))
    }

    // SAFETY: same immutable foreign allocation contract as slice; text is UTF-8 checked.
    pub(crate) unsafe fn text(&self, view: SliceView<u8>) -> Result<&str, Failure> {
        // SAFETY: caller supplies valid immutable storage for the call.
        let bytes = unsafe { self.slice(view)? };
        std::str::from_utf8(bytes)
            .map_err(|_| Failure::Argument("InvalidUtf8: malformed text view"))
    }
}
