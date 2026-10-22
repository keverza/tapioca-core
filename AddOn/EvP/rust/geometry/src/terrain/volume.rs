//! S3b.2 cut/fill volumes (G13) on a regular plan grid.
//!
//! **Grid.** Over the existing surface's bounds (or a given domain), cells of
//! `cell` metres start at the minimum corner; the last row and column are partial
//! when the extent is not a whole number of cells. The cell count is
//! `round(extent / cell)` when that is within `1e-12` (absolute plus relative) of
//! the ratio, otherwise its ceiling. Each cell is sampled at the centres of
//! `subdivisions x subdivisions` equal sub-cells (one: the cell centre), each
//! weighted by its own area. This matches the Python analysis oracle exactly for
//! one subdivision, including its sampling order (x outer, y inner).
//!
//! **Volume.** `diff = proposed - existing` at a sample, `volume = area * diff`;
//! positive is fill, negative cut, `net = fill - cut`. A sample off either
//! surface is excluded and counted.
//!
//! **Context.** With a property ring, samples strictly inside it are the site;
//! the rest (outside or on it) are reported separately as context, never
//! dropped, so the whole terrain is always accounted for.
//!
//! **Attribution.** When the proposed surface is the existing mesh with edited
//! heights ([`Proposed::Edited`]), a sample's difference is the barycentric blend
//! of its face's vertex displacements, and each vertex displacement belongs to
//! the edit that owns it. Per edit, `net` sums exactly to the total net; `cut`
//! and `fill` are the signed parts of each sample's contribution.

pub mod exact;

use super::field::Field;
use super::plan;

/// The proposed surface.
#[derive(Clone, Copy, Debug)]
pub enum Proposed<'a> {
    /// A different surface.
    Field(&'a Field),
    /// A horizontal plane at this height, metres.
    Plane(f64),
    /// The existing mesh with these heights, one per vertex, and per-vertex owners
    /// (the edit index that moved each vertex).
    Edited {
        /// New heights, metres.
        heights: &'a [f64],
        /// Owning edit per vertex.
        owners: &'a [Option<u32>],
    },
}

/// Grid resolution.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Grid {
    /// Cell size, metres, positive.
    pub cell: f64,
    /// Samples per cell along each axis, at least one.
    pub subdivisions: u32,
}

/// Borrowed cut/fill request.
#[derive(Clone, Copy, Debug)]
pub struct VolumeRequest<'a> {
    /// The existing surface.
    pub existing: &'a Field,
    /// The proposed surface.
    pub proposed: Proposed<'a>,
    /// Resolution.
    pub grid: Grid,
    /// `[x_min, y_min, x_max, y_max]`; the existing bounds when `None`.
    pub domain: Option<[f64; 4]>,
    /// Flat xyz property ring; outside it is context.
    pub property: Option<&'a [f64]>,
}

/// Volumes over a set of samples, cubic and square metres.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Volumes {
    /// Excavated volume.
    pub cut: f64,
    /// Placed volume.
    pub fill: f64,
    /// `fill - cut`.
    pub net: f64,
    /// Plan area sampled on both surfaces.
    pub area: f64,
    /// Samples taken on both surfaces.
    pub samples: u32,
    /// Samples off either surface.
    pub excluded: u32,
}

/// One edit's share of the volumes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EditVolumes {
    /// Edit index.
    pub edit: u32,
    /// Cut it caused.
    pub cut: f64,
    /// Fill it caused.
    pub fill: f64,
    /// Its net, `fill - cut`.
    pub net: f64,
    /// Plan area where it changed the surface.
    pub area: f64,
}

/// Site, context and per-edit volumes.
#[derive(Clone, Debug, PartialEq)]
pub struct VolumeReport {
    /// Inside the property ring, or everything without one.
    pub site: Volumes,
    /// Outside or on the property ring.
    pub context: Volumes,
    /// Per owning edit, ascending; empty unless [`Proposed::Edited`].
    pub edits: Vec<EditVolumes>,
    /// Cells along x and y.
    pub cells: [u32; 2],
}

/// Volume refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VolumeError {
    /// A cell is not finite and positive, subdivisions are zero, or the grid
    /// would exceed 2^32 samples.
    Grid,
    /// The domain is not finite with positive extent, or a height is not finite.
    Domain,
    /// Edited heights or owners do not have one entry per existing vertex.
    Shape,
    /// The property ring has fewer than three vertices or a non-finite one.
    Property,
}

impl VolumeError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Grid => {
                "VolumeGrid: cell must be finite and positive with 1+ subdivisions within 2^32 samples"
            }
            Self::Domain => {
                "VolumeDomain: the domain needs finite positive extent and finite heights"
            }
            Self::Shape => {
                "VolumeShape: edited heights and owners need one entry per existing vertex"
            }
            Self::Property => {
                "VolumeProperty: the property ring needs three or more finite vertices"
            }
        }
    }
}

/// The oracle's cell count: the nearest integer when the ratio is within 1e-12
/// of it (absolute plus relative), otherwise the ceiling.
fn cell_count(extent: f64, cell: f64) -> Result<u32, VolumeError> {
    let ratio = extent / cell;
    let nearest = ratio.round();
    let count = if (ratio - nearest).abs() <= 1e-12 + 1e-12 * nearest.abs() {
        nearest
    } else {
        ratio.ceil()
    };
    if !(count >= 1.0 && count <= f64::from(u32::MAX)) {
        return Err(VolumeError::Grid);
    }
    Ok(plan::clamp_index(count, u32::MAX))
}

/// A sample's difference and, for edited surfaces, its per-owner parts.
fn difference(
    request: &VolumeRequest<'_>,
    x: f64,
    y: f64,
    parts: &mut Vec<(u32, f64)>,
) -> Option<f64> {
    parts.clear();
    let existing = request.existing.height_at(x, y)?;
    match request.proposed {
        Proposed::Field(field) => field
            .height_at(x, y)
            .map(|sample| sample.height - existing.height),
        Proposed::Plane(z) => Some(z - existing.height),
        Proposed::Edited { heights, owners } => {
            let corners = request.existing.corners(x, y)?;
            let mut total = 0.0;
            for (&corner, weight) in corners.iter().zip(existing.weights) {
                let index = corner as usize;
                let (new, old) = (heights.get(index)?, request.existing.vertex_height(index)?);
                let part = weight * (new - old);
                total += part;
                if let Some(Some(edit)) = owners.get(index)
                    && part != 0.0
                {
                    parts.push((*edit, part));
                }
            }
            // One part per edit: two corners owned by one edit are one share.
            parts.sort_by_key(|&(edit, _)| edit);
            parts.dedup_by(|later, earlier| {
                if later.0 == earlier.0 {
                    earlier.1 += later.1;
                    true
                } else {
                    false
                }
            });
            Some(total)
        }
    }
}

fn accumulate(volumes: &mut Volumes, area: f64, diff: f64) {
    let volume = area * diff;
    if volume < 0.0 {
        volumes.cut += -volume;
    } else {
        volumes.fill += volume;
    }
    volumes.area += area;
    volumes.samples += 1;
}

/// Cut/fill volumes between two surfaces on a regular grid.
///
/// O(samples x face lookup); allocates per edit. Worker-classified.
///
/// # Errors
/// See [`VolumeError`].
pub fn cut_fill(request: &VolumeRequest<'_>) -> Result<VolumeReport, VolumeError> {
    let Grid { cell, subdivisions } = request.grid;
    if !(cell.is_finite() && cell > 0.0) || subdivisions == 0 {
        return Err(VolumeError::Grid);
    }
    if let Proposed::Plane(z) = request.proposed
        && !z.is_finite()
    {
        return Err(VolumeError::Domain);
    }
    if let Proposed::Edited { heights, owners } = request.proposed {
        let count = request.existing.vertex_count();
        if heights.len() != count || owners.len() != count {
            return Err(VolumeError::Shape);
        }
        if heights.iter().any(|z| !z.is_finite()) {
            return Err(VolumeError::Domain);
        }
    }
    let property = match request.property {
        Some(ring) => {
            if !ring.len().is_multiple_of(3) || ring.iter().any(|v| !v.is_finite()) {
                return Err(VolumeError::Property);
            }
            let vertices = plan::vertices(ring);
            if vertices.len() < 3 {
                return Err(VolumeError::Property);
            }
            Some(vertices)
        }
        None => None,
    };
    let bounds = request.existing.bounds();
    let [x_min, y_min, x_max, y_max] =
        request
            .domain
            .unwrap_or([bounds.min[0], bounds.min[1], bounds.max[0], bounds.max[1]]);
    if [x_min, y_min, x_max, y_max].iter().any(|v| !v.is_finite())
        || x_max <= x_min
        || y_max <= y_min
    {
        return Err(VolumeError::Domain);
    }
    let columns = cell_count(x_max - x_min, cell)?;
    let rows = cell_count(y_max - y_min, cell)?;
    let total =
        u64::from(columns) * u64::from(rows) * u64::from(subdivisions) * u64::from(subdivisions);
    if total > u64::from(u32::MAX) {
        return Err(VolumeError::Grid);
    }
    let mut site = Volumes::default();
    let mut context = Volumes::default();
    let mut per_edit: Vec<EditVolumes> = Vec::new();
    let mut parts = Vec::new();
    let steps = f64::from(subdivisions);
    for column in 0..columns {
        let x0 = x_min + f64::from(column) * cell;
        let x1 = (x0 + cell).min(x_max);
        for row in 0..rows {
            let y0 = y_min + f64::from(row) * cell;
            let y1 = (y0 + cell).min(y_max);
            let (width, height) = ((x1 - x0) / steps, (y1 - y0) / steps);
            for i in 0..subdivisions {
                let x = if subdivisions == 1 {
                    f64::midpoint(x0, x1)
                } else {
                    x0 + (f64::from(i) + 0.5) * width
                };
                for j in 0..subdivisions {
                    let y = if subdivisions == 1 {
                        f64::midpoint(y0, y1)
                    } else {
                        y0 + (f64::from(j) + 0.5) * height
                    };
                    let inside = property.is_none_or(|ring| plan::inside(ring, x, y) == Some(true));
                    let bucket = if inside { &mut site } else { &mut context };
                    let Some(diff) = difference(request, x, y, &mut parts) else {
                        bucket.excluded += 1;
                        continue;
                    };
                    let area = width * height;
                    accumulate(bucket, area, diff);
                    for &(edit, part) in &parts {
                        let position = match per_edit.binary_search_by_key(&edit, |row| row.edit) {
                            Ok(position) => position,
                            Err(position) => {
                                per_edit.insert(
                                    position,
                                    EditVolumes {
                                        edit,
                                        ..EditVolumes::default()
                                    },
                                );
                                position
                            }
                        };
                        if let Some(row) = per_edit.get_mut(position) {
                            let volume = area * part;
                            if volume < 0.0 {
                                row.cut += -volume;
                            } else {
                                row.fill += volume;
                            }
                            row.net += volume;
                            row.area += area;
                        }
                    }
                }
            }
        }
    }
    for volumes in [&mut site, &mut context] {
        volumes.net = volumes.fill - volumes.cut;
    }
    Ok(VolumeReport {
        site,
        context,
        edits: per_edit,
        cells: [columns, rows],
    })
}

#[cfg(test)]
mod tests;
