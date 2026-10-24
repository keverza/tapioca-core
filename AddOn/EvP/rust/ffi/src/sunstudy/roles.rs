//! Role resolution, receiver selection, preset advice and binding refresh.
//!
//! GUIDs cross as length-delimited UTF-8; non-UTF-8 text is refused (the C++
//! original accepts arbitrary bytes, but host GUIDs are ASCII).

use super::{Regions, flag};
use crate::borrow::{BorrowContext, SliceView};
use crate::boundary::{Failure, call};
use std::collections::BTreeMap;
use tapioca_sunstudy::receivers::{self, ReceiverMesh};
use tapioca_sunstudy::roles::{self, BoundList, ElementRole, ElementRoles};

type TextList = SliceView<SliceView<u8>>;

// SAFETY: every text in `list` was registered and checked through `Regions`.
unsafe fn strings<'a>(
    context: &'a BorrowContext,
    list: &[SliceView<u8>],
) -> Result<Vec<&'a str>, Failure> {
    list.iter()
        // SAFETY: forwarded from the caller's checked registration.
        .map(|text| unsafe { context.text(*text) })
        .collect()
}

/// Four GUID lists: the snapshot's elements and the three picks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RolesRequest {
    /// One GUID per element, in element order.
    pub elements: TextList,
    /// Analysis picks.
    pub analysis: TextList,
    /// Context picks.
    pub context: TextList,
    /// Ignored picks.
    pub ignored: TextList,
}

/// Counts that accompany the per-element roles.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RolesSummary {
    /// Analysis, context and ignored element counts.
    pub counts: [usize; 3],
    /// Unmatched analysis, context and ignored picks.
    pub unmatched: [usize; 3],
    /// 1 when a named analysis list left nothing to measure.
    pub analysis_named_but_absent: u8,
}

/// Caller-owned outputs, unchanged on refusal.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RolesOutput {
    /// One role byte per element (0 analysis, 1 context, 2 ignored).
    pub roles: *mut u8,
    /// Capacity in bytes; at least the element count.
    pub capacity: usize,
    /// Required summary record.
    pub summary: *mut RolesSummary,
}

fn summary(resolved: &ElementRoles) -> RolesSummary {
    RolesSummary {
        counts: [resolved.analysis, resolved.context, resolved.ignored],
        unmatched: [
            resolved.unmatched_analysis,
            resolved.unmatched_context,
            resolved.unmatched_ignored,
        ],
        analysis_named_but_absent: u8::from(resolved.analysis_named_but_absent),
    }
}

/// Resolve every element's role from the three pick lists.
///
/// # Safety
/// The request, every list and every text must be live, initialized and
/// immutable. The role capacity and summary must be live, aligned, exclusively
/// writable and disjoint from every other region and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_resolve_roles(
    request: *const RolesRequest,
    output: *const RolesOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            let lists = [
                regions.text_list(request.elements)?,
                regions.text_list(request.analysis)?,
                regions.text_list(request.context)?,
                regions.text_list(request.ignored)?,
            ];
            regions.output(target.roles, target.capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            if target.capacity < request.elements.count {
                return Err(Failure::Argument("RolesOutputTooSmall"));
            }
            let context = regions.context();
            let [elements, analysis, picked_context, ignored] = [
                strings(&context, &lists[0])?,
                strings(&context, &lists[1])?,
                strings(&context, &lists[2])?,
                strings(&context, &lists[3])?,
            ];
            let resolved =
                roles::resolve_element_roles(&elements, &analysis, &picked_context, &ignored);
            let bytes: Vec<u8> = resolved.roles.iter().map(|r| *r as u8).collect();
            // SAFETY: bytes.len() == element count <= checked disjoint capacity.
            if !bytes.is_empty() {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), target.roles, bytes.len());
            }
            target.summary.write(summary(&resolved));
            Ok(())
        })
    }
}

/// One mesh's receiver inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReceiverMeshView {
    /// Triangles in the mesh.
    pub triangle_count: usize,
    /// Model-pool material per triangle; may be shorter than the count.
    pub tri_material: SliceView<i32>,
}

/// One model-pool transparency entry.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MaterialTransparency {
    /// Model-pool material index.
    pub material: i32,
    /// 0..1 transparency.
    pub transparency: f64,
}

/// Receiver-selection inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReceiversRequest {
    /// One view per mesh.
    pub meshes: SliceView<ReceiverMeshView>,
    /// One role byte per mesh (0, 1 or 2).
    pub roles: SliceView<u8>,
    /// Transparency pool; material indices must be unique.
    pub transparency: SliceView<MaterialTransparency>,
    /// Inclusive glass threshold.
    pub threshold: f64,
    /// 1 to sample glass faces only, else 0.
    pub glass_only: u8,
}

/// Face and role counts after selection.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReceiversSummary {
    /// Sampled faces.
    pub analysis_faces: usize,
    /// Non-ignored faces that only cast shadow.
    pub context_faces: usize,
    /// Faces with a missing or non-finite material.
    pub unknown_material_faces: usize,
    /// Analysis, context and ignored mesh counts after demotion.
    pub counts: [usize; 3],
    /// Total face-mask bytes written (sum of triangle counts).
    pub mask_count: usize,
}

/// Caller-owned outputs, unchanged on refusal.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReceiversOutput {
    /// One role byte per mesh after demotion.
    pub roles: *mut u8,
    /// One byte per mesh: 1 when its face mask is populated, 0 for "every face".
    pub has_mask: *mut u8,
    /// Capacity of `roles` and `has_mask` in bytes; at least the mesh count.
    pub mesh_capacity: usize,
    /// Concatenated per-mesh face masks, zero for meshes without a mask.
    pub face_mask: *mut u8,
    /// Capacity in bytes; at least the sum of triangle counts.
    pub mask_capacity: usize,
    /// Required summary record.
    pub summary: *mut ReceiversSummary,
}

fn decode_role(byte: u8) -> Result<ElementRole, Failure> {
    match byte {
        0 => Ok(ElementRole::Analysis),
        1 => Ok(ElementRole::Context),
        2 => Ok(ElementRole::Ignored),
        _ => Err(Failure::Argument("ReceiversRole: expected 0, 1 or 2")),
    }
}

fn role_counts(roles: Vec<ElementRole>) -> ElementRoles {
    let count = |want| roles.iter().filter(|r| **r == want).count();
    ElementRoles {
        analysis: count(ElementRole::Analysis),
        context: count(ElementRole::Context),
        ignored: count(ElementRole::Ignored),
        roles,
        ..ElementRoles::default()
    }
}

/// Select receiver faces per mesh.
///
/// # Safety
/// The request and all of its arrays must be live, initialized and immutable.
/// Every output array at its declared capacity and the summary must be live,
/// aligned, exclusively writable and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_receivers(
    request: *const ReceiversRequest,
    output: *const ReceiversOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            regions.input(request.meshes.data, request.meshes.count);
            regions.input(request.roles.data, request.roles.count);
            regions.input(request.transparency.data, request.transparency.count);
            regions.check()?;
            let meshes = {
                let context = regions.context();
                context.slice(request.meshes)?.to_vec()
            };
            for mesh in &meshes {
                regions.input(mesh.tri_material.data, mesh.tri_material.count);
            }
            regions.output(target.roles, target.mesh_capacity);
            regions.output(target.has_mask, target.mesh_capacity);
            regions.output(target.face_mask, target.mask_capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let glass_only = flag(request.glass_only, "ReceiversGlassOnly: expected 0 or 1")?;
            let role_list: Vec<ElementRole> = context
                .slice(request.roles)?
                .iter()
                .map(|b| decode_role(*b))
                .collect::<Result<_, _>>()?;
            if role_list.len() != meshes.len() {
                return Err(Failure::Argument("ReceiversRoleCount: one role per mesh"));
            }
            let mut pool = BTreeMap::new();
            for entry in context.slice(request.transparency)? {
                if pool.insert(entry.material, entry.transparency).is_some() {
                    return Err(Failure::Argument("ReceiversDuplicateMaterial"));
                }
            }
            let views: Vec<ReceiverMesh<'_>> = meshes
                .iter()
                .map(|m| {
                    Ok(ReceiverMesh {
                        triangle_count: m.triangle_count,
                        tri_material: context.slice(m.tri_material)?,
                    })
                })
                .collect::<Result<_, Failure>>()?;
            let total = views
                .iter()
                .try_fold(0_usize, |sum, m| sum.checked_add(m.triangle_count))
                .ok_or(Failure::Argument("ReceiversTooManyFaces"))?;
            if target.mesh_capacity < meshes.len() || target.mask_capacity < total {
                return Err(Failure::Argument("ReceiversOutputTooSmall"));
            }
            let result = receivers::build_receivers(
                &views,
                &role_counts(role_list),
                glass_only,
                &pool,
                request.threshold,
            );
            let mut mask = Vec::with_capacity(total);
            let mut has_mask = Vec::with_capacity(meshes.len());
            for (view, faces) in views.iter().zip(&result.faces) {
                has_mask.push(u8::from(!faces.is_empty()));
                if faces.is_empty() {
                    mask.resize(mask.len() + view.triangle_count, 0);
                } else {
                    mask.extend_from_slice(faces);
                }
            }
            let roles: Vec<u8> = result.roles.roles.iter().map(|r| *r as u8).collect();
            // SAFETY: lengths equal the checked mesh/mask counts <= capacities.
            if !roles.is_empty() {
                std::ptr::copy_nonoverlapping(roles.as_ptr(), target.roles, roles.len());
                std::ptr::copy_nonoverlapping(has_mask.as_ptr(), target.has_mask, has_mask.len());
            }
            if !mask.is_empty() {
                std::ptr::copy_nonoverlapping(mask.as_ptr(), target.face_mask, mask.len());
            }
            target.summary.write(ReceiversSummary {
                analysis_faces: result.analysis_faces,
                context_faces: result.context_faces,
                unknown_material_faces: result.unknown_material_faces,
                counts: [
                    result.roles.analysis,
                    result.roles.context,
                    result.roles.ignored,
                ],
                mask_count: mask.len(),
            });
            Ok(())
        })
    }
}

/// Preset-advice inputs.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PresetAdviceRequest {
    /// Model triangle count; 0 is unknown.
    pub triangle_count: usize,
    /// Selected preset name ("early" or "late").
    pub preset: SliceView<u8>,
}

/// Caller-owned advice text (UTF-8, not NUL-terminated).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PresetAdviceOutput {
    /// Text buffer; null only at zero capacity.
    pub text: *mut u8,
    /// Capacity in bytes; must hold the advice (under 80 bytes).
    pub capacity: usize,
    /// Required length out; 0 when there is no advice.
    pub length: *mut usize,
}

/// Advice for a model size and the selected preset.
///
/// # Safety
/// The request, its text and the output descriptor must be live and
/// immutable; the text buffer and length must be live, aligned, exclusively
/// writable and disjoint from every other region.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_preset_advice(
    request: *const PresetAdviceRequest,
    output: *const PresetAdviceOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            regions.input(request.preset.data, request.preset.count);
            regions.output(target.text, target.capacity);
            regions.output(target.length, 1);
            regions.check()?;
            let context = regions.context();
            let preset = context.text(request.preset)?;
            let advice = receivers::preset_advice(request.triangle_count, preset).unwrap_or("");
            if advice.len() > target.capacity {
                return Err(Failure::Argument("PresetAdviceOutputTooSmall"));
            }
            // SAFETY: advice.len() <= checked disjoint capacity.
            if !advice.is_empty() {
                std::ptr::copy_nonoverlapping(advice.as_ptr(), target.text, advice.len());
            }
            target.length.write(advice.len());
            Ok(())
        })
    }
}

/// One bound role list.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BoundListView {
    /// 1 when the caller supplies this list, else 0 (the list is ignored).
    pub present: u8,
    /// 1 when the binding names a set for this role, else 0.
    pub bound: u8,
    /// The set's live members.
    pub live: TextList,
    /// The study's current list.
    pub study: TextList,
}

/// Binding-refresh inputs: context, ignored and analysis lists in that order.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BindingRequest {
    /// The binding's generation; 0 = unbound.
    pub generation: u64,
    /// The palette's current declaration generation.
    pub current_generation: u64,
    /// Context, ignored, analysis.
    pub lists: [BoundListView; 3],
}

/// Outcome and which lists to replace with their live sets.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BindingOutput {
    /// 0 inactive, 1 unchanged, 2 changed, 3 detached.
    pub refresh: u8,
    /// 1 to replace the context, ignored, analysis list respectively.
    pub replace: [u8; 3],
}

/// Decide a live selection-binding refresh.
///
/// # Safety
/// The request and every list/text must be live, initialized and immutable;
/// the output must be live, aligned, exclusively writable and disjoint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_refresh_binding(
    request: *const BindingRequest,
    output: *mut BindingOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.output(output, 1);
            regions.check()?;
            let request = regions.context().record(request)?;
            let mut texts = Vec::with_capacity(3);
            for list in &request.lists {
                let present = flag(list.present, "BindingPresent: expected 0 or 1")?;
                let bound = flag(list.bound, "BindingBound: expected 0 or 1")?;
                let live = regions.text_list(list.live)?;
                let study = regions.text_list(list.study)?;
                texts.push((present, bound, live, study));
            }
            regions.check()?;
            let context = regions.context();
            let mut owned = Vec::with_capacity(3);
            for (present, bound, live, study) in &texts {
                owned.push((
                    *present,
                    *bound,
                    strings(&context, live)?,
                    strings(&context, study)?,
                ));
            }
            let mut slots = [None, None, None];
            for (slot, (present, bound, live, study)) in slots.iter_mut().zip(&owned) {
                if *present {
                    *slot = Some(BoundList {
                        bound: *bound,
                        live: live.as_slice(),
                        study: study.as_slice(),
                    });
                }
            }
            let decision =
                roles::refresh_binding(request.generation, request.current_generation, slots);
            output.write(BindingOutput {
                refresh: decision.refresh as u8,
                replace: [
                    u8::from(decision.context),
                    u8::from(decision.ignored),
                    u8::from(decision.analysis),
                ],
            });
            Ok(())
        })
    }
}
