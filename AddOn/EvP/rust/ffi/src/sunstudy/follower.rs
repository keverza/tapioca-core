//! The follower policy as a Rust-owned handle.
//!
//! Handle contract: one thread at a time (the C++ driver ticks on one thread;
//! the C++ follower has the same contract). Every event and query goes through
//! one export, so the C++ wrapper keeps the C++ class's method surface while
//! the decisions are made here.

use super::Regions;
use crate::borrow::SliceView;
use crate::boundary::{Failure, call, memory_range};
use tapioca_sunstudy::follower::{DependencySignature, Follower};

/// Opaque follower handle.
pub struct FollowerHandle {
    follower: Follower,
}

/// SAFETY: `handle` must be null or a live handle used by no other thread.
unsafe fn follower<'a>(handle: *mut FollowerHandle) -> Result<&'a mut Follower, Failure> {
    memory_range(handle.cast_const(), 1)?;
    // SAFETY: non-null and aligned (checked); exclusivity is the caller's
    // contract.
    unsafe { handle.as_mut() }
        .map(|h| &mut h.follower)
        .ok_or(Failure::Argument("FollowerHandle: null"))
}

/// Create a follower in the no-study state.
///
/// # Safety
/// `out` must be one writable pointer slot, disjoint from the error buffer.
/// Release with `tapioca_sunstudy_follower_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_follower_create(
    out: *mut *mut FollowerHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; the output slot is checked first.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.output(out, 1);
            regions.check()?;
            out.write(Box::into_raw(Box::new(FollowerHandle {
                follower: Follower::default(),
            })));
            Ok(())
        })
    }
}

/// Release a follower. Null is accepted.
///
/// # Safety
/// `handle` must be null or a live handle no thread is using, never used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_follower_destroy(
    handle: *mut FollowerHandle,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; ownership returns to Rust once.
    unsafe {
        call(error, error_capacity, || {
            if !handle.is_null() {
                memory_range(handle.cast_const(), 1)?;
                // SAFETY: the caller transfers the live Box allocation back.
                drop(Box::from_raw(handle));
            }
            Ok(())
        })
    }
}

/// One event or query.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FollowerEvent {
    /// 0 adopt, 1 observe, 2 should-start, 3 milliseconds-until-start,
    /// 4 note-started, 5 can-publish, 6 note-completed, 7 note-failed,
    /// 8 clear, 9 set-debounce, 10 read only.
    pub kind: u8,
    /// Geometry, sun, sampling.
    pub signature: [u64; 3],
    /// Monotonic milliseconds.
    pub now_ms: i64,
    /// The run's generation (can-publish, note-completed, note-failed).
    pub generation: u64,
    /// Debounce milliseconds (set-debounce).
    pub debounce_ms: i64,
    /// Study id, UTF-8 (adopt, note-completed).
    pub study_id: SliceView<u8>,
}

/// The follower after an event.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FollowerSnapshot {
    /// The event's answer: a flag (0/1), a generation, or milliseconds.
    pub answer: i64,
    /// Adopted study's signature.
    pub study: [u64; 3],
    /// Last observed world.
    pub world: [u64; 3],
    /// Generation, reruns, discarded results.
    pub counters: [u64; 3],
    /// Quiet period, milliseconds.
    pub debounce_ms: i64,
    /// `FollowState`.
    pub state: u8,
    /// `DirtyReason`.
    pub dirty_reason: u8,
}

fn signature(fields: [u64; 3]) -> DependencySignature {
    let [geometry, sun, sampling] = fields;
    DependencySignature {
        geometry,
        sun,
        sampling,
    }
}

fn fields(s: DependencySignature) -> [u64; 3] {
    [s.geometry, s.sun, s.sampling]
}

#[expect(
    clippy::cast_possible_wrap,
    reason = "a generation crosses as its two's-complement bits"
)]
fn bits(value: u64) -> i64 {
    value as i64
}

fn apply(f: &mut Follower, e: &FollowerEvent, id: &str) -> Result<i64, Failure> {
    let s = signature(e.signature);
    Ok(match e.kind {
        0 => {
            f.adopt(id, s, e.now_ms);
            0
        }
        1 => i64::from(f.observe(s, e.now_ms)),
        2 => i64::from(f.should_start(e.now_ms)),
        3 => f.milliseconds_until_start(e.now_ms),
        4 => bits(f.note_started(s)),
        5 => i64::from(f.can_publish_result(e.generation, s)),
        6 => i64::from(f.note_completed(e.generation, id, s, e.now_ms)),
        7 => {
            f.note_failed(e.generation);
            0
        }
        8 => {
            f.clear();
            0
        }
        9 => {
            f.set_debounce_ms(e.debounce_ms);
            0
        }
        10 => 0,
        _ => return Err(Failure::Argument("FollowerEvent: kind")),
    })
}

/// Apply one event and read the follower back.
///
/// # Safety
/// The handle must be live and used by no other thread; the event and its id
/// text live for the call; `out` writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_follower_apply(
    handle: *mut FollowerHandle,
    event: *const FollowerEvent,
    out: *mut FollowerSnapshot,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(event, 1);
            regions.output(out, 1);
            regions.check()?;
            let event = regions.context().record(event)?;
            regions.input(event.study_id.data, event.study_id.count);
            regions.check()?;
            let context = regions.context();
            let id = context.text(event.study_id)?;
            let f = follower(handle)?;
            let answer = apply(f, &event, id)?;
            out.write(FollowerSnapshot {
                answer,
                study: fields(f.study_signature()),
                world: fields(f.world_signature()),
                counters: [f.generation(), f.reruns(), f.discarded()],
                debounce_ms: f.debounce_ms(),
                state: f.state() as u8,
                dirty_reason: f.dirty_reason() as u8,
            });
            Ok(())
        })
    }
}

/// Destination for a follower text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FollowerTextOutput {
    /// 0 the adopted study id, 1 the description.
    pub which: u8,
    /// Bytes out (UTF-8, not terminated); may be null with capacity 0.
    pub text: *mut u8,
    /// Capacity in bytes.
    pub capacity: usize,
    /// Bytes the text needs (written even when it did not fit).
    pub length: *mut usize,
}

/// Read the study id or the description (shape-then-fill).
///
/// # Safety
/// The handle must be live and used by no other thread; the descriptor live;
/// `text` writable for `capacity` and `length` writable, all disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_follower_text(
    handle: *mut FollowerHandle,
    output: *const FollowerTextOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(output, 1);
            regions.check()?;
            let t = regions.context().record(output)?;
            regions.output(t.text, t.capacity);
            regions.output(t.length, 1);
            regions.check()?;
            let f = follower(handle)?;
            let text = match t.which {
                0 => f.study_id().to_owned(),
                1 => f.describe(),
                _ => return Err(Failure::Argument("FollowerTextOutput: which")),
            };
            t.length.write(text.len());
            if t.capacity >= text.len() && !text.is_empty() {
                std::ptr::copy_nonoverlapping(text.as_ptr(), t.text, text.len());
            }
            Ok(())
        })
    }
}
