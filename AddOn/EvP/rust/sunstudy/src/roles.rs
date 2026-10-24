//! Which elements a study measures and which only cast shadow.
//!
//! Port of `SunStudy/SunStudyRoles.cpp` (minus `OccluderSnapshot`, which builds
//! a host snapshot and stays C++) and the decision half of
//! `SunStudySelectionBinding.hpp`.
//!
//! | analysis | context | analysed            | context         | ignored  |
//! |----------|---------|---------------------|-----------------|----------|
//! | empty    | empty   | everything          | --              | --       |
//! | empty    | named   | all but the context | the named ones  | --       |
//! | named    | empty   | the named ones      | everything else | --       |
//! | named    | named   | the named ones      | the named ones  | the rest |
//!
//! An element named in both lists is analysis. An explicit ignored list wins
//! over both and is applied before the table.

use std::collections::BTreeSet;

/// An element's role. Discriminants are the C ABI values.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ElementRole {
    /// Receives samples and casts shadow.
    Analysis = 0,
    /// Casts shadow, is not measured.
    Context = 1,
    /// Absent from the study.
    Ignored = 2,
}

/// Resolved roles plus what the picks could not match.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ElementRoles {
    /// One per element, in the caller's element order.
    pub roles: Vec<ElementRole>,
    /// Elements with [`ElementRole::Analysis`].
    pub analysis: usize,
    /// Elements with [`ElementRole::Context`].
    pub context: usize,
    /// Elements with [`ElementRole::Ignored`].
    pub ignored: usize,
    /// Distinct canonical analysis picks matching no element: reported, not
    /// silently dropped.
    pub unmatched_analysis: usize,
    /// Distinct canonical context picks matching no element.
    pub unmatched_context: usize,
    /// Distinct canonical ignored picks matching no element.
    pub unmatched_ignored: usize,
    /// An analysis list was named and nothing of it is left to measure: the
    /// study is refused rather than widened or reported as zero hours.
    pub analysis_named_but_absent: bool,
}

impl ElementRoles {
    /// Whether the occluders are a strict subset of the snapshot.
    #[must_use]
    pub fn restricts_occluders(&self) -> bool {
        self.ignored > 0
    }

    /// Per element: 1 when its faces are sampled.
    #[must_use]
    pub fn sample_mask(&self) -> Vec<u8> {
        self.roles
            .iter()
            .map(|role| u8::from(*role == ElementRole::Analysis))
            .collect()
    }
}

/// C `isspace` in the "C" locale: space, `\t`, `\n`, `\v`, `\f`, `\r`.
/// (`u8::is_ascii_whitespace` omits `\v`.)
fn c_space(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r')
}

/// One canonical GUID form: braces and C-locale whitespace removed, ASCII
/// upper case. Non-ASCII bytes pass through unchanged, as `toupper` leaves them
/// in the "C" locale.
#[must_use]
pub fn canonical_guid(guid: &str) -> String {
    guid.chars()
        .filter(|c| !matches!(*c, '{' | '}') && !u8::try_from(*c).is_ok_and(c_space))
        .map(|c| c.to_ascii_uppercase())
        .collect()
}

fn canonical_set<S: AsRef<str>>(guids: &[S]) -> BTreeSet<String> {
    guids
        .iter()
        .map(|g| canonical_guid(g.as_ref()))
        .filter(|g| !g.is_empty())
        .collect()
}

fn unmatched(picked: &BTreeSet<String>, present: &BTreeSet<String>) -> usize {
    picked.iter().filter(|g| !present.contains(*g)).count()
}

/// Resolve every element's role from the three pick lists.
#[must_use]
pub fn resolve_element_roles<S: AsRef<str>>(
    elements: &[S],
    analysis_picked: &[S],
    context_picked: &[S],
    ignored_picked: &[S],
) -> ElementRoles {
    let ignored = canonical_set(ignored_picked);
    let analysis_all = canonical_set(analysis_picked);
    let context_all = canonical_set(context_picked);
    // Whether a list was named is decided before the ignore list thins it: an
    // analysis list that is all ignored is still named and must not widen.
    let analysis_named = !analysis_all.is_empty();
    let context_named = !context_all.is_empty();
    let analysis: BTreeSet<&String> = analysis_all.difference(&ignored).collect();
    let context: BTreeSet<&String> = context_all.difference(&ignored).collect();

    let mut out = ElementRoles::default();
    let mut present = BTreeSet::new();
    for element in elements {
        let guid = canonical_guid(element.as_ref());
        let role = if ignored.contains(&guid) {
            ElementRole::Ignored
        } else if !analysis_named {
            if context.contains(&guid) {
                ElementRole::Context
            } else {
                ElementRole::Analysis
            }
        } else if analysis.contains(&guid) {
            ElementRole::Analysis
        } else if !context_named || context.contains(&guid) {
            ElementRole::Context
        } else {
            ElementRole::Ignored
        };
        match role {
            ElementRole::Analysis => out.analysis += 1,
            ElementRole::Context => out.context += 1,
            ElementRole::Ignored => out.ignored += 1,
        }
        out.roles.push(role);
        present.insert(guid);
    }
    out.unmatched_analysis = unmatched(&analysis_all, &present);
    out.unmatched_context = unmatched(&context_all, &present);
    out.unmatched_ignored = unmatched(&ignored, &present);
    out.analysis_named_but_absent = analysis_named && out.analysis == 0;
    out
}

/// Outcome of a live selection-binding refresh. Discriminants are the C ABI
/// values and follow the C++ enum order.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BindingRefresh {
    /// No binding: explicit lists stay fixed.
    Inactive = 0,
    /// Bound sets equal the study's lists canonically.
    Unchanged = 1,
    /// At least one bound list must be replaced by its live set.
    Changed = 2,
    /// The palette declared a newer generation: the binding is dropped and
    /// the study keeps its last explicit lists.
    Detached = 3,
}

/// One bound role list: whether a set is bound, the live set, and the study's
/// current list.
#[derive(Clone, Copy, Debug)]
pub struct BoundList<'a, S> {
    /// The binding names a set for this role.
    pub bound: bool,
    /// The set's current members.
    pub live: &'a [S],
    /// The study's list for this role.
    pub study: &'a [S],
}

/// What a refresh decided; the caller copies `live` into the study list for
/// every `true` flag (an empty live set is an intentional removal).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BindingDecision {
    /// The outcome.
    pub refresh: BindingRefresh,
    /// Replace the context list.
    pub context: bool,
    /// Replace the ignored list.
    pub ignored: bool,
    /// Replace the analysis list.
    pub analysis: bool,
}

/// Canonical sets for change detection. Unlike role resolution, an empty
/// canonical GUID is a member here, exactly as in the C++ binding.
fn binding_set<S: AsRef<str>>(guids: &[S]) -> BTreeSet<String> {
    guids.iter().map(|g| canonical_guid(g.as_ref())).collect()
}

fn changed<S: AsRef<str>>(list: &BoundList<'_, S>) -> bool {
    list.bound && binding_set(list.live) != binding_set(list.study)
}

/// Decide a refresh. `generation == 0` means unbound. Pass `analysis` as
/// `None` when the caller has no analysis list to update.
#[must_use]
pub fn refresh_binding<S: AsRef<str>>(
    generation: u64,
    current_generation: u64,
    lists: [Option<BoundList<'_, S>>; 3],
) -> BindingDecision {
    let quiet = |refresh| BindingDecision {
        refresh,
        context: false,
        ignored: false,
        analysis: false,
    };
    if generation == 0 {
        return quiet(BindingRefresh::Inactive);
    }
    if generation != current_generation {
        return quiet(BindingRefresh::Detached);
    }
    let [context, ignored, analysis] = lists.map(|list| list.as_ref().is_some_and(changed));
    let refresh = if context || ignored || analysis {
        BindingRefresh::Changed
    } else {
        BindingRefresh::Unchanged
    };
    BindingDecision {
        refresh,
        context,
        ignored,
        analysis,
    }
}

#[cfg(test)]
mod tests;
