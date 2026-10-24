//! Exact-sample reuse across an edit: which samples of a converged study may
//! keep their sunlight, and at which steps they must be traced again.
//!
//! Port of `SunStudy/SunStudyReuse.cpp` and the live `ChangedBounds` /
//! `ShadowInfluenceEnvelope` helpers of `SunStudyCache`. Reuse needs exact
//! sample identity on an unchanged element (same canonical GUID, same vertices
//! and triangles, same role, bit-identical position and normal) and is
//! conservative about shadow: a step is dirty when the sunward ray from the
//! sample can meet the OLD or NEW bounds of any changed element. It may
//! over-dirty; it must never under-dirty. The result is a plan; the caller
//! seeds the target session with it.

use crate::occlusion::NO_REUSE;
use crate::raster::{Vec3, cpp_max, cpp_min};
use crate::roles::canonical_guid;
use crate::series::SunSeries;
use std::collections::BTreeMap;

/// An axis-aligned box that starts empty.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ChangedBounds {
    /// Minimum corner.
    pub min: Vec3,
    /// Maximum corner.
    pub max: Vec3,
    /// False until the first point.
    pub valid: bool,
}

impl ChangedBounds {
    /// Grow to include `point`.
    pub fn add(&mut self, point: &Vec3) {
        if !self.valid {
            self.min = *point;
            self.max = *point;
            self.valid = true;
            return;
        }
        for ((lo, hi), p) in self.min.iter_mut().zip(self.max.iter_mut()).zip(point) {
            *lo = cpp_min(*lo, *p);
            *hi = cpp_max(*hi, *p);
        }
    }

    /// Whether a valid box overlaps `[other_min, other_max]` (touching counts).
    #[must_use]
    pub fn intersects(&self, other_min: &Vec3, other_max: &Vec3) -> bool {
        self.valid
            && self
                .min
                .iter()
                .zip(&self.max)
                .zip(other_min.iter().zip(other_max))
                .all(|((lo, hi), (other_lo, other_hi))| !(hi < other_lo || lo > other_hi))
    }
}

/// The box swept away from the sun by `scene_extent` for every direction
/// (pointing TOWARD the sun), unioned with the box itself: a conservative
/// envelope of where its shadow can fall. Sub-1e-12 directions cast nothing.
#[must_use]
pub fn shadow_influence_envelope(
    bounds: &ChangedBounds,
    directions: &[[f64; 3]],
    scene_extent: f64,
) -> ChangedBounds {
    let mut envelope = ChangedBounds::default();
    if !bounds.valid {
        return envelope;
    }
    let (lo, hi) = (bounds.min, bounds.max);
    let corners = [
        [lo[0], lo[1], lo[2]],
        [hi[0], lo[1], lo[2]],
        [lo[0], hi[1], lo[2]],
        [hi[0], hi[1], lo[2]],
        [lo[0], lo[1], hi[2]],
        [hi[0], lo[1], hi[2]],
        [lo[0], hi[1], hi[2]],
        [hi[0], hi[1], hi[2]],
    ];
    for corner in &corners {
        envelope.add(corner);
    }
    if directions.is_empty() || scene_extent.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater)
    {
        return envelope;
    }
    for d in directions {
        let length = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
        if length.partial_cmp(&1e-12) != Some(std::cmp::Ordering::Greater) {
            continue;
        }
        let travel = [
            -d[0] / length * scene_extent,
            -d[1] / length * scene_extent,
            -d[2] / length * scene_extent,
        ];
        for c in &corners {
            envelope.add(&[c[0] + travel[0], c[1] + travel[1], c[2] + travel[2]]);
        }
    }
    envelope
}

/// One snapshot mesh as reuse sees it.
#[derive(Clone, Copy, Debug)]
pub struct ReuseMesh<'a> {
    /// Element GUID, any spelling.
    pub guid: &'a str,
    /// xyz f64 vertices.
    pub vertices: &'a [f64],
    /// Index triples.
    pub triangles: &'a [u32],
}

/// One study's data that reuse compares.
#[derive(Clone, Copy, Debug)]
pub struct ReuseStudy<'a> {
    /// Snapshot meshes.
    pub meshes: &'a [ReuseMesh<'a>],
    /// Role byte per mesh.
    pub roles: &'a [u8],
    /// xyz sample positions.
    pub positions: &'a [f64],
    /// xyz sample normals.
    pub normals: &'a [f64],
    /// Mesh index per sample.
    pub sample_meshes: &'a [u32],
    /// The study's day.
    pub series: &'a SunSeries,
    /// Grid spacing, metres.
    pub grid_spacing: f64,
    /// Sampling domain (0 triangle, 1 patch).
    pub domain: u8,
}

/// The completed source and the unpublished target.
#[derive(Clone, Copy)]
pub struct ReuseRequest<'a> {
    /// The converged source study.
    pub source: ReuseStudy<'a>,
    /// The study being prepared.
    pub target: ReuseStudy<'a>,
    /// Whether the source session converged.
    pub source_converged: bool,
    /// Whether the source traced with the default ray bounds.
    pub source_default_ray_bounds: bool,
    /// Polled between meshes and samples.
    pub is_cancelled: Option<&'a dyn Fn() -> bool>,
}

/// What to seed: per target sample its source sample (or [`NO_REUSE`]) and
/// sample-major dirty-step words.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReusePlan {
    /// Source sample per target sample.
    pub map: Vec<usize>,
    /// Steps to trace again, sample-major 64-bit words.
    pub dirty_steps: Vec<u64>,
    /// Samples with no dirty step at all.
    pub reused_samples: usize,
    /// Sample x steps kept.
    pub reused_sample_steps: usize,
}

fn index_meshes(meshes: &[ReuseMesh<'_>]) -> Option<BTreeMap<String, usize>> {
    let mut index = BTreeMap::new();
    for (i, mesh) in meshes.iter().enumerate() {
        if mesh.vertices.len() % 3 != 0 || !mesh.vertices.iter().all(|c| c.is_finite()) {
            return None;
        }
        let guid = canonical_guid(mesh.guid);
        if guid.is_empty() || index.insert(guid, i).is_some() {
            return None; // ambiguous identity: recompute, never guess
        }
    }
    Some(index)
}

fn bounds_of(mesh: &ReuseMesh<'_>) -> ChangedBounds {
    let mut bounds = ChangedBounds::default();
    for v in mesh.vertices.as_chunks::<3>().0 {
        bounds.add(v);
    }
    bounds
}

#[expect(
    clippy::float_cmp,
    reason = "exact identity of unchanged data, as std::equal in C++"
)]
fn same_sun(a: &SunSeries, b: &SunSeries) -> bool {
    a.version() == b.version()
        && a.step_count() == b.step_count()
        && a.timestep_minutes() == b.timestep_minutes()
        && a.steps()
            .iter()
            .zip(b.steps())
            .all(|(x, y)| x.hour == y.hour && x.minute == y.minute && x.direction == y.direction)
}

fn ray_hits_bounds(point: &[f64], direction: &Vec3, bounds: &ChangedBounds) -> bool {
    let (mut entry, mut exit) = (0.0_f64, f64::INFINITY);
    let axes = point
        .iter()
        .zip(direction)
        .zip(bounds.min.iter().zip(&bounds.max));
    for ((&p, &d), (&lo, &hi)) in axes {
        if d == 0.0 {
            if p < lo || p > hi {
                return false;
            }
            continue;
        }
        let mut near = (lo - p) / d;
        let mut far = (hi - p) / d;
        if near > far {
            std::mem::swap(&mut near, &mut far);
        }
        entry = cpp_max(entry, near);
        exit = cpp_min(exit, far);
        if entry > exit {
            return false;
        }
    }
    true
}

/// One changed element's old and new bounds (either may be invalid).
type Change = (ChangedBounds, ChangedBounds);

struct Zone {
    bounds: ChangedBounds,
    envelope: ChangedBounds,
}

/// Mark the steps whose sunward ray from `point` meets a zone's box; returns
/// how many steps are dirty. `dirty` must be cleared words for this sample.
fn dirty_shadow_steps(
    point: &[f64],
    normal: &[f64],
    zones: &[Zone],
    directions: &[[f64; 3]],
    dirty: &mut [u64],
) -> usize {
    dirty.fill(0);
    let Ok(at) = <[f64; 3]>::try_from(point) else {
        return 0;
    };
    let n = <[f64; 3]>::try_from(normal).unwrap_or([f64::NAN; 3]);
    let mut count = 0;
    for zone in zones.iter().filter(|z| z.envelope.intersects(&at, &at)) {
        for (step, d) in directions.iter().enumerate() {
            let bit = 1_u64 << (step % 64);
            let Some(word) = dirty.get_mut(step / 64) else {
                continue;
            };
            if *word & bit != 0 {
                continue;
            }
            // An unchanged normal still proves self-shadowing.
            if n[0] * d[0] + n[1] * d[1] + n[2] * d[2] <= 0.0 {
                continue;
            }
            if ray_hits_bounds(point, d, &zone.bounds) {
                *word |= bit;
                count += 1;
            }
        }
    }
    count
}

fn cancelled(request: &ReuseRequest<'_>) -> bool {
    request.is_cancelled.is_some_and(|f| f())
}

#[expect(
    clippy::float_cmp,
    reason = "grid spacing must be identical, as C++ !="
)]
fn compatible(r: &ReuseRequest<'_>) -> bool {
    let (s, t) = (&r.source, &r.target);
    r.source_default_ray_bounds
        && r.source_converged
        && same_sun(s.series, t.series)
        && s.positions.len() % 3 == 0
        && t.positions.len() % 3 == 0
        && s.grid_spacing == t.grid_spacing
        && s.domain == t.domain
        && s.sample_meshes.len() == s.positions.len() / 3
        && t.sample_meshes.len() == t.positions.len() / 3
        && s.normals.len() == s.positions.len()
        && t.normals.len() == t.positions.len()
        && s.roles.len() == s.meshes.len()
        && t.roles.len() == t.meshes.len()
}

/// Changes between the snapshots and, per target mesh, its unchanged source
/// mesh; `None` when identity is ambiguous or the request is cancelled.
fn changes(r: &ReuseRequest<'_>) -> Option<(Vec<Change>, Vec<usize>)> {
    let before = index_meshes(r.source.meshes)?;
    let after = index_meshes(r.target.meshes)?;
    let mut list = Vec::new();
    let mut old_mesh = vec![NO_REUSE; r.target.meshes.len()];
    for (guid, &i) in &before {
        if cancelled(r) {
            return None;
        }
        let mesh = r.source.meshes.get(i)?;
        match after.get(guid) {
            None => list.push((bounds_of(mesh), ChangedBounds::default())),
            Some(&next) => {
                let target = r.target.meshes.get(next)?;
                let same = mesh.vertices == target.vertices
                    && mesh.triangles == target.triangles
                    && r.source.roles.get(i) == r.target.roles.get(next);
                if same {
                    *old_mesh.get_mut(next)? = i;
                } else {
                    list.push((bounds_of(mesh), bounds_of(target)));
                }
            }
        }
    }
    for (guid, &next) in &after {
        if !before.contains_key(guid) {
            list.push((
                ChangedBounds::default(),
                bounds_of(r.target.meshes.get(next)?),
            ));
        }
    }
    Some((list, old_mesh))
}

fn scene_extent(r: &ReuseRequest<'_>) -> f64 {
    let mut scene = ChangedBounds::default();
    for mesh in r.source.meshes.iter().chain(r.target.meshes) {
        let b = bounds_of(mesh);
        if b.valid {
            scene.add(&b.min);
            scene.add(&b.max);
        }
    }
    for p in r.target.positions.as_chunks::<3>().0 {
        scene.add(p);
    }
    let diagonal: f64 = scene
        .max
        .iter()
        .zip(&scene.min)
        .map(|(hi, lo)| (hi - lo) * (hi - lo))
        .fold(0.0, |sum, v| sum + v);
    diagonal.sqrt() + 0.001
}

fn zones(
    list: &[(ChangedBounds, ChangedBounds)],
    directions: &[[f64; 3]],
    extent: f64,
) -> Vec<Zone> {
    let mut out = Vec::new();
    for (old, new) in list {
        for bounds in [old, new].into_iter().filter(|b| b.valid) {
            let mut padded = *bounds;
            for (lo, hi) in padded.min.iter_mut().zip(padded.max.iter_mut()) {
                // Conservative roundoff margin, including survey coordinates.
                let pad = 1.0e-6 + cpp_max(lo.abs(), hi.abs()) * 1.0e-12;
                *lo -= pad;
                *hi += pad;
            }
            out.push(Zone {
                bounds: padded,
                envelope: shadow_influence_envelope(&padded, directions, extent),
            });
        }
    }
    out
}

/// Plan the reuse of a converged source study for a target study. `None`
/// means "no reuse" (incompatible, ambiguous, more than 256 changed elements,
/// nothing reusable, or cancelled).
#[must_use]
pub fn plan_reuse(request: &ReuseRequest<'_>) -> Option<ReusePlan> {
    if !compatible(request) {
        return None;
    }
    let extent = scene_extent(request);
    let (list, old_mesh) = changes(request)?;
    if list.len() > 256 {
        return None; // bulk replacement is not a local edit
    }
    let (source, target) = (&request.source, &request.target);
    let directions: Vec<[f64; 3]> = target.series.steps().iter().map(|s| s.direction).collect();
    let zones = zones(&list, &directions, extent);
    let mut samples_before: Vec<Vec<usize>> = vec![Vec::new(); source.meshes.len()];
    for (sample, &mesh) in source.sample_meshes.iter().enumerate() {
        samples_before
            .get_mut(usize::try_from(mesh).ok()?)?
            .push(sample);
    }
    let steps = target.series.step_count();
    let words = steps.div_ceil(64);
    if words == 0 {
        return None;
    }
    let count = target.sample_meshes.len();
    let mut dirty = vec![u64::MAX; count * words];
    if steps % 64 != 0 {
        for sample in 0..count {
            if let Some(last) = dirty.get_mut(sample * words + words - 1) {
                *last = (1 << (steps % 64)) - 1;
            }
        }
    }
    let mut cursors = vec![0_usize; old_mesh.len()];
    let mut map = vec![NO_REUSE; count];
    let (mut reused, mut reused_steps) = (0, 0);
    for (sample, &mesh) in target.sample_meshes.iter().enumerate() {
        if cancelled(request) {
            return None;
        }
        let mesh = usize::try_from(mesh).ok().filter(|m| *m < old_mesh.len())?;
        let old = old_mesh.get(mesh).copied().unwrap_or(NO_REUSE);
        let cursor = cursors.get_mut(mesh)?;
        let index = *cursor;
        *cursor += 1;
        let Some(&from) = (old != NO_REUSE)
            .then(|| samples_before.get(old).and_then(|s| s.get(index)))
            .flatten()
        else {
            continue;
        };
        let point = target.positions.get(sample * 3..sample * 3 + 3)?;
        let normal = target.normals.get(sample * 3..sample * 3 + 3)?;
        let identical = Some(point) == source.positions.get(from * 3..from * 3 + 3)
            && Some(normal) == source.normals.get(from * 3..from * 3 + 3);
        if !identical {
            continue;
        }
        let words_of = dirty.get_mut(sample * words..sample * words + words)?;
        let dirty_count = dirty_shadow_steps(point, normal, &zones, &directions, words_of);
        *map.get_mut(sample)? = from;
        reused += usize::from(dirty_count == 0);
        reused_steps += steps - dirty_count;
    }
    if reused_steps == 0 || cancelled(request) {
        return None;
    }
    Some(ReusePlan {
        map,
        dirty_steps: dirty,
        reused_samples: reused,
        reused_sample_steps: reused_steps,
    })
}

/// A patch span's owning element and sample range.
#[derive(Clone, Copy, Debug)]
pub struct SpanOwner<'a> {
    /// The span key's element GUID.
    pub element: &'a str,
    /// First sample.
    pub first: usize,
    /// Sample count.
    pub count: usize,
}

/// Why a patch-domain sample map could not be built.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SampleMeshError {
    /// Empty, duplicate or non-finite mesh identity: C++ leaves the record's
    /// existing map untouched.
    Ambiguous,
    /// A span names a missing mesh or overruns the samples: C++ clears the map.
    BadSpan,
}

/// Mesh index per sample for a patch-domain study (`SetSampleMeshes`): each
/// span's samples take the index of the mesh whose canonical GUID matches the
/// span's element; unclaimed samples keep `u32::MAX`.
///
/// # Errors
/// [`SampleMeshError`] as described on its variants.
pub fn patch_sample_meshes(
    meshes: &[ReuseMesh<'_>],
    sample_count: usize,
    spans: &[SpanOwner<'_>],
) -> Result<Vec<u32>, SampleMeshError> {
    let index = index_meshes(meshes).ok_or(SampleMeshError::Ambiguous)?;
    let mut out = vec![u32::MAX; sample_count];
    for span in spans {
        let mesh = *index
            .get(&canonical_guid(span.element))
            .ok_or(SampleMeshError::BadSpan)?;
        if span.first > sample_count || span.count > sample_count - span.first {
            return Err(SampleMeshError::BadSpan);
        }
        let value = u32::try_from(mesh).unwrap_or(u32::MAX);
        out.get_mut(span.first..span.first + span.count)
            .ok_or(SampleMeshError::BadSpan)?
            .fill(value);
    }
    Ok(out)
}

#[cfg(test)]
mod tests;
