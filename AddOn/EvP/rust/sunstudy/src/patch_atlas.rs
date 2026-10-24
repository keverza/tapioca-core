//! One atlas tile per surface, with an address that survives an edit.
//!
//! Port of `SunStudy/SunStudyPatchAtlas.cpp` and
//! `SunStudy/SunStudyAtlasReuse.cpp`. Allocations are keyed by [`PatchKey`] and
//! kept: an untouched surface's tile does not move when another changes. It
//! never compacts (that would move tiles still being drawn); it rebuilds only
//! when it has to grow, and says so through `resized`. Generations never reset,
//! so a stale rectangle can never match a new one.

use crate::atlas::{
    AtlasOptions, AtlasSamples, AtlasTile, SunStudyAtlas, build_sun_study_atlas, next_power_of_two,
    starting_side, tile_texel,
};
use crate::patch_sampler::{PatchKey, PatchSampleGrid};
use crate::roles::canonical_guid;
use std::collections::{BTreeMap, BTreeSet};

/// One surface's rectangle and when it last moved.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PatchAtlasAllocation {
    /// Identity.
    pub key: PatchKey,
    /// Left texel.
    pub x: u32,
    /// Top texel.
    pub y: u32,
    /// Lattice columns (0 = refused).
    pub width: u32,
    /// Lattice rows (0 = refused).
    pub height: u32,
    /// Bumped whenever the rectangle changes.
    pub generation: u32,
}

impl PatchAtlasAllocation {
    /// Whether it has a rectangle.
    #[must_use]
    pub fn placed(&self) -> bool {
        self.width > 0 && self.height > 0
    }
}

/// What one fit did: counts that tell an incremental update from a rebuild.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchAtlasUpdate {
    /// Same key, same size: did not move.
    pub retained: usize,
    /// Dirty, same size (counted by callers that track dirtiness).
    pub reused: usize,
    /// Same key, new size: a new rectangle.
    pub moved: usize,
    /// New keys (or every key after a rebuild).
    pub added: usize,
    /// Keys no longer wanted.
    pub removed: usize,
    /// The atlas grew and every rectangle moved.
    pub resized: bool,
}

/// The allocator's scalar state, for transport across the C ABI.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchAtlasCursor {
    /// Texels across.
    pub width: u32,
    /// Texels down.
    pub height: u32,
    /// Shelf cursor x (only moves forward).
    pub pen_x: u32,
    /// Shelf cursor y.
    pub pen_y: u32,
    /// Current shelf height.
    pub shelf_height: u32,
    /// Next generation to issue.
    pub next_generation: u32,
}

/// One rectangle a fit must place.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PatchShape {
    /// Identity.
    pub key: PatchKey,
    /// Lattice columns.
    pub columns: u32,
    /// Lattice rows.
    pub rows: u32,
}

impl From<&crate::patch_sampler::PatchSampleSpan> for PatchShape {
    fn from(span: &crate::patch_sampler::PatchSampleSpan) -> Self {
        Self {
            key: span.key.clone(),
            columns: span.columns,
            rows: span.rows,
        }
    }
}

/// The keep-allocations patch atlas.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PatchAtlas {
    allocations: BTreeMap<PatchKey, PatchAtlasAllocation>,
    cursor: PatchAtlasCursor,
}

impl Default for PatchAtlas {
    fn default() -> Self {
        Self {
            allocations: BTreeMap::new(),
            cursor: PatchAtlasCursor {
                next_generation: 1,
                ..PatchAtlasCursor::default()
            },
        }
    }
}

impl PatchAtlas {
    /// Rebuild an atlas from transported state.
    #[must_use]
    pub fn from_state(cursor: PatchAtlasCursor, allocations: Vec<PatchAtlasAllocation>) -> Self {
        Self {
            allocations: allocations
                .into_iter()
                .map(|a| (a.key.clone(), a))
                .collect(),
            cursor,
        }
    }

    /// The scalar state.
    #[must_use]
    pub fn cursor(&self) -> PatchAtlasCursor {
        self.cursor
    }

    /// Allocations in key order.
    pub fn allocations(&self) -> impl Iterator<Item = &PatchAtlasAllocation> {
        self.allocations.values()
    }

    /// Texels across.
    #[must_use]
    pub fn width(&self) -> u32 {
        self.cursor.width
    }

    /// Texels down.
    #[must_use]
    pub fn height(&self) -> u32 {
        self.cursor.height
    }

    /// `width * height`.
    #[must_use]
    pub fn texel_count(&self) -> usize {
        usize::try_from(u64::from(self.cursor.width) * u64::from(self.cursor.height))
            .unwrap_or(usize::MAX)
    }

    /// The allocation for `key`.
    #[must_use]
    pub fn find(&self, key: &PatchKey) -> Option<&PatchAtlasAllocation> {
        self.allocations.get(key)
    }

    /// Drop everything; the generation keeps counting.
    pub fn clear(&mut self) {
        self.allocations.clear();
        let next = self.cursor.next_generation.wrapping_add(1);
        self.cursor = PatchAtlasCursor {
            next_generation: next,
            ..PatchAtlasCursor::default()
        };
    }

    fn issue_generation(&mut self) -> u32 {
        let generation = self.cursor.next_generation;
        self.cursor.next_generation = generation.wrapping_add(1);
        generation
    }

    fn place(&mut self, width: u32, height: u32, gutter: u32) -> Option<(u32, u32)> {
        let c = &mut self.cursor;
        let (need_w, need_h) = (width.wrapping_add(gutter), height.wrapping_add(gutter));
        if c.pen_x.wrapping_add(need_w) > c.width {
            c.pen_x = gutter;
            c.pen_y = c.pen_y.wrapping_add(c.shelf_height);
            c.shelf_height = 0;
        }
        if c.pen_y.wrapping_add(need_h) > c.height {
            return None;
        }
        let at = (c.pen_x, c.pen_y);
        c.pen_x = c.pen_x.wrapping_add(need_w);
        c.shelf_height = c.shelf_height.max(need_h);
        Some(at)
    }

    /// A fresh allocation and whether `place` found room (a refused one has
    /// zero size, as in C++).
    fn allocate(&mut self, shape: &PatchShape, gutter: u32) -> (PatchAtlasAllocation, bool) {
        let generation = self.issue_generation();
        let mut allocation = PatchAtlasAllocation {
            key: shape.key.clone(),
            x: 0,
            y: 0,
            width: shape.columns,
            height: shape.rows,
            generation,
        };
        let room = self.place(shape.columns, shape.rows, gutter);
        if let Some((x, y)) = room {
            (allocation.x, allocation.y) = (x, y);
        } else {
            (allocation.width, allocation.height) = (0, 0);
        }
        (allocation, room.is_some())
    }

    /// Repack everything tallest first (stable on ties) into a `side` square.
    fn rebuild(&mut self, shapes: &[PatchShape], gutter: u32, side: u32) -> bool {
        self.allocations.clear();
        self.cursor.width = side;
        self.cursor.height = side;
        self.cursor.pen_x = gutter;
        self.cursor.pen_y = gutter;
        self.cursor.shelf_height = 0;
        let mut order: Vec<&PatchShape> = shapes.iter().collect();
        order.sort_by(|a, b| b.rows.cmp(&a.rows).then(b.columns.cmp(&a.columns)));
        for shape in order {
            let (allocation, _) = self.allocate(shape, gutter);
            self.allocations.insert(shape.key.clone(), allocation);
        }
        self.allocations.values().all(PatchAtlasAllocation::placed)
    }

    fn grown(&self) -> PatchAtlasUpdate {
        PatchAtlasUpdate {
            added: self.allocations.len(),
            resized: true,
            ..PatchAtlasUpdate::default()
        }
    }

    /// Fit `shapes`, keeping every allocation whose key and size are
    /// unchanged. Idempotent on an unchanged set. A refusal clears the atlas.
    pub fn fit(
        &mut self,
        shapes: &[PatchShape],
        gutter: u32,
        max_dimension: u32,
    ) -> PatchAtlasUpdate {
        let mut update = PatchAtlasUpdate::default();
        let gutter = gutter.max(1);
        let wanted: BTreeSet<&PatchKey> = shapes.iter().map(|s| &s.key).collect();
        let before = self.allocations.len();
        self.allocations.retain(|key, _| wanted.contains(key));
        update.removed = before - self.allocations.len();

        if self.cursor.width == 0 {
            return self.first_fit(shapes, gutter, max_dimension, update);
        }

        for shape in shapes {
            let existing = self
                .allocations
                .get(&shape.key)
                .map(|a| a.width == shape.columns && a.height == shape.rows);
            if existing == Some(true) {
                update.retained += 1;
                continue;
            }
            let (allocation, room) = self.allocate(shape, gutter);
            if !room {
                return self.grow(shapes, gutter, max_dimension);
            }
            if existing.is_some() {
                update.moved += 1;
            } else {
                update.added += 1;
            }
            self.allocations.insert(shape.key.clone(), allocation);
        }
        update
    }

    /// An empty atlas is sized from the total area (and the widest shape),
    /// then doubled until everything fits or the maximum refuses.
    fn first_fit(
        &mut self,
        shapes: &[PatchShape],
        gutter: u32,
        max_dimension: u32,
        update: PatchAtlasUpdate,
    ) -> PatchAtlasUpdate {
        let mut area = 0_u64;
        let mut widest = 1_u32;
        for s in shapes {
            area +=
                u64::from(s.columns.wrapping_add(gutter)) * u64::from(s.rows.wrapping_add(gutter));
            widest = widest.max(s.columns.max(s.rows).wrapping_add(gutter.wrapping_mul(2)));
        }
        let mut side = starting_side(area).max(next_power_of_two(widest));
        while side <= max_dimension && side != 0 {
            if self.rebuild(shapes, gutter, side) {
                return self.grown();
            }
            side <<= 1;
        }
        self.clear();
        update
    }

    /// Out of room without compacting: grow and repack everything.
    fn grow(&mut self, shapes: &[PatchShape], gutter: u32, max_dimension: u32) -> PatchAtlasUpdate {
        let mut side = self.cursor.width;
        while side <= max_dimension {
            side = side.wrapping_shl(1);
            if side > max_dimension || side == 0 {
                break;
            }
            if self.rebuild(shapes, gutter, side) {
                return self.grown();
            }
        }
        self.clear();
        PatchAtlasUpdate::default()
    }

    /// Texel of one sample, or `None` when its patch is not placed.
    #[must_use]
    pub fn texel_of(&self, grid: &PatchSampleGrid, sample: usize) -> Option<u64> {
        let span = *grid.span_of.get(sample)?;
        let span = grid.spans.get(usize::try_from(span).ok()?)?;
        let allocation = self.find(&span.key).filter(|a| a.placed())?;
        let column = (*grid.cell_columns.get(sample)?).min(allocation.width - 1);
        let row = (*grid.cell_rows.get(sample)?).min(allocation.height - 1);
        Some(
            u64::from(allocation.y.wrapping_add(row)) * u64::from(self.cursor.width)
                + u64::from(allocation.x.wrapping_add(column)),
        )
    }

    /// Scatter one patch's values into `image` (`texel_count` long).
    #[must_use]
    pub fn scatter_patch(
        &self,
        grid: &PatchSampleGrid,
        span_index: usize,
        hours: &[f64],
        image: &mut [f32],
    ) -> bool {
        let Some(span) = grid.spans.get(span_index) else {
            return false;
        };
        if image.len() != self.texel_count()
            || hours.len() != span.count
            || !self
                .find(&span.key)
                .is_some_and(PatchAtlasAllocation::placed)
        {
            return false;
        }
        for (i, value) in hours.iter().enumerate() {
            let texel = self
                .texel_of(grid, span.first + i)
                .and_then(|t| usize::try_from(t).ok());
            if let Some(slot) = texel.and_then(|t| image.get_mut(t)) {
                *slot = crate::atlas::f64_to_f32(*value);
            }
        }
        true
    }

    /// The texels an allocation covers, row-major, for a partial upload.
    #[must_use]
    pub fn read_rectangle(
        &self,
        allocation: &PatchAtlasAllocation,
        image: &[f32],
    ) -> Option<Vec<f32>> {
        if !allocation.placed() || image.len() != self.texel_count() {
            return None;
        }
        let width = usize::try_from(allocation.width).ok()?;
        let height = usize::try_from(allocation.height).ok()?;
        let atlas_width = usize::try_from(self.cursor.width).ok()?;
        let (x, y) = (
            usize::try_from(allocation.x).ok()?,
            usize::try_from(allocation.y).ok()?,
        );
        let mut out = vec![-1.0; width * height];
        for (index, slot) in out.iter_mut().enumerate() {
            let (row, column) = (index / width, index % width);
            if let Some(value) = image.get((y + row) * atlas_width + x + column) {
                *slot = *value;
            }
        }
        Some(out)
    }
}

/// One snapshot mesh, for stable triangle identities.
#[derive(Clone, Copy, Debug)]
pub struct AtlasMesh<'a> {
    /// The element GUID (any spelling; canonicalised here).
    pub guid: &'a str,
    /// Triangles in the mesh, in snapshot face order.
    pub triangle_count: usize,
}

/// A triangle atlas whose rectangles are keyed by (canonical element GUID,
/// local triangle index), so an unchanged element keeps its tiles. Ambiguous
/// identities (empty or duplicate GUIDs, or meshes that do not partition the
/// layouts) clear `allocations` and fall back to a fresh pack. `previous`
/// seeds the allocations when it fits within the maximum.
#[must_use]
pub fn build_stable_triangle_atlas(
    samples: &AtlasSamples<'_>,
    meshes: &[AtlasMesh<'_>],
    allocations: &mut PatchAtlas,
    previous: Option<&PatchAtlas>,
    options: &AtlasOptions,
) -> SunStudyAtlas {
    let mut shapes = Vec::new();
    let mut faces = Vec::new();
    let mut identities = BTreeSet::new();
    let mut face = 0_usize;
    for mesh in meshes {
        let guid = canonical_guid(mesh.guid);
        if guid.is_empty()
            || !identities.insert(guid.clone())
            || face + mesh.triangle_count > samples.layouts.len()
        {
            allocations.clear();
            return build_sun_study_atlas(samples, options);
        }
        for local in 0..mesh.triangle_count {
            if let Some(layout) = samples
                .layouts
                .get(face)
                .filter(|l| l.gridded && l.columns != 0 && l.rows != 0)
            {
                shapes.push(PatchShape {
                    key: PatchKey {
                        element: guid.clone(),
                        plane: u64::try_from(local).unwrap_or(u64::MAX),
                        occurrence: 0,
                    },
                    columns: layout.columns,
                    rows: layout.rows,
                });
                faces.push(face);
            }
            face += 1;
        }
    }
    if face != samples.layouts.len() {
        allocations.clear();
        return build_sun_study_atlas(samples, options);
    }
    if let Some(previous) = previous.filter(|p| p.width() <= options.max_dimension) {
        allocations.clone_from(previous);
    }
    allocations.fit(&shapes, options.gutter, options.max_dimension);
    let mut atlas = SunStudyAtlas {
        width: allocations.width(),
        height: allocations.height(),
        tiles: vec![AtlasTile::default(); samples.layouts.len()],
        ..SunStudyAtlas::default()
    };
    if atlas.width == 0 && !faces.is_empty() {
        return atlas;
    }
    for (shape, face) in shapes.iter().zip(&faces) {
        let Some(placed) = allocations.find(&shape.key).filter(|a| a.placed()) else {
            return SunStudyAtlas::default();
        };
        if let Some(tile) = atlas.tiles.get_mut(*face) {
            *tile = AtlasTile {
                x: placed.x,
                y: placed.y,
                width: placed.width,
                height: placed.height,
            };
        }
    }
    atlas.placed_faces = faces.len();
    let count = samples.faces.len();
    if samples.cell_columns.len() != count || samples.cell_rows.len() != count {
        return SunStudyAtlas::default();
    }
    atlas.texels = vec![0; count];
    for (i, slot) in atlas.texels.iter_mut().enumerate() {
        let tile = samples
            .faces
            .get(i)
            .and_then(|f| usize::try_from(*f).ok())
            .and_then(|f| atlas.tiles.get(f));
        let Some(tile) = tile else {
            return SunStudyAtlas::default();
        };
        if let (true, Some(column), Some(row)) = (
            tile.placed(),
            samples.cell_columns.get(i),
            samples.cell_rows.get(i),
        ) {
            *slot = tile_texel(tile, atlas.width, *column, *row);
        }
    }
    atlas.valid = true;
    atlas
}

#[cfg(test)]
mod tests;
