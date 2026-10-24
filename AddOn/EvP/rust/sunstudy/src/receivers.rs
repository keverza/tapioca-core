//! Which faces receive samples, and the preset advice.
//!
//! Port of `SunStudy/SunStudyPreset.cpp`. Transparency is the model pool's 0..1
//! value, not an attribute index or renderer alpha. This selects receiver
//! candidates only: every non-ignored face still casts a binary shadow,
//! including the glass itself.

use crate::roles::{ElementRole, ElementRoles};
use std::collections::BTreeMap;

/// Advice text for a model size and the selected preset; `None` for unknown or
/// medium models. Advisory only, never a mode switch or timing promise.
#[must_use]
pub fn preset_advice(triangle_count: usize, selected_preset: &str) -> Option<&'static str> {
    // Bands anchored by the October live runs: 44K triangles stayed light;
    // 818K/1.59M samples took eight minutes on manual CPU.
    if triangle_count >= 250_000 && selected_preset == "early" {
        return Some("Large model: consider switching to Late model (glass only).");
    }
    if triangle_count > 0 && triangle_count <= 50_000 && selected_preset == "late" {
        return Some("Small model: consider switching to Early model (all surfaces).");
    }
    None
}

/// One mesh's receiver inputs.
#[derive(Clone, Copy, Debug)]
pub struct ReceiverMesh<'a> {
    /// Triangles in the mesh.
    pub triangle_count: usize,
    /// Model-pool material index per triangle; may be shorter than the
    /// triangle count, and a missing entry is an unknown material.
    pub tri_material: &'a [i32],
}

/// Receiver selection. A mesh's `faces` entry is empty for "every face"; a
/// populated mask keeps source-face addresses intact.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Receivers {
    /// Roles after demoting glass-free analysis meshes to context.
    pub roles: ElementRoles,
    /// Per mesh, empty or one byte per triangle (1 = sampled).
    pub faces: Vec<Vec<u8>>,
    /// Faces that will be sampled.
    pub analysis_faces: usize,
    /// Non-ignored faces that only cast shadow.
    pub context_faces: usize,
    /// Faces whose material is missing from the pool or non-finite: never
    /// glass, so they can never widen the study.
    pub unknown_material_faces: usize,
}

/// Select receiver faces. With `glass_only`, an analysis face is sampled when
/// its material's transparency is finite and `>= threshold`. A role count
/// that disagrees with the mesh count returns the roles unchanged and no
/// faces, as the C++ original does.
#[must_use]
pub fn build_receivers(
    meshes: &[ReceiverMesh<'_>],
    roles: &ElementRoles,
    glass_only: bool,
    transparency: &BTreeMap<i32, f64>,
    threshold: f64,
) -> Receivers {
    let mut out = Receivers {
        roles: roles.clone(),
        ..Receivers::default()
    };
    if roles.roles.len() != meshes.len() {
        return out;
    }
    out.faces = vec![Vec::new(); meshes.len()];
    for ((mesh, role), mask) in meshes
        .iter()
        .zip(out.roles.roles.iter_mut())
        .zip(out.faces.iter_mut())
    {
        match *role {
            ElementRole::Ignored => continue,
            ElementRole::Context => {
                out.context_faces += mesh.triangle_count;
                continue;
            }
            ElementRole::Analysis if !glass_only => {
                out.analysis_faces += mesh.triangle_count;
                continue;
            }
            ElementRole::Analysis => {}
        }
        *mask = vec![0; mesh.triangle_count];
        let mut measured = 0;
        for (face, slot) in mask.iter_mut().enumerate() {
            let value = mesh
                .tri_material
                .get(face)
                .and_then(|material| transparency.get(material))
                .filter(|value| value.is_finite());
            let Some(value) = value else {
                out.unknown_material_faces += 1;
                continue;
            };
            if *value >= threshold {
                *slot = 1;
                measured += 1;
            }
        }
        out.analysis_faces += measured;
        out.context_faces += mesh.triangle_count - measured;
        if measured == 0 {
            *role = ElementRole::Context;
            // Wraps like the C++ size_t when a caller passes inconsistent counts.
            out.roles.analysis = out.roles.analysis.wrapping_sub(1);
            out.roles.context += 1;
            mask.clear();
        } else if measured == mesh.triangle_count {
            mask.clear();
        }
    }
    out
}

#[cfg(test)]
mod tests;
