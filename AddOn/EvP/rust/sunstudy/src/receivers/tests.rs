//! Mirrors `test_sunstudypreset.cpp`.

use super::*;
use crate::roles::{ElementRole, resolve_element_roles};

const BOX: usize = 12;
const NONE: [&str; 0] = [];

fn mesh(materials: &[i32]) -> ReceiverMesh<'_> {
    ReceiverMesh {
        triangle_count: BOX,
        tri_material: materials,
    }
}

fn pool(entries: &[(i32, f64)]) -> BTreeMap<i32, f64> {
    entries.iter().copied().collect()
}

#[test]
fn light_models_recommend_early_and_large_topology_recommends_late() {
    assert!(preset_advice(818_230, "early").is_some_and(|a| a.contains("Late model")));
    assert!(preset_advice(43_959, "late").is_some_and(|a| a.contains("Early model")));
    assert_eq!(preset_advice(818_230, "late"), None);
    assert_eq!(preset_advice(43_959, "early"), None);
}

#[test]
fn unknown_medium_and_boundary_counts_do_not_give_misleading_guidance() {
    assert_eq!(preset_advice(0, "late"), None);
    assert_eq!(preset_advice(100_000, "early"), None);
    assert_eq!(preset_advice(100_000, "late"), None);
    assert_eq!(preset_advice(249_999, "early"), None);
    assert!(preset_advice(250_000, "early").is_some());
    assert!(preset_advice(50_000, "late").is_some());
    assert_eq!(preset_advice(50_001, "late"), None);
}

#[test]
fn glass_faces_only_and_every_other_non_ignored_face_remains_context() {
    let mut pane = [1; BOX];
    pane[2] = 7;
    pane[3] = 7;
    let zeros = [0; BOX];
    let meshes = [mesh(&pane), mesh(&zeros), mesh(&zeros)];
    let roles = resolve_element_roles(&["pane", "wall", "excluded"], &NONE, &NONE, &["excluded"]);
    let result = build_receivers(
        &meshes,
        &roles,
        true,
        &pool(&[(0, 0.0), (1, 0.0), (7, 0.49)]),
        0.4,
    );
    assert_eq!((result.analysis_faces, result.context_faces), (2, 22));
    assert_eq!(
        (
            result.roles.analysis,
            result.roles.context,
            result.roles.ignored
        ),
        (1, 1, 1)
    );
    let mask = result.faces.first().cloned().unwrap_or_default();
    assert_eq!(mask.len(), BOX);
    assert_eq!((mask.first(), mask.get(2)), (Some(&0), Some(&1)));
}

#[test]
fn picked_receivers_and_missing_material_never_widen_to_whole_model() {
    let zeros = [0; BOX];
    let meshes = [mesh(&zeros), mesh(&zeros)];
    let roles = resolve_element_roles(&["picked", "other"], &["picked"], &NONE, &NONE);
    let early = build_receivers(&meshes, &roles, false, &pool(&[]), 0.4);
    assert_eq!((early.analysis_faces, early.context_faces), (12, 12));
    let late = build_receivers(&meshes, &roles, true, &pool(&[]), 0.4);
    assert_eq!((late.analysis_faces, late.context_faces), (0, 24));
    assert_eq!(late.unknown_material_faces, 12);
    assert_eq!(late.roles.context, 2);
}

#[test]
fn threshold_is_inclusive_and_non_finite_transparency_is_unknown() {
    let zeros = [0; BOX];
    let meshes = [mesh(&zeros)];
    let roles = resolve_element_roles(&["glass"], &NONE, &NONE, &NONE);
    let at = |value| build_receivers(&meshes, &roles, true, &pool(&[(0, value)]), 0.4);
    assert_eq!(at(0.4).analysis_faces, 12);
    assert_eq!(at(0.399).analysis_faces, 0);
    let invalid = at(f64::NAN);
    assert_eq!(
        (invalid.analysis_faces, invalid.unknown_material_faces),
        (0, 12)
    );
}

#[test]
fn a_short_material_list_reads_as_unknown() {
    let short = [0; 4];
    let roles = resolve_element_roles(&["glass"], &NONE, &NONE, &NONE);
    let result = build_receivers(&[mesh(&short)], &roles, true, &pool(&[(0, 1.0)]), 0.4);
    assert_eq!(
        (result.analysis_faces, result.unknown_material_faces),
        (4, 8)
    );
    assert_eq!(result.faces.first().map(Vec::len), Some(BOX));
}

#[test]
fn automatic_analysis_applies_context_and_ignored_before_glass_filtering() {
    let mut pane = [1; BOX];
    pane[2] = 7;
    pane[3] = 7;
    let zeros = [0; BOX];
    let opaque = [1; BOX];
    let meshes = [mesh(&pane), mesh(&zeros), mesh(&zeros), mesh(&opaque)];
    let transparency = pool(&[(0, 0.7), (1, 0.0), (7, 0.7)]);
    let roles = resolve_element_roles(
        &["pane", "context", "ignored", "opaque"],
        &NONE,
        &["context"],
        &["ignored"],
    );
    for late in [false, true] {
        let r = build_receivers(&meshes, &roles, late, &transparency, 0.4);
        assert_eq!(r.analysis_faces, if late { 2 } else { 24 });
        assert_eq!(r.context_faces, if late { 34 } else { 12 });
        assert_eq!(r.roles.roles.get(1), Some(&ElementRole::Context));
        assert_eq!(r.roles.roles.get(2), Some(&ElementRole::Ignored));
    }
}

#[test]
fn a_role_count_mismatch_returns_the_roles_unchanged() {
    let zeros = [0; BOX];
    let roles = resolve_element_roles(&["a", "b"], &NONE, &NONE, &NONE);
    let result = build_receivers(&[mesh(&zeros)], &roles, true, &pool(&[]), 0.4);
    assert_eq!(result.roles, roles);
    assert_eq!(result.faces, Vec::<Vec<u8>>::new());
}
