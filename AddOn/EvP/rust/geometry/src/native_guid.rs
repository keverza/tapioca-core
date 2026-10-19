//! Canonical native GUID text shared by detached catalogs and identity bindings.

pub(crate) fn is_canonical(bytes: &[u8]) -> bool {
    if bytes.len() != 36 {
        return false;
    }
    let mut has_nonzero = false;
    for (index, &byte) in bytes.iter().enumerate() {
        if [8, 13, 18, 23].contains(&index) {
            if byte != b'-' {
                return false;
            }
        } else if !byte.is_ascii_digit() && !(b'A'..=b'F').contains(&byte) {
            return false;
        } else if byte != b'0' {
            has_nonzero = true;
        }
    }
    has_nonzero
}
