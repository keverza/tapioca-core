//! The study's result as a texture: per-triangle shelf packing, the sample to
//! texel map, the scatter and the point-to-UV map.
//!
//! Port of `SunStudy/SunStudyAtlas.cpp`. The packer is a deterministic shelf:
//! faces tallest first (ties by width, then face index), laid in rows, in the
//! smallest power-of-two square that fits. Refused, never truncated: an atlas
//! missing faces would draw holes while every total looked right.

use crate::raster::cpp_max;
use crate::sampler::FaceLayout;

/// Where one face lives in the atlas; `width == 0` means not placed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AtlasTile {
    /// Left texel.
    pub x: u32,
    /// Top texel.
    pub y: u32,
    /// Texels across (the face's lattice columns).
    pub width: u32,
    /// Texels down (the face's lattice rows).
    pub height: u32,
}

impl AtlasTile {
    /// Whether the face has a rectangle.
    #[must_use]
    pub fn placed(&self) -> bool {
        self.width > 0 && self.height > 0
    }
}

/// A packed triangle atlas.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SunStudyAtlas {
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// One per source face.
    pub tiles: Vec<AtlasTile>,
    /// Texel index per sample (`row * width + column`).
    pub texels: Vec<u32>,
    /// Faces with a rectangle.
    pub placed_faces: usize,
    /// False when refused.
    pub valid: bool,
}

impl SunStudyAtlas {
    /// `width * height`.
    #[must_use]
    pub fn texel_count(&self) -> usize {
        usize::try_from(u64::from(self.width) * u64::from(self.height)).unwrap_or(usize::MAX)
    }
}

/// Packing settings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AtlasOptions {
    /// Empty texels around every tile; at least 1 (bilinear bleed otherwise).
    pub gutter: u32,
    /// Ceiling per axis.
    pub max_dimension: u32,
}

impl Default for AtlasOptions {
    fn default() -> Self {
        Self {
            gutter: 1,
            max_dimension: 8192,
        }
    }
}

/// The per-sample arrays the atlas reads from a sample grid.
#[derive(Clone, Copy, Debug)]
pub struct AtlasSamples<'a> {
    /// One per source face.
    pub layouts: &'a [FaceLayout],
    /// Source face per sample.
    pub faces: &'a [u32],
    /// Cell column per sample.
    pub cell_columns: &'a [u32],
    /// Cell row per sample.
    pub cell_rows: &'a [u32],
}

/// Smallest power of two `>= value`, capped at 2^30 (as the C++ loop).
pub(crate) fn next_power_of_two(value: u32) -> u32 {
    let mut result = 1_u32;
    while result < value && result < (1 << 30) {
        result <<= 1;
    }
    result
}

/// The starting side for `area` texels: sqrt * 1.15, ceiled, as a power of two.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    clippy::cast_precision_loss,
    reason = "mirrors static_cast<uint32_t>(ceil(sqrt(double(area)) * 1.15))"
)]
pub(crate) fn starting_side(area: u64) -> u32 {
    next_power_of_two(((area as f64).sqrt() * 1.15).ceil() as u32)
}

#[derive(Clone, Copy)]
struct Pending {
    face: u32,
    width: u32,
    height: u32,
}

fn shelf_pack(pending: &[Pending], tiles: &mut [AtlasTile], gutter: u32, side: u32) -> bool {
    tiles.fill(AtlasTile::default());
    let (mut pen_x, mut pen_y, mut shelf) = (gutter, gutter, 0_u32);
    for item in pending {
        if pen_x.wrapping_add(item.width) > side {
            pen_x = gutter;
            pen_y = pen_y.wrapping_add(shelf);
            shelf = 0;
        }
        if pen_y.wrapping_add(item.height) > side {
            return false;
        }
        if let Some(tile) = usize::try_from(item.face)
            .ok()
            .and_then(|f| tiles.get_mut(f))
        {
            *tile = AtlasTile {
                x: pen_x,
                y: pen_y,
                width: item.width - gutter,
                height: item.height - gutter,
            };
        }
        pen_x = pen_x.wrapping_add(item.width);
        shelf = shelf.max(item.height);
    }
    true
}

/// Texel of one sample inside a placed tile, clamped to the tile.
pub(crate) fn tile_texel(tile: &AtlasTile, atlas_width: u32, column: u32, row: u32) -> u32 {
    let column = column.min(tile.width - 1);
    let row = row.min(tile.height - 1);
    tile.y
        .wrapping_add(row)
        .wrapping_mul(atlas_width)
        .wrapping_add(tile.x.wrapping_add(column))
}

/// Pack the sampler's per-face layouts into one atlas. No layouts means the
/// sampler was not asked for them: an invalid, empty atlas. Nothing gridded
/// is a valid 0x0 atlas.
#[must_use]
pub fn build_sun_study_atlas(samples: &AtlasSamples<'_>, options: &AtlasOptions) -> SunStudyAtlas {
    let mut atlas = SunStudyAtlas::default();
    if samples.layouts.is_empty() {
        return atlas;
    }
    let gutter = options.gutter.max(1);
    let mut area = 0_u64;
    let mut pending = Vec::with_capacity(samples.layouts.len());
    for (face, layout) in samples.layouts.iter().enumerate() {
        if !layout.gridded || layout.columns == 0 || layout.rows == 0 {
            continue;
        }
        let item = Pending {
            face: u32::try_from(face).unwrap_or(u32::MAX),
            width: layout.columns.wrapping_add(gutter),
            height: layout.rows.wrapping_add(gutter),
        };
        area += u64::from(item.width) * u64::from(item.height);
        pending.push(item);
    }
    atlas.tiles = vec![AtlasTile::default(); samples.layouts.len()];
    if pending.is_empty() {
        atlas.valid = true;
        return atlas;
    }
    let mut side = starting_side(area);
    for item in &pending {
        side = side.max(next_power_of_two(
            item.width.max(item.height).wrapping_add(gutter),
        ));
    }
    pending.sort_by(|a, b| {
        b.height
            .cmp(&a.height)
            .then(b.width.cmp(&a.width))
            .then(a.face.cmp(&b.face))
    });
    while side <= options.max_dimension {
        if shelf_pack(&pending, &mut atlas.tiles, gutter, side) {
            atlas.width = side;
            atlas.height = side;
            break;
        }
        side <<= 1;
        if side == 0 {
            break;
        }
    }
    if atlas.width == 0 {
        atlas.tiles.fill(AtlasTile::default());
        return atlas;
    }
    atlas.placed_faces = atlas.tiles.iter().filter(|t| t.placed()).count();
    let count = samples.faces.len();
    atlas.texels = vec![0; count];
    let have_cells = samples.cell_columns.len() == count && samples.cell_rows.len() == count;
    for (i, slot) in atlas.texels.iter_mut().enumerate() {
        let tile = samples
            .faces
            .get(i)
            .and_then(|f| usize::try_from(*f).ok())
            .and_then(|f| atlas.tiles.get(f))
            .filter(|t| have_cells && t.placed());
        if let (Some(tile), Some(column), Some(row)) =
            (tile, samples.cell_columns.get(i), samples.cell_rows.get(i))
        {
            *slot = tile_texel(tile, atlas.width, *column, *row);
        }
    }
    atlas.valid = true;
    atlas
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "mirrors static_cast<float>(double) for display values"
)]
pub(crate) fn f64_to_f32(value: f64) -> f32 {
    value as f32
}

/// Scatter per-sample values into a texel image; untouched texels hold
/// `empty_value` (use a negative sentinel: 0 means "never sunlit").
#[must_use]
pub fn scatter_to_atlas(atlas: &SunStudyAtlas, per_sample: &[f64], empty_value: f32) -> Vec<f32> {
    if !atlas.valid || atlas.texel_count() == 0 {
        return Vec::new();
    }
    let mut image = vec![empty_value; atlas.texel_count()];
    for (texel, value) in atlas.texels.iter().zip(per_sample) {
        if let Some(slot) = usize::try_from(*texel).ok().and_then(|t| image.get_mut(t)) {
            *slot = f64_to_f32(*value);
        }
    }
    image
}

#[expect(
    clippy::cast_precision_loss,
    reason = "mirrors static_cast<double>(long) for a cell origin"
)]
fn cell_origin(value: i64) -> f64 {
    value as f64
}

/// Texture coordinates in `[0, 1]` of a point on a placed, gridded face,
/// clamped (never wrapped) to the face's tile.
#[must_use]
pub fn atlas_uv(
    atlas: &SunStudyAtlas,
    layouts: &[FaceLayout],
    face: u32,
    point: &[f64; 3],
    spacing: f64,
) -> Option<[f32; 2]> {
    if !atlas.valid || spacing.partial_cmp(&0.0) != Some(std::cmp::Ordering::Greater) {
        return None;
    }
    let index = usize::try_from(face).ok()?;
    let tile = atlas.tiles.get(index)?;
    let layout = layouts.get(index)?;
    if !tile.placed() || !layout.gridded {
        return None;
    }
    let relative = [
        point[0] - layout.origin[0],
        point[1] - layout.origin[1],
        point[2] - layout.origin[2],
    ];
    let along =
        |axis: &[f64; 3]| relative[0] * axis[0] + relative[1] * axis[1] + relative[2] * axis[2];
    let cell_u = along(&layout.u_axis) / spacing - cell_origin(layout.u_start);
    let cell_v = along(&layout.v_axis) / spacing - cell_origin(layout.v_start);
    let clamp = |value: f64, limit: u32| {
        let low = cpp_max(value, 0.0);
        let high = f64::from(limit);
        if high < low { high } else { low }
    };
    Some([
        f64_to_f32((f64::from(tile.x) + clamp(cell_u, tile.width)) / f64::from(atlas.width)),
        f64_to_f32((f64::from(tile.y) + clamp(cell_v, tile.height)) / f64::from(atlas.height)),
    ])
}

#[cfg(test)]
mod tests;
