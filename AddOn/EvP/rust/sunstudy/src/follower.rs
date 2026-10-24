//! When a study is stale, when to recompute it, and which result may become
//! visible.
//!
//! Port of `SunStudy/SunStudyFollower.cpp`: a state machine over events and a
//! millisecond stamp the caller supplies, with no clock and no study of its
//! own. Staleness is judged against a dependency signature (geometry, sun,
//! sampling), never against renderer notifications. The transition to stale
//! is immediate and the cache keeps drawing; only the replacement waits for a
//! quiet period that restarts when the world moves again. Completion is
//! checked by generation and by signature, so a result whose world moved
//! while it ran is discarded.

/// The inputs a study's result is valid for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DependencySignature {
    /// Snapshot id of the sampled geometry.
    pub geometry: u64,
    /// Content hash of the day's sun.
    pub sun: u64,
    /// Grid spacing, sample mode, jitter and offset.
    pub sampling: u64,
}

/// Why the visible study is no longer valid.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum DirtyReason {
    /// Not dirty.
    #[default]
    None = 0,
    /// The geometry changed.
    Geometry = 1,
    /// The sun changed.
    Sun = 2,
    /// The sampling changed.
    Sampling = 3,
}

/// The follower's state.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FollowState {
    /// Nothing adopted yet.
    #[default]
    NoStudy = 0,
    /// Adopted and matching the world.
    Current = 1,
    /// Stale; the cache still draws.
    DirtyVisible = 2,
    /// The quiet period expired (never entered by this port, as in C++).
    Starting = 3,
    /// A replacement is running; the cache still draws.
    UpdatingVisible = 4,
    /// The last attempt failed; holds until the world moves.
    Failed = 5,
}

/// Quiet period before a replacement starts, milliseconds.
pub const DEBOUNCE_MS: i64 = 300;

/// The follower.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Follower {
    state: FollowState,
    dirty_reason: DirtyReason,
    study_id: String,
    study: DependencySignature,
    world: DependencySignature,
    run: DependencySignature,
    have_run: bool,
    generation: u64,
    reruns: u64,
    discarded: u64,
    dirty_since_ms: i64,
    debounce_ms: i64,
    have_world: bool,
}

impl Default for Follower {
    fn default() -> Self {
        Self {
            state: FollowState::NoStudy,
            dirty_reason: DirtyReason::None,
            study_id: String::new(),
            study: DependencySignature::default(),
            world: DependencySignature::default(),
            run: DependencySignature::default(),
            have_run: false,
            generation: 0,
            reruns: 0,
            discarded: 0,
            dirty_since_ms: 0,
            debounce_ms: DEBOUNCE_MS,
            have_world: false,
        }
    }
}

/// Which field moved; geometry first, as it invalidates the most.
fn what_changed(was: &DependencySignature, now: &DependencySignature) -> DirtyReason {
    if was.geometry != now.geometry {
        DirtyReason::Geometry
    } else if was.sun != now.sun {
        DirtyReason::Sun
    } else if was.sampling != now.sampling {
        DirtyReason::Sampling
    } else {
        DirtyReason::None
    }
}

fn reason_text(reason: DirtyReason) -> &'static str {
    match reason {
        DirtyReason::Geometry => "geometry",
        DirtyReason::Sun => "sun",
        DirtyReason::Sampling => "sampling",
        DirtyReason::None => "none",
    }
}

impl Follower {
    /// A study became the visible one; the world is what it was computed
    /// against.
    pub fn adopt(&mut self, study_id: &str, signature: DependencySignature, now_ms: i64) {
        study_id.clone_into(&mut self.study_id);
        self.study = signature;
        self.world = signature;
        self.have_world = true;
        self.state = FollowState::Current;
        self.dirty_reason = DirtyReason::None;
        self.dirty_since_ms = now_ms;
        self.have_run = false;
    }

    /// Every transition into dirty supersedes what runs and restarts the
    /// quiet period.
    fn go_dirty(&mut self, reason: DirtyReason, now_ms: i64) {
        self.generation = self.generation.wrapping_add(1);
        self.state = FollowState::DirtyVisible;
        self.have_run = false;
        self.dirty_reason = reason;
        self.dirty_since_ms = now_ms;
    }

    /// The world as it stands; the only call that decides. Returns true when
    /// the state changed.
    pub fn observe(&mut self, world: DependencySignature, now_ms: i64) -> bool {
        let first = !self.have_world;
        let previous = self.world;
        self.world = world;
        self.have_world = true;
        if self.state == FollowState::NoStudy {
            return false;
        }
        if self.state == FollowState::Failed && !first && previous == world {
            return false;
        }
        // A run in flight is judged against what it targets, not against the
        // study it replaces (the live runaway).
        let running = matches!(
            self.state,
            FollowState::Starting | FollowState::UpdatingVisible
        );
        let against = if running && self.have_run {
            self.run
        } else {
            self.study
        };
        let reason = what_changed(&against, &world);
        if reason == DirtyReason::None {
            if self.state == FollowState::DirtyVisible {
                self.state = FollowState::Current;
                self.dirty_reason = DirtyReason::None;
                return true;
            }
            return false;
        }
        // Already dirty for this reason: restart the quiet period only when
        // the world moved again (a polling caller must not postpone it).
        if self.state == FollowState::DirtyVisible && self.dirty_reason == reason {
            if previous != world {
                self.dirty_since_ms = now_ms;
            }
            return false;
        }
        self.go_dirty(reason, now_ms);
        true
    }

    /// The quiet period has expired and a replacement should start.
    #[must_use]
    pub fn should_start(&self, now_ms: i64) -> bool {
        self.state == FollowState::DirtyVisible
            && now_ms.wrapping_sub(self.dirty_since_ms) >= self.debounce_ms
    }

    /// What is left of the quiet period, or -1 when nothing waits.
    #[must_use]
    pub fn milliseconds_until_start(&self, now_ms: i64) -> i64 {
        if self.state != FollowState::DirtyVisible {
            return -1;
        }
        let elapsed = now_ms.wrapping_sub(self.dirty_since_ms);
        if elapsed >= self.debounce_ms {
            0
        } else {
            self.debounce_ms.wrapping_sub(elapsed)
        }
    }

    /// A replacement started for `signature`; returns its generation.
    pub fn note_started(&mut self, signature: DependencySignature) -> u64 {
        self.state = FollowState::UpdatingVisible;
        self.world = signature;
        self.have_world = true;
        self.run = signature;
        self.have_run = true;
        self.reruns = self.reruns.wrapping_add(1);
        self.generation
    }

    /// A preview may be published without accepting the final grid.
    #[must_use]
    pub fn can_publish_result(&self, generation: u64, signature: DependencySignature) -> bool {
        self.state == FollowState::UpdatingVisible
            && self.have_run
            && generation == self.generation
            && signature == self.run
            && self.have_world
            && signature == self.world
    }

    /// A run finished; false when its result is stale and must be dropped.
    pub fn note_completed(
        &mut self,
        generation: u64,
        study_id: &str,
        signature: DependencySignature,
        now_ms: i64,
    ) -> bool {
        if generation != self.generation {
            self.discarded = self.discarded.wrapping_add(1);
            return false;
        }
        if self.have_world && signature != self.world {
            self.discarded = self.discarded.wrapping_add(1);
            let reason = what_changed(&signature, &self.world);
            self.go_dirty(reason, now_ms);
            return false;
        }
        self.adopt(study_id, signature, now_ms);
        true
    }

    /// A run failed; a failure from a superseded run is only counted.
    pub fn note_failed(&mut self, generation: u64) {
        if generation != self.generation {
            self.discarded = self.discarded.wrapping_add(1);
            return;
        }
        self.state = FollowState::Failed;
        self.have_run = false;
    }

    /// Forget everything; the generation keeps counting so an orphaned run
    /// never matches again.
    pub fn clear(&mut self) {
        self.state = FollowState::NoStudy;
        self.dirty_reason = DirtyReason::None;
        self.study_id.clear();
        self.study = DependencySignature::default();
        self.world = DependencySignature::default();
        self.run = DependencySignature::default();
        self.have_world = false;
        self.have_run = false;
        self.generation = self.generation.wrapping_add(1);
        self.dirty_since_ms = 0;
    }

    /// Set the quiet period; non-positive restores the default.
    pub fn set_debounce_ms(&mut self, ms: i64) {
        self.debounce_ms = if ms > 0 { ms } else { DEBOUNCE_MS };
    }

    /// The state.
    #[must_use]
    pub fn state(&self) -> FollowState {
        self.state
    }

    /// Why it is dirty.
    #[must_use]
    pub fn dirty_reason(&self) -> DirtyReason {
        self.dirty_reason
    }

    /// The adopted study's id.
    #[must_use]
    pub fn study_id(&self) -> &str {
        &self.study_id
    }

    /// The adopted study's signature.
    #[must_use]
    pub fn study_signature(&self) -> DependencySignature {
        self.study
    }

    /// The last observed world.
    #[must_use]
    pub fn world_signature(&self) -> DependencySignature {
        self.world
    }

    /// The generation a result must carry.
    #[must_use]
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// Replacements started.
    #[must_use]
    pub fn reruns(&self) -> u64 {
        self.reruns
    }

    /// Results dropped because the world moved while they ran.
    #[must_use]
    pub fn discarded(&self) -> u64 {
        self.discarded
    }

    /// The quiet period.
    #[must_use]
    pub fn debounce_ms(&self) -> i64 {
        self.debounce_ms
    }

    /// A sentence naming both snapshots.
    #[must_use]
    pub fn describe(&self) -> String {
        match self.state {
            FollowState::NoStudy => "no sun study is being followed".to_owned(),
            FollowState::Current => format!(
                "sun study '{}' is current for snapshot {}",
                self.study_id, self.study.geometry
            ),
            FollowState::DirtyVisible => format!(
                "sun study partly stale ({}): scene snapshot {} != study snapshot {}",
                reason_text(self.dirty_reason),
                self.world.geometry,
                self.study.geometry
            ),
            FollowState::Starting => "a replacement sun study is starting".to_owned(),
            FollowState::UpdatingVisible => {
                "a replacement sun study is resolving its timesteps; the cache stays on screen"
                    .to_owned()
            }
            FollowState::Failed => "the last sun study attempt failed".to_owned(),
        }
    }
}

#[cfg(test)]
mod tests;
