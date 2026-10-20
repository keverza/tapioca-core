//! Legacy capture mesh preparation, behind the G1 parity seam.
//!
//! Source positions are f64 world metres, right-handed Z-up. This module mirrors
//! `VertexWeld`, `TriangleDegeneracy` and `WireframeEdges`, not an editor remesher.
//! Analysis keeps vertex/face order; welding returns EVERY corner-to-vertex
//! correspondence and resets its source-local key table at each body boundary.
//! C++ retains native GUIDs, bounds spanning unreferenced body vertices, all face
//! sidecars and authored identity. These primitives do not allocate semantic IDs
//! or certify D14 generated ancestry. Bulk work stays worker-classified.

use crate::mesh::{self, MeshView};
use crate::value::Aabb;

mod weld;
pub use weld::{WeldCorner, WeldRequest, WeldedCorners, weld_corners};

/// Static, typed input/arithmetic refusals shared by this primitive family.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Mesh geometry or optional compatibility sidecars are malformed.
    Mesh,
    /// Body boundaries, corner counts or visibility ranges are malformed.
    Shape,
    /// Every supplied position/normal must be finite.
    NonFinite,
    /// A normal's rounded 1e4-quantized key is not representable as i32.
    NormalQuantum,
    /// Visibility keys must be canonical and sorted within every used range.
    WireKeys,
    /// Derived arithmetic or float-normal storage is not representable.
    Arithmetic,
}

impl Error {
    /// Stable static diagnostic, also used by the C boundary.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Mesh => "MeshPrepMesh: invalid mesh geometry or sidecar shape",
            Self::Shape => "MeshPrepShape: invalid corner/body/visibility range or count",
            Self::NonFinite => "MeshPrepNonFinite: positions and normals must be finite",
            Self::NormalQuantum => "MeshPrepNormalQuantum: quantized normal exceeds i32",
            Self::WireKeys => "MeshPrepWireKeys: expected canonical locally sorted edge keys",
            Self::Arithmetic => {
                "MeshPrepArithmetic: unrepresentable normal arithmetic or float storage"
            }
        }
    }
}

/// A flat visibility-pool range, in edge-key elements, not bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EdgeRange {
    /// First key in the shared visibility pool.
    pub offset: u64,
    /// Number of keys; duplicates are permitted.
    pub count: u64,
}

/// Borrowed non-topology-changing mesh analysis input.
pub struct AnalysisRequest<'a> {
    /// Geometry-only legacy view; all supplied sidecars are validated unchanged.
    pub mesh: MeshView<'a>,
    /// Canonical `(min_vertex << 32 | max_vertex)` source edge keys.
    pub visible_edges: &'a [u64],
    /// One range per triangle. Empty means the whole pool applies to every
    /// triangle. Each selected range must be sorted, as in the C++ oracle.
    pub edge_ranges: &'a [EdgeRange],
}

/// Complete newly owned derived values, published together by the boundary.
pub struct MeshAnalysis {
    /// Tight bounds of the supplied vertices, NOT retained extractor body bounds.
    pub bounds: Aabb,
    /// Existing source normals satisfy the C++ 0.9..1.1 length test.
    pub normals_usable: bool,
    /// Area-weighted fallback normals, same f32 accumulation order as C++.
    pub smooth_normals: Vec<f32>,
    /// One 0/1 roundoff-zero classification per triangle; nothing is removed.
    pub roundoff_zero: Vec<u8>,
    /// One barycentric-domain visibility mask per triangle; no topology change.
    pub wire_masks: Vec<u8>,
}

/// Analyze without repairing geometry, changing topology or replacing identity.
///
/// The legacy normal fallback intentionally smooths hard edges. Per-corner
/// normals plus [`weld_corners`] remain the primary shading path. Empty meshes,
/// unused vertices and degenerate triangles remain allowed. Bounds use exact
/// min/max with the canonical empty infinity envelope, no epsilon/origin shift.
///
/// # Errors
/// Malformed mesh/ranges/keys or nonfinite/unrepresentable arithmetic refuse the
/// whole result. C++ accidental NaN/inf successes are not a parity contract.
pub fn analyze_mesh(input: &AnalysisRequest<'_>) -> Result<MeshAnalysis, Error> {
    mesh::validate_mesh(&input.mesh).map_err(|_| Error::Mesh)?;
    let (vertices, _) = input.mesh.vertices.as_chunks::<3>();
    let (triangles, _) = input.mesh.triangles.as_chunks::<3>();
    validate_visibility(input, triangles.len())?;
    let mut bounds = Aabb {
        mn: [f64::INFINITY; 3],
        mx: [f64::NEG_INFINITY; 3],
    };
    for point in vertices {
        for ((mn, mx), &value) in bounds.mn.iter_mut().zip(&mut bounds.mx).zip(point) {
            // Comparisons preserve first/signed-zero behavior of Aabb::Expand.
            if value < *mn {
                *mn = value;
            }
            if value > *mx {
                *mx = value;
            }
        }
    }
    let mut result = MeshAnalysis {
        bounds,
        normals_usable: normals_are_usable(input.mesh.normals, input.mesh.vertices.len()),
        smooth_normals: vec![0.0; input.mesh.vertices.len()],
        roundoff_zero: Vec::with_capacity(triangles.len()),
        wire_masks: Vec::with_capacity(triangles.len()),
    };
    for (index, &[a, b, c]) in triangles.iter().enumerate() {
        let point = |i: u32| {
            vertices
                .get(usize::try_from(i).map_err(|_| Error::Mesh)?)
                .copied()
                .ok_or(Error::Mesh)
        };
        let points = [point(a)?, point(b)?, point(c)?];
        result
            .roundoff_zero
            .push(u8::from(roundoff_zero_triangle(points)));
        let normal = triangle_normal(points)?;
        for i in [a, b, c] {
            let offset = usize::try_from(i)
                .map_err(|_| Error::Mesh)?
                .checked_mul(3)
                .ok_or(Error::Mesh)?;
            let target = result
                .smooth_normals
                .get_mut(offset..offset.checked_add(3).ok_or(Error::Mesh)?)
                .ok_or(Error::Mesh)?;
            for (sum, component) in target.iter_mut().zip(normal) {
                let weight = stored_normal(component);
                *sum += weight;
                if !weight.is_finite() || !sum.is_finite() {
                    return Err(Error::Arithmetic);
                }
            }
        }
        let visible = visibility_for(input, index)?;
        let mask =
            [(b, c), (c, a), (a, b)]
                .into_iter()
                .enumerate()
                .fold(0, |mask, (bit, (x, y))| {
                    mask | if visible.binary_search(&wire_edge_key(x, y)).is_ok() {
                        1 << bit
                    } else {
                        0
                    }
                });
        result.wire_masks.push(mask);
    }
    let (normals, _) = result.smooth_normals.as_chunks_mut::<3>();
    for normal in normals {
        let mut value = normal.map(f64::from);
        if normalize(&mut value)? {
            for (target, value) in normal.iter_mut().zip(value) {
                *target = stored_normal(value);
            }
        }
    }
    Ok(result)
}

fn wire_edge_key(a: u32, b: u32) -> u64 {
    (u64::from(a.min(b)) << 32) | u64::from(a.max(b))
}

fn validate_visibility(input: &AnalysisRequest<'_>, triangles: usize) -> Result<(), Error> {
    if !input.edge_ranges.is_empty() && input.edge_ranges.len() != triangles {
        return Err(Error::Shape);
    }
    if input
        .visible_edges
        .iter()
        .any(|&key| (key >> 32) > (key & 0xffff_ffff))
    {
        return Err(Error::WireKeys);
    }
    // A global visibility pool is validated once, not once per triangle.
    let ranges = if input.edge_ranges.is_empty() {
        1
    } else {
        triangles
    };
    for index in 0..ranges {
        let keys = visibility_for(input, index)?;
        if keys.iter().zip(keys.iter().skip(1)).any(|(a, b)| a > b) {
            return Err(Error::WireKeys);
        }
    }
    Ok(())
}

fn visibility_for<'a>(input: &'a AnalysisRequest<'_>, triangle: usize) -> Result<&'a [u64], Error> {
    if input.edge_ranges.is_empty() {
        return Ok(input.visible_edges);
    }
    let range = input.edge_ranges.get(triangle).ok_or(Error::Shape)?;
    let start = usize::try_from(range.offset).map_err(|_| Error::Shape)?;
    let count = usize::try_from(range.count).map_err(|_| Error::Shape)?;
    let end = start.checked_add(count).ok_or(Error::Shape)?;
    input.visible_edges.get(start..end).ok_or(Error::Shape)
}

fn normals_are_usable(normals: &[f32], coordinates: usize) -> bool {
    let (triples, _) = normals.as_chunks::<3>();
    normals.len() == coordinates
        && !normals.is_empty()
        && triples.iter().all(|n| {
            let [x, y, z] = n.map(f64::from);
            let squared = x * x + y * y + z * z;
            (0.9 * 0.9..=1.1 * 1.1).contains(&squared)
        })
}

fn normalize(value: &mut [f64; 3]) -> Result<bool, Error> {
    let [x, y, z] = *value;
    let length = (x * x + y * y + z * z).sqrt();
    if !length.is_finite() {
        return Err(Error::Arithmetic);
    }
    if length <= 1e-12 {
        return Ok(false);
    }
    for component in value {
        *component /= length;
    }
    Ok(true)
}

fn triangle_normal([a, b, c]: [[f64; 3]; 3]) -> Result<[f64; 3], Error> {
    let [ux, uy, uz] = difference(b, a);
    let [vx, vy, vz] = difference(c, a);
    let normal = [uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx];
    if normal.iter().all(|v| v.is_finite()) {
        Ok(normal)
    } else {
        Err(Error::Arithmetic)
    }
}

fn roundoff_zero_triangle([first, second, third]: [[f64; 3]; 3]) -> bool {
    if !first
        .iter()
        .chain(&second)
        .chain(&third)
        .all(|v| v.is_finite())
    {
        return false;
    }
    let mut u = difference(second, first);
    let mut v = difference(third, first);
    let scale = u.iter().chain(&v).map(|x| x.abs()).fold(0.0_f64, f64::max);
    if !scale.is_finite() {
        return false;
    }
    if scale <= 0.0 {
        return true;
    }
    for component in u.iter_mut().chain(&mut v) {
        *component /= scale;
    }
    let [ux, uy, uz] = u;
    let [vx, vy, vz] = v;
    let cross = [uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx];
    // MSVC's three-argument hypot scales by its greatest component first.
    let greatest = cross.iter().map(|x| x.abs()).fold(0.0_f64, f64::max);
    if greatest <= 0.0 {
        return true;
    }
    let [cx, cy, cz] = cross.map(|component| component / greatest);
    greatest * (cx * cx + cy * cy + cz * cz).sqrt() <= 8.0 * f64::EPSILON
}

fn difference([x, y, z]: [f64; 3], [origin_x, origin_y, origin_z]: [f64; 3]) -> [f64; 3] {
    [x - origin_x, y - origin_y, z - origin_z]
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "Legacy normals are deliberately f32; caller checks finite arithmetic"
)]
fn stored_normal(value: f64) -> f32 {
    value as f32
}

#[cfg(test)]
mod tests;
