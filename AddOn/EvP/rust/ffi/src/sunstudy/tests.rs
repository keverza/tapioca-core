use super::*;

#[test]
fn version_matches_the_core_contract() {
    let mut version = 0;
    let mut error = [b'x'; 64];
    // SAFETY: exclusive local outputs of the declared sizes.
    let status =
        unsafe { tapioca_sunstudy_version(&raw mut version, error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 0);
    assert_eq!(version, tapioca_sunstudy::ABI_VERSION);
    assert_eq!(error.first(), Some(&0));
}

#[test]
fn null_version_is_refused_without_writing() {
    let mut error = [0; 64];
    // SAFETY: the null output is rejected before any write.
    let status =
        unsafe { tapioca_sunstudy_version(std::ptr::null_mut(), error.as_mut_ptr(), error.len()) };
    assert_eq!(status, 1);
    assert!(error.starts_with(b"Null or misaligned"));
}
