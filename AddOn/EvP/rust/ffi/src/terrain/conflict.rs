//! Located conflict ABI v1: where a band cannot be built as authored, and where
//! an edit left a vertex in place. Additive to the hybrid build and edit
//! exports (whose records and counters are unchanged) and to their refusal
//! contract: a request the build refuses still assesses successfully here.
//!
//! Both exports take the existing request record and their own output, with the
//! query/fill contract of `tapioca_terrain_triangulate`.

use super::edit::{TerrainEditRequest, with_edit};
use super::hybrid::{Shape, TerrainHybridRequest, copy, with_band};
use super::{Failure, call, memory_range};
use crate::pools::{Checked, Range};
use tapioca_geometry::terrain::edit::ClipKind;
use tapioca_geometry::terrain::edit::Hand;
use tapioca_geometry::terrain::hybrid::{self as kernel, ConflictCode};

/// One located band conflict. `code`: 1 `DaylightNotReached`, 2
/// `DaylightBeyondLimit`, 3 `VerticalFace`, 4 `DaylightTopology`, 5
/// `EdgeContradictsEffect`. `side`: 1 left, 2 right. `hard` 1 when the build
/// refuses on it.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainConflict {
    /// Plan position and height of the place, world metres.
    pub position: [f64; 3],
    /// Per-code magnitude (see the C header).
    pub magnitude: f64,
    /// Conflict code.
    pub code: u32,
    /// Station index.
    pub station: u32,
    /// Side.
    pub side: u32,
    /// 0 or 1.
    pub hard: u32,
}

/// One located edit clip. `kind`: 1 beyond a daylight extension, 2 beyond the
/// maximum distance.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TerrainClip {
    /// Plan distance from the edit's core, metres.
    pub distance: f64,
    /// Height change withheld, metres (signed, after strength).
    pub withheld: f64,
    /// Edit index.
    pub edit: u32,
    /// Input vertex.
    pub vertex: u32,
    /// Clip kind.
    pub kind: u32,
    /// Zero.
    pub reserved: u32,
}

/// Caller-owned rows. `count` is required; query mode publishes only it and
/// needs zero capacity.
#[repr(C)]
pub struct TerrainRowsOutput<T> {
    /// Required row count.
    pub count: *mut usize,
    /// Rows.
    pub rows: *mut T,
    /// Row capacity.
    pub capacity: usize,
    /// 1 publishes only the count.
    pub query_only: u32,
    /// Zero.
    pub reserved: u32,
}

// Raw pointers and scalars only: copyable whatever the row type.
impl<T> Clone for TerrainRowsOutput<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for TerrainRowsOutput<T> {}

/// Located band conflicts output.
pub type TerrainConflictOutput = TerrainRowsOutput<TerrainConflict>;
/// Located edit clips output.
pub type TerrainClipOutput = TerrainRowsOutput<TerrainClip>;

pub(super) fn writable<T>(target: TerrainRowsOutput<T>) -> Vec<Checked> {
    vec![
        memory_range(target.count.cast_const(), 1),
        memory_range(target.rows.cast_const(), target.capacity),
    ]
}

pub(super) fn mode<T>(target: &TerrainRowsOutput<T>) -> Result<(), Failure> {
    if target.query_only > 1
        || target.reserved != 0
        || (target.query_only == 1 && target.capacity != 0)
    {
        return Err(Failure::Argument("TerrainOutputMode"));
    }
    Ok(())
}

// SAFETY: the target ranges were preflighted disjoint from every input, the
// error buffer and each other.
pub(super) unsafe fn publish<T: Copy>(
    rows: &[T],
    target: TerrainRowsOutput<T>,
) -> Result<(), Failure> {
    if target.query_only == 0 && target.capacity < rows.len() {
        return Err(Failure::Argument("TerrainOutputTooSmall"));
    }
    // SAFETY: capacity verified before ANY write; POD copies cannot unwind.
    unsafe {
        if target.query_only == 0 {
            copy(rows, target.rows);
        }
        target.count.write(rows.len());
    }
    Ok(())
}

pub(super) const fn code(value: ConflictCode) -> u32 {
    match value {
        ConflictCode::DaylightNotReached => 1,
        ConflictCode::DaylightBeyondLimit => 2,
        ConflictCode::VerticalFace => 3,
        ConflictCode::DaylightTopology => 4,
        ConflictCode::EdgeContradictsEffect => 5,
    }
}

/// Every located conflict of a band, hard and soft, without building.
///
/// # Safety
/// Same live/aligned/disjoint/unchanged-on-refusal contract as
/// `tapioca_terrain_hybrid_build`, with this output record. Attribution
/// refusals return STATE; a design refusal that has no location ARG.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_hybrid_conflicts(
    input: *const TerrainHybridRequest,
    output: *const TerrainConflictOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: call validates error storage and contains panics; with_band
    // preflights every pool before any borrow and nothing is written before
    // the capacity check.
    unsafe {
        call(error, capacity, || {
            let range: Range = memory_range(error.cast_const(), capacity)?;
            let shape = Shape { writable, mode };
            let (rows, target) = with_band(input, output, range, shape, |request| {
                let found = kernel::assess(request).map_err(|e| Failure::Argument(e.message()))?;
                Ok(found
                    .iter()
                    .map(|c| TerrainConflict {
                        position: c.position,
                        magnitude: c.magnitude,
                        code: code(c.code),
                        station: c.station,
                        side: match c.side {
                            Hand::Left => 1,
                            Hand::Right => 2,
                        },
                        hard: u32::from(c.code.hard()),
                    })
                    .collect::<Vec<_>>())
            })?;
            publish(&rows, target)
        })
    }
}

/// The located soft-tier clips of a terrain edit (the rows behind its
/// `extension_clipped`/`limit_clipped` counters), by vertex then edit.
///
/// # Safety
/// Same contract as `tapioca_terrain_edit`, with this output record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_terrain_edit_clips(
    input: *const TerrainEditRequest,
    output: *const TerrainClipOutput,
    error: *mut u8,
    capacity: usize,
) -> i32 {
    // SAFETY: as above; with_edit also preflights each surface's nested pools.
    unsafe {
        call(error, capacity, || {
            let range: Range = memory_range(error.cast_const(), capacity)?;
            let (edited, target) =
                with_edit(input, output, range, writable, |target, _, _| mode(target))?;
            let rows: Vec<TerrainClip> = edited
                .clips
                .iter()
                .map(|c| TerrainClip {
                    distance: c.distance,
                    withheld: c.withheld,
                    edit: c.edit,
                    vertex: c.vertex,
                    kind: match c.kind {
                        ClipKind::Extension => 1,
                        ClipKind::Limit => 2,
                    },
                    reserved: 0,
                })
                .collect();
            publish(&rows, target)
        })
    }
}
