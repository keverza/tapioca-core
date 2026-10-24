//! Mirrors `test_sunstudyroles.cpp`, `test_sunstudyselectionbinding.cpp` and
//! the binding case of `test_sunstudyreceivers.cpp`.

use super::*;
use ElementRole::{Analysis, Context, Ignored};

const ELEMENTS: [&str; 3] = ["{AAAA-1}", "{BBBB-2}", "{CCCC-3}"];
const NONE: [&str; 0] = [];

fn resolve(analysis: &[&str], context: &[&str], ignored: &[&str]) -> ElementRoles {
    resolve_element_roles(&ELEMENTS, analysis, context, ignored)
}

#[test]
fn naming_nothing_analyses_everything() {
    let roles = resolve(&NONE, &NONE, &NONE);
    assert_eq!(roles.analysis, 3);
    assert!(!roles.restricts_occluders());
    assert_eq!(roles.sample_mask(), [1, 1, 1]);
}

#[test]
fn naming_only_context_analyses_everything_else() {
    let roles = resolve(&NONE, &["{BBBB-2}"], &NONE);
    assert_eq!(roles.roles, [Analysis, Context, Analysis]);
    assert!(!roles.restricts_occluders());
}

#[test]
fn naming_only_analysis_keeps_every_other_element_casting_shadow() {
    let roles = resolve(&["{AAAA-1}"], &NONE, &NONE);
    assert_eq!(roles.roles, [Analysis, Context, Context]);
    assert_eq!(roles.ignored, 0);
    assert_eq!(roles.sample_mask(), [1, 0, 0]);
}

#[test]
fn naming_both_ignores_the_rest_and_restricts_the_occluders() {
    let roles = resolve(&["{AAAA-1}"], &["{BBBB-2}"], &NONE);
    assert_eq!(roles.roles, [Analysis, Context, Ignored]);
    assert!(roles.restricts_occluders());
}

#[test]
fn an_element_in_both_lists_is_analysed() {
    let roles = resolve(&["{AAAA-1}"], &["{AAAA-1}", "{BBBB-2}"], &NONE);
    assert_eq!(roles.roles.get(..2), Some(&[Analysis, Context][..]));
}

#[test]
fn guids_match_across_braces_case_and_c_whitespace() {
    let roles = resolve(&["aaaa-1"], &["{bbbb-2}"], &NONE);
    assert_eq!(roles.roles.get(..2), Some(&[Analysis, Context][..]));
    assert_eq!((roles.unmatched_analysis, roles.unmatched_context), (0, 0));
    assert_eq!(canonical_guid(" {a\x0Bb\tc}\r\n"), "ABC");
    assert_eq!(canonical_guid("é{x}"), "éX");
}

#[test]
fn picked_elements_the_snapshot_does_not_hold_are_counted_not_dropped() {
    let roles = resolve(&["{AAAA-1}", "{GONE-9}"], &["{GONE-8}"], &NONE);
    assert_eq!((roles.unmatched_analysis, roles.unmatched_context), (1, 1));
    assert!(!roles.analysis_named_but_absent);
}

#[test]
fn an_ignored_element_neither_casts_shadow_nor_is_measured() {
    let roles = resolve(&NONE, &NONE, &["{CCCC-3}"]);
    assert_eq!(roles.roles, [Analysis, Analysis, Ignored]);
    assert!(roles.restricts_occluders());
}

#[test]
fn the_ignored_list_wins_over_analysis_and_context() {
    let roles = resolve(
        &["{AAAA-1}", "{BBBB-2}"],
        &["{CCCC-3}"],
        &["{BBBB-2}", "{CCCC-3}"],
    );
    assert_eq!(roles.roles, [Analysis, Ignored, Ignored]);
}

#[test]
fn an_analysis_list_that_is_all_ignored_is_refused_not_widened() {
    let roles = resolve(&["{AAAA-1}"], &NONE, &["{AAAA-1}"]);
    assert!(roles.analysis_named_but_absent);
    assert_eq!(roles.analysis, 0);
    assert_eq!(roles.roles.get(1), Some(&Context));
}

#[test]
fn an_ignored_element_the_model_does_not_hold_is_counted() {
    let roles = resolve(&NONE, &NONE, &["{GONE-7}"]);
    assert_eq!(roles.unmatched_ignored, 1);
    assert!(!roles.restricts_occluders());
}

#[test]
fn an_analysis_list_with_nothing_in_the_model_is_not_a_study_of_everything() {
    let roles = resolve(&["{GONE-9}"], &NONE, &NONE);
    assert!(roles.analysis_named_but_absent);
    assert_eq!(roles.analysis, 0);
}

#[test]
fn blank_picks_are_not_a_named_list() {
    let roles = resolve(&["{}", "  "], &NONE, &NONE);
    assert_eq!(roles.analysis, 3);
    assert!(!roles.analysis_named_but_absent);
}

#[expect(
    clippy::unnecessary_wraps,
    reason = "fills the Option slots refresh_binding takes"
)]
fn bound<'a>(live: &'a [&'a str], study: &'a [&'a str]) -> Option<BoundList<'a, &'a str>> {
    Some(BoundList {
        bound: true,
        live,
        study,
    })
}

#[test]
fn binding_reports_which_bound_lists_changed() {
    let decision = refresh_binding(
        8,
        8,
        [
            bound(&[], &["site"]),
            bound(&["excluded"], &["excluded"]),
            None,
        ],
    );
    assert_eq!(decision.refresh, BindingRefresh::Changed);
    assert!(decision.context && !decision.ignored && !decision.analysis);
}

#[test]
fn equivalent_canonical_picks_do_not_invalidate() {
    let decision = refresh_binding(
        8,
        8,
        [
            bound(&["{B}", " A ", "b"], &["a", "b"]),
            bound(&["{C}"], &["c"]),
            None,
        ],
    );
    assert_eq!(decision.refresh, BindingRefresh::Unchanged);
}

#[test]
fn a_newer_generation_detaches_and_an_unbound_study_is_inactive() {
    let lists = [bound(&[], &["site"]), None, None];
    assert_eq!(
        refresh_binding(8, 9, lists).refresh,
        BindingRefresh::Detached
    );
    assert_eq!(
        refresh_binding(0, 9, lists).refresh,
        BindingRefresh::Inactive
    );
    assert!(!refresh_binding(8, 9, lists).context);
}

#[test]
fn an_unbound_role_keeps_its_explicit_list() {
    let unbound = Some(BoundList {
        bound: false,
        live: &[][..],
        study: &["excluded"][..],
    });
    let decision = refresh_binding(8, 8, [bound(&[], &["site"]), unbound, None]);
    assert!(decision.context && !decision.ignored);
}

#[test]
fn live_analysis_clear_is_a_change_and_an_empty_guid_counts() {
    let none = [None, None];
    let [context, ignored] = none;
    let unchanged = refresh_binding(3, 3, [context, ignored, bound(&["{GLASS}"], &["glass"])]);
    assert_eq!(unchanged.refresh, BindingRefresh::Unchanged);
    let cleared = refresh_binding(3, 3, [None, None, bound(&[], &["glass"])]);
    assert!(cleared.analysis);
    // Unlike role resolution, a blank GUID is a member of a binding set.
    assert!(refresh_binding(3, 3, [bound(&["{}"], &[]), None, None]).context);
}
