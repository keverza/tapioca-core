use super::*;

#[test]
fn version_and_success_message() {
    let mut version = 99;
    let mut error = [b'x'; 128];
    // SAFETY: exclusive local outputs of the exact declared sizes.
    let status = unsafe { tapioca_version(&raw mut version, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, OK);
    assert_eq!(version, ABI_VERSION);
    assert_eq!(error.first(), Some(&0));
}

#[test]
fn no_error_buffer_is_valid() {
    let mut version = 0;
    // SAFETY: version is a local u32; zero-capacity error has no memory access.
    let status = unsafe { tapioca_version(&raw mut version, std::ptr::null_mut(), 0) };
    assert_eq!(status, OK);
    assert_eq!(version, ABI_VERSION);
}

#[test]
fn invalid_error_buffer_leaves_version_untouched() {
    let mut version = 99;
    // SAFETY: deliberate null/length mismatch is rejected before dereferencing;
    // the only nonnull pointer refers to a live local u32.
    let status = unsafe { tapioca_version(&raw mut version, std::ptr::null_mut(), 1) };
    assert_eq!(status, ERR_ARG);
    assert_eq!(version, 99);
}

#[test]
fn null_version_reports_argument_error() {
    let mut error = [0; 128];
    // SAFETY: the null version is rejected; error is an exclusive local buffer.
    let status = unsafe { tapioca_version(std::ptr::null_mut(), error.as_mut_ptr(), error.len()) };
    assert_eq!(status, ERR_ARG);
    assert!(error.starts_with(b"Null or misaligned"));
    assert!(error.contains(&0));
}

#[test]
fn misaligned_version_is_never_dereferenced() {
    let mut storage = [0_u32; 2];
    let mut error = [0; 128];
    let pointer = storage.as_mut_ptr().cast::<u8>().wrapping_add(1).cast();
    // SAFETY: deliberately misaligned pointer is rejected without dereferencing;
    // its backing storage remains live, and error is an independent allocation.
    let status = unsafe { tapioca_version(pointer, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, ERR_ARG);
    assert_eq!(storage, [0, 0]);
}

#[test]
fn overlapping_buffers_refuse_before_version_write() {
    let mut storage = [u32::MAX; 2];
    let pointer = storage.as_mut_ptr();
    // SAFETY: both pointers describe live writable storage. This deliberate
    // overlap is rejected before either buffer is written.
    let status = unsafe { tapioca_version(pointer, pointer.cast(), 1) };
    assert_eq!(status, ERR_ARG);
    assert_eq!(storage, [u32::MAX; 2]);
}

#[test]
fn overflowing_ranges_refuse_without_memory_access() {
    let mut version = 99;
    let mut error = [0_u8; 1];
    // SAFETY: impossible capacity is rejected before access; pointer itself is live.
    let status = unsafe { tapioca_version(&raw mut version, error.as_mut_ptr(), usize::MAX) };
    assert_eq!(status, ERR_ARG);
    assert_eq!(version, 99);
    assert!(memory_range(std::ptr::NonNull::<u32>::dangling().as_ptr(), usize::MAX).is_err());
}

#[test]
#[expect(
    clippy::panic,
    reason = "Deliberate panic tests the same boundary helper used by exports"
)]
fn unwinding_panic_returns_internal_status() {
    let mut error = [0; 128];
    // SAFETY: exclusive local error buffer; the closure has no foreign references.
    let status = unsafe {
        call(error.as_mut_ptr(), error.len(), || {
            panic!("FFI safety fixture")
        })
    };
    assert_eq!(status, ERR_INTERNAL);
    assert!(error.starts_with(b"Rust panic contained"));
}

#[test]
fn error_message_truncation_keeps_valid_utf8_and_terminator() {
    for capacity in 1..=8 {
        let mut error = [0xff; 8];
        // SAFETY: capacity never exceeds the exclusive local buffer.
        let status = unsafe {
            call(error.as_mut_ptr(), capacity, || {
                Err(Failure::Argument("é🌿x"))
            })
        };
        assert_eq!(status, ERR_ARG);
        let prefix: Vec<u8> = error
            .iter()
            .copied()
            .take_while(|byte| *byte != 0)
            .collect();
        assert!(prefix.len() < capacity);
        assert!(std::str::from_utf8(&prefix).is_ok());
        assert!(error.iter().skip(capacity).all(|byte| *byte == 0xff));
    }
}
