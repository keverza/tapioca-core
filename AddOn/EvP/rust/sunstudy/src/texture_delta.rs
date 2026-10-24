//! Exact row-run deltas between two atlas images, for partial GPU uploads.
//!
//! Port of `SunStudy/SunStudyTextureDelta.hpp`. Changed texels (compared by
//! bytes, so retired sentinels and NaN payloads count) form runs per row;
//! a run extends the region directly above only when its x and width match.
//! A layer that fragments past `max_regions_per_layer` collapses to one
//! full-plane region. Incompatible inputs return nothing: the caller must
//! do a complete upload.

/// One rectangle of one layer.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AtlasRegion {
    /// Left texel.
    pub x: u32,
    /// Top texel.
    pub y: u32,
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// Array layer.
    pub layer: u32,
}

/// Texel types compared by their bytes, as the C++ `memcmp`.
pub trait TexelBits: Copy {
    /// The value's bit pattern.
    fn bits(self) -> u64;
}

impl TexelBits for f32 {
    fn bits(self) -> u64 {
        u64::from(self.to_bits())
    }
}

impl TexelBits for u32 {
    fn bits(self) -> u64 {
        u64::from(self)
    }
}

/// Whether every region is non-empty and inside a `width x height x layers`
/// image.
#[must_use]
pub fn atlas_regions_valid(regions: &[AtlasRegion], width: u32, height: u32, layers: u32) -> bool {
    regions.iter().all(|r| {
        r.width != 0
            && r.height != 0
            && r.x < width
            && r.y < height
            && r.width <= width - r.x
            && r.height <= height - r.y
            && r.layer < layers
    })
}

/// Maximal runs of changed texels in one row, as `(start, width)`.
fn row_runs(width: u32, changed: impl Fn(u32) -> bool) -> Vec<(u32, u32)> {
    let mut runs = Vec::new();
    let mut x = 0;
    while x < width {
        if !changed(x) {
            x += 1;
            continue;
        }
        let start = x;
        x += 1;
        while x < width && changed(x) {
            x += 1;
        }
        runs.push((start, x - start));
    }
    runs
}

/// Extend the region above when its x and width match a run (the first such
/// region in row order), else start a new one; returns the region's index.
fn merge_run(
    regions: &mut Vec<AtlasRegion>,
    previous: &[usize],
    (start, run): (u32, u32),
    (y, layer): (u32, u32),
) -> usize {
    let extend = previous.iter().copied().find(|&index| {
        regions
            .get(index)
            .is_some_and(|r| r.x == start && r.width == run)
    });
    if let Some(index) = extend {
        if let Some(region) = regions.get_mut(index) {
            region.height += 1;
        }
        return index;
    }
    regions.push(AtlasRegion {
        x: start,
        y,
        width: run,
        height: 1,
        layer,
    });
    regions.len() - 1
}

/// The changed regions between `before` and `after`. Empty when the shape is
/// inconsistent (a full upload is then required) or nothing changed.
#[must_use]
pub fn atlas_changed_regions<T: TexelBits>(
    size: (u32, u32, u32),
    before: &[T],
    after: &[T],
    max_regions_per_layer: usize,
) -> Vec<AtlasRegion> {
    let (width, height, layers) = size;
    let (Ok(columns), Ok(rows), Ok(slices)) = (
        usize::try_from(width),
        usize::try_from(height),
        usize::try_from(layers),
    ) else {
        return Vec::new();
    };
    let plane = columns.checked_mul(rows);
    let total = plane.and_then(|p| p.checked_mul(slices));
    let (Some(plane), Some(total)) = (plane, total) else {
        return Vec::new();
    };
    if columns == 0
        || rows == 0
        || slices == 0
        || before.len() != total
        || before.len() != after.len()
    {
        return Vec::new();
    }
    let mut regions: Vec<AtlasRegion> = Vec::new();
    for layer in 0..layers {
        let first_region = regions.len();
        let mut previous: Vec<usize> = Vec::new();
        let layer_base = usize::try_from(layer).unwrap_or(0) * plane;
        for y in 0..height {
            let row = layer_base + usize::try_from(y).unwrap_or(0) * columns;
            let changed = |column: u32| {
                let i = row + usize::try_from(column).unwrap_or(0);
                match (before.get(i), after.get(i)) {
                    (Some(a), Some(b)) => a.bits() != b.bits(),
                    _ => false,
                }
            };
            let current: Vec<usize> = row_runs(width, changed)
                .into_iter()
                .map(|(start, run)| merge_run(&mut regions, &previous, (start, run), (y, layer)))
                .collect();
            previous = current;
            if regions.len() - first_region > max_regions_per_layer {
                regions.truncate(first_region);
                regions.push(AtlasRegion {
                    x: 0,
                    y: 0,
                    width,
                    height,
                    layer,
                });
                break;
            }
        }
    }
    regions
}

#[cfg(test)]
mod tests;
