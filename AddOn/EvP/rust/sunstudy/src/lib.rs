//! Pure sun-study analysis core.
//!
//! The SDK-free half of the native sun study, ported from
//! `Sources/AddOn/SunStudy/` behind the C ABI in `tapioca-core-ffi`. Every
//! function takes numbers and returns numbers: no host, rendering, platform,
//! filesystem or FFI dependency, and no solar-position formula (the host supplies
//! sun directions). The C++ implementation stays the production authority and
//! the parity oracle until the integration flip.

#![forbid(unsafe_code)]

pub mod limits;
pub mod raster;
pub mod series;
pub mod winding;

/// Version of the sun-study C ABI surface (`SunStudyRustFfi.h`).
///
/// Bumped only when an exported sun-study record or call changes shape, so the
/// C++ wrapper can refuse a mismatched archive instead of misreading it.
pub const ABI_VERSION: u32 = 1;
