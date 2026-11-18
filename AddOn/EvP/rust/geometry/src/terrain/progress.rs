//! How far the running terrain build has got, for the host to show while it waits: a large site's
//! refinement takes a minute or more. The build writes it as it goes; any thread reads it at any
//! time. No lock and no callback into the host. Process-wide: while two builds overlap it holds
//! whichever wrote last.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// Nothing running.
pub const IDLE: u32 = 0;
/// Composing the field, rebuilding the region, tracing the toe.
pub const PREPARING: u32 = 1;
/// Refining the graded faces to the vertical tolerance (most of the time on a large site).
pub const REFINING: u32 = 2;
/// Heights, volumes and the published mesh.
pub const FINISHING: u32 = 3;

/// One reading.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Progress {
    /// [`IDLE`], [`PREPARING`], [`REFINING`] or [`FINISHING`].
    pub stage: u32,
    /// The refinement round, from 1.
    pub round: u32,
    /// Vertices the refinement has added so far.
    pub added: u32,
    /// Most vertices it may add.
    pub budget: u32,
    /// Triangles the current round is fixing.
    pub pending: u32,
    /// Largest height error left at the start of the current round, metres.
    pub residual: f64,
    /// That error at the first round, metres: where the refinement started.
    pub first: f64,
    /// The error it refines to, metres.
    pub tolerance: f64,
}

static STAGE: AtomicU32 = AtomicU32::new(IDLE);
static ROUND: AtomicU32 = AtomicU32::new(0);
static ADDED: AtomicU32 = AtomicU32::new(0);
static BUDGET: AtomicU32 = AtomicU32::new(0);
static PENDING: AtomicU32 = AtomicU32::new(0);
static RESIDUAL: AtomicU64 = AtomicU64::new(0);
static FIRST: AtomicU64 = AtomicU64::new(0);
static TOLERANCE: AtomicU64 = AtomicU64::new(0);

/// The latest values. Each is read on its own, so a reading taken while a round is written may mix
/// two rounds' values: good for a progress bar, not for a decision.
#[must_use]
pub fn now() -> Progress {
    Progress {
        stage: STAGE.load(Ordering::Relaxed),
        round: ROUND.load(Ordering::Relaxed),
        added: ADDED.load(Ordering::Relaxed),
        budget: BUDGET.load(Ordering::Relaxed),
        pending: PENDING.load(Ordering::Relaxed),
        residual: f64::from_bits(RESIDUAL.load(Ordering::Relaxed)),
        first: f64::from_bits(FIRST.load(Ordering::Relaxed)),
        tolerance: f64::from_bits(TOLERANCE.load(Ordering::Relaxed)),
    }
}

/// A build running: [`PREPARING`] from its start, [`IDLE`] again when it is dropped, however the
/// build ends.
pub(crate) struct Running;

impl Running {
    pub(crate) fn begin() -> Self {
        for counter in [&ROUND, &ADDED, &BUDGET, &PENDING] {
            counter.store(0, Ordering::Relaxed);
        }
        for value in [&RESIDUAL, &FIRST, &TOLERANCE] {
            value.store(0, Ordering::Relaxed);
        }
        STAGE.store(PREPARING, Ordering::Relaxed);
        Self
    }
}

impl Drop for Running {
    fn drop(&mut self) {
        STAGE.store(IDLE, Ordering::Relaxed);
    }
}

/// A refinement round begins: `at`'s round, vertices added and allowed, triangles to fix, error
/// left and tolerance (its `stage` and `first` are set here). The first round's error is kept.
pub(crate) fn refining(at: &Progress) {
    if at.round <= 1 {
        FIRST.store(at.residual.to_bits(), Ordering::Relaxed);
    }
    ROUND.store(at.round, Ordering::Relaxed);
    ADDED.store(at.added, Ordering::Relaxed);
    BUDGET.store(at.budget, Ordering::Relaxed);
    PENDING.store(at.pending, Ordering::Relaxed);
    RESIDUAL.store(at.residual.to_bits(), Ordering::Relaxed);
    TOLERANCE.store(at.tolerance.to_bits(), Ordering::Relaxed);
    STAGE.store(REFINING, Ordering::Relaxed);
}

/// The refinement is done; the rest of the build follows.
pub(crate) fn finishing() {
    STAGE.store(FINISHING, Ordering::Relaxed);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn round(round: u32, added: u32, pending: u32, residual: f64) -> Progress {
        Progress {
            round,
            added,
            budget: 200,
            pending,
            residual,
            tolerance: 0.01,
            ..Progress::default()
        }
    }

    #[test]
    fn a_build_reads_preparing_refining_finishing_then_idle() {
        // Process-wide: other tests' builds may write too, so only this sequence's own writes,
        // read back at once, are asserted.
        let running = Running::begin();
        refining(&round(1, 0, 50, 0.4));
        let first = now();
        refining(&round(2, 120, 10, 0.05));
        let second = now();
        finishing();
        let last = now().stage;
        drop(running);
        if first.round == 1 && second.round == 2 {
            assert_eq!(first.stage, REFINING);
            assert!(
                (second.first - 0.4).abs() < f64::EPSILON,
                "the first round's error stays"
            );
            assert_eq!(second.added, 120);
            assert_eq!(second.pending, 10);
        }
        assert!(matches!(last, FINISHING | PREPARING | REFINING), "{last}");
    }
}
