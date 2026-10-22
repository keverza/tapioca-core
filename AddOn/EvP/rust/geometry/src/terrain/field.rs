//! S3b.2 terrain field (G13): height, normal, gradient, slope and aspect sampled
//! from a terrain mesh's upward faces.
//!
//! **Faces.** Only upward faces count: a triangle whose unit normal has z above
//! [`UPWARD`] (the snapshot's terrain arrives as a closed solid; its sides and
//! bottom are not the surface). The threshold matches the Python analysis
//! oracle. Every sample names the original triangle index it came from.
//!
//! **Ties.** A point on a shared edge or vertex lies in several faces; the lowest
//! original triangle index wins, as in the oracle. Barycentric weights at or
//! above `-1e-12` count as inside. Heights are continuous across shared edges, so
//! the tie only decides the reported face.
//!
//! **Gradient.** Central differences of the height at `+-step` along x and y;
//! one-sided where one side leaves the surface; the face plane where both do.
//! Slope is `atan(|gradient|)` in radians; aspect is the compass bearing of the
//! downslope direction in radians clockwise from +y (north), `None` when flat.
//!
//! Lookup uses a uniform plan grid over face bounds built once per field; a
//! query tests one cell's faces in ascending index. Positions are world metres in
//! `f64`; nothing is narrowed.

use super::plan::{self, clamp_index};

/// Minimum z component of an upward face's unit normal.
pub const UPWARD: f64 = 0.001;
/// Barycentric weight at or above `-INSIDE` counts as inside.
const INSIDE: f64 = 1e-12;

/// Field refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FieldError {
    /// Points not whole xyz triples, triangles not whole triples, or a count
    /// beyond 32-bit indices.
    Shape,
    /// A coordinate is NaN or infinite.
    NonFinite,
    /// A triangle names a vertex that does not exist.
    Index,
    /// No triangle faces upward.
    Empty,
    /// A query step or raster cell is not finite and positive.
    Step,
}

impl FieldError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Shape => "FieldShape: whole xyz points and vertex triples required",
            Self::NonFinite => "FieldNonFinite: all coordinates must be finite",
            Self::Index => "FieldIndex: a triangle names a missing vertex",
            Self::Empty => "FieldEmpty: the mesh has no upward face",
            Self::Step => "FieldStep: steps and cells must be finite and positive",
        }
    }
}

/// A height sample and the original triangle it came from.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sample {
    /// Height in metres.
    pub height: f64,
    /// Original triangle index.
    pub face: u32,
    /// Barycentric weights of the face's three corners, in corner order.
    pub weights: [f64; 3],
}

/// How one gradient component was obtained.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GradientMethod {
    /// `(h(+step) - h(-step)) / 2 step`.
    Central,
    /// One side left the surface: `(h(+step) - h(0)) / step` or its mirror.
    OneSided,
    /// Both sides left the surface: the sampled face's plane.
    Face,
}

/// Height gradient `(dz/dx, dz/dy)` and how each component was taken.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Gradient {
    /// Rise per metre along x and along y.
    pub slope: [f64; 2],
    /// Method per component.
    pub method: [GradientMethod; 2],
}

/// Axis-aligned bounds of the upward faces' vertices, metres.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Bounds {
    /// Minimum x, y, z.
    pub min: [f64; 3],
    /// Maximum x, y, z.
    pub max: [f64; 3],
}

/// How much of the mesh the field covers.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Coverage {
    /// Plan area of the upward faces, square metres.
    pub area: f64,
    /// Upward faces.
    pub faces: u32,
    /// Triangles excluded as not upward (sides, bottom, degenerate in plan).
    pub excluded: u32,
}

/// A regular raster of node heights for bilinear interpolation.
#[derive(Clone, Debug, PartialEq)]
pub struct Raster {
    /// Lower-left node, metres.
    pub origin: [f64; 2],
    /// Node spacing, metres.
    pub cell: f64,
    /// Nodes along x and along y.
    pub nodes: [u32; 2],
    /// Row-major (y outer) node heights; `None` off the surface.
    pub heights: Vec<Option<f64>>,
}

impl Raster {
    /// Bilinear height at `(x, y)`; `None` outside the raster or where any of the
    /// four surrounding nodes is off the surface.
    #[must_use]
    pub fn bilinear(&self, x: f64, y: f64) -> Option<f64> {
        let [columns, rows] = self.nodes;
        let across = (x - self.origin[0]) / self.cell;
        let up = (y - self.origin[1]) / self.cell;
        let last = |count: u32| f64::from(count.saturating_sub(1));
        if !(across >= 0.0 && up >= 0.0 && across <= last(columns) && up <= last(rows)) {
            return None;
        }
        // Cell index, clamped so the last node line uses the last cell.
        let cell = |t: f64, count: u32| {
            let index = clamp_index(t, count.saturating_sub(2));
            (index, t - f64::from(index))
        };
        let ((column, fx), (row, fy)) = (cell(across, columns), cell(up, rows));
        let node = |east: u32, north: u32| {
            let index = (row + north) as usize * columns as usize + (column + east) as usize;
            self.heights.get(index).copied().flatten()
        };
        let (south_west, south_east) = (node(0, 0)?, node(1, 0)?);
        let (north_west, north_east) = (node(0, 1)?, node(1, 1)?);
        let south = south_west * (1.0 - fx) + south_east * fx;
        let north = north_west * (1.0 - fx) + north_east * fx;
        Some(south * (1.0 - fy) + north * fy)
    }
}

/// A plan segment with heights at both ends, metres.
pub(super) type Segment = [[f64; 3]; 2];

/// An upward face: corner vertices, corner positions and its original index.
#[derive(Clone, Copy, Debug)]
struct Face {
    corners: [u32; 3],
    points: [[f64; 3]; 3],
    original: u32,
}

/// The sampled surface. Owns copies of the positions it uses.
#[derive(Clone, Debug)]
pub struct Field {
    faces: Vec<Face>,
    /// Every input vertex's height, by input index (edited-surface volumes).
    heights: Vec<f64>,
    excluded: u32,
    bounds: Bounds,
    origin: [f64; 2],
    cell: f64,
    columns: u32,
    rows: u32,
    /// Per grid cell, upward-face positions in ascending original index.
    buckets: Vec<Vec<u32>>,
}

fn barycentric(points: &[[f64; 3]; 3], x: f64, y: f64) -> Option<[f64; 3]> {
    let [[ax, ay, _], [bx, by, _], [cx, cy, _]] = *points;
    let area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if area <= 0.0 {
        return None;
    }
    let wa = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area;
    let wb = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area;
    Some([wa, wb, 1.0 - wa - wb])
}

fn upward(points: &[[f64; 3]; 3]) -> bool {
    let [origin, second, third] = *points;
    let (along, across) = (
        [
            second[0] - origin[0],
            second[1] - origin[1],
            second[2] - origin[2],
        ],
        [
            third[0] - origin[0],
            third[1] - origin[1],
            third[2] - origin[2],
        ],
    );
    let normal = [
        along[1] * across[2] - along[2] * across[1],
        along[2] * across[0] - along[0] * across[2],
        along[0] * across[1] - along[1] * across[0],
    ];
    let length = (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]).sqrt();
    length > 0.0 && normal[2] / length > UPWARD
}

impl Field {
    /// Build a field over the upward faces of a triangle mesh.
    ///
    /// # Errors
    /// See [`FieldError`].
    pub fn new(points: &[f64], triangles: &[u32]) -> Result<Self, FieldError> {
        if !points.len().is_multiple_of(3) || !triangles.len().is_multiple_of(3) {
            return Err(FieldError::Shape);
        }
        if u32::try_from(points.len() / 3).is_err() || u32::try_from(triangles.len() / 3).is_err() {
            return Err(FieldError::Shape);
        }
        if points.iter().any(|value| !value.is_finite()) {
            return Err(FieldError::NonFinite);
        }
        let vertices = plan::vertices(points);
        let mut faces = Vec::new();
        let mut excluded = 0_u32;
        for (original, &corners) in (0_u32..).zip(triangles.as_chunks::<3>().0) {
            let mut face_points = [[0.0; 3]; 3];
            for (slot, &corner) in face_points.iter_mut().zip(&corners) {
                *slot = *vertices.get(corner as usize).ok_or(FieldError::Index)?;
            }
            if upward(&face_points) {
                faces.push(Face {
                    corners,
                    points: face_points,
                    original,
                });
            } else {
                excluded += 1;
            }
        }
        if faces.is_empty() {
            return Err(FieldError::Empty);
        }
        let mut bounds = Bounds {
            min: [f64::INFINITY; 3],
            max: [f64::NEG_INFINITY; 3],
        };
        for point in faces.iter().flat_map(|face| face.points) {
            for ((low, high), value) in bounds.min.iter_mut().zip(bounds.max.iter_mut()).zip(point)
            {
                *low = low.min(value);
                *high = high.max(value);
            }
        }
        let (width, height) = (bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1]);
        // About one face per cell on average, at most 4097 cells per axis.
        let count = f64::from(u32::try_from(faces.len()).map_err(|_| FieldError::Shape)?);
        let cell = ((width * height).max(f64::MIN_POSITIVE) / count)
            .sqrt()
            .max(width.max(height) / 4096.0)
            .max(1e-9);
        let columns = clamp_index(width / cell, 4096) + 1;
        let rows = clamp_index(height / cell, 4096) + 1;
        let mut field = Self {
            faces,
            heights: vertices.iter().map(|&[_, _, z]| z).collect(),
            excluded,
            bounds,
            origin: [bounds.min[0], bounds.min[1]],
            cell,
            columns,
            rows,
            buckets: vec![Vec::new(); columns as usize * rows as usize],
        };
        for position in 0..field.faces.len() {
            let Some(face) = field.faces.get(position) else {
                continue;
            };
            let xs = face.points.map(|p| p[0]);
            let ys = face.points.map(|p| p[1]);
            let (c0, r0) = field.cell_of(
                xs.iter().copied().fold(f64::INFINITY, f64::min),
                ys.iter().copied().fold(f64::INFINITY, f64::min),
            );
            let (c1, r1) = field.cell_of(
                xs.iter().copied().fold(f64::NEG_INFINITY, f64::max),
                ys.iter().copied().fold(f64::NEG_INFINITY, f64::max),
            );
            let index = u32::try_from(position).map_err(|_| FieldError::Shape)?;
            for row in r0..=r1 {
                for column in c0..=c1 {
                    if let Some(bucket) = field
                        .buckets
                        .get_mut(row as usize * columns as usize + column as usize)
                    {
                        bucket.push(index);
                    }
                }
            }
        }
        Ok(field)
    }

    fn cell_of(&self, x: f64, y: f64) -> (u32, u32) {
        (
            clamp_index((x - self.origin[0]) / self.cell, self.columns - 1),
            clamp_index((y - self.origin[1]) / self.cell, self.rows - 1),
        )
    }

    fn locate(&self, x: f64, y: f64) -> Option<(&Face, [f64; 3])> {
        if !(x.is_finite() && y.is_finite()) {
            return None;
        }
        let pad = self.cell * 1e-9;
        if x < self.bounds.min[0] - pad
            || x > self.bounds.max[0] + pad
            || y < self.bounds.min[1] - pad
            || y > self.bounds.max[1] + pad
        {
            return None;
        }
        let (column, row) = self.cell_of(x, y);
        let bucket = self
            .buckets
            .get(row as usize * self.columns as usize + column as usize)?;
        bucket.iter().find_map(|&position| {
            let face = self.faces.get(position as usize)?;
            let weights = barycentric(&face.points, x, y)?;
            weights
                .iter()
                .all(|&w| w >= -INSIDE)
                .then_some((face, weights))
        })
    }

    /// Height at `(x, y)`, or `None` off the surface.
    #[must_use]
    pub fn height_at(&self, x: f64, y: f64) -> Option<Sample> {
        self.locate(x, y).map(|(face, weights)| Sample {
            height: face
                .points
                .iter()
                .zip(&weights)
                .map(|(p, w)| p[2] * w)
                .sum(),
            face: face.original,
            weights,
        })
    }

    /// The sampled face's corner vertex indices, in corner order.
    #[must_use]
    pub fn corners(&self, x: f64, y: f64) -> Option<[u32; 3]> {
        self.locate(x, y).map(|(face, _)| face.corners)
    }

    /// Unit upward normal of the face at `(x, y)`.
    #[must_use]
    pub fn normal_at(&self, x: f64, y: f64) -> Option<[f64; 3]> {
        let (face, _) = self.locate(x, y)?;
        let [dx, dy] = plane(&face.points);
        let length = (dx * dx + dy * dy + 1.0).sqrt();
        Some([-dx / length, -dy / length, 1.0 / length])
    }

    /// Gradient at `(x, y)` from samples `step` metres away.
    ///
    /// # Errors
    /// [`FieldError::Step`] when `step` is not finite and positive.
    pub fn gradient_at(&self, x: f64, y: f64, step: f64) -> Result<Option<Gradient>, FieldError> {
        if !(step.is_finite() && step > 0.0) {
            return Err(FieldError::Step);
        }
        let Some((face, _)) = self.locate(x, y) else {
            return Ok(None);
        };
        let here = self.height_at(x, y).map_or(0.0, |sample| sample.height);
        let face_slope = plane(&face.points);
        let component = |east: f64, north: f64, face: f64| {
            let ahead = self
                .height_at(x + east, y + north)
                .map(|sample| sample.height);
            let behind = self
                .height_at(x - east, y - north)
                .map(|sample| sample.height);
            match (ahead, behind) {
                (Some(high), Some(low)) => ((high - low) / (2.0 * step), GradientMethod::Central),
                (Some(high), None) => ((high - here) / step, GradientMethod::OneSided),
                (None, Some(low)) => ((here - low) / step, GradientMethod::OneSided),
                (None, None) => (face, GradientMethod::Face),
            }
        };
        let (along_x, method_x) = component(step, 0.0, face_slope[0]);
        let (along_y, method_y) = component(0.0, step, face_slope[1]);
        Ok(Some(Gradient {
            slope: [along_x, along_y],
            method: [method_x, method_y],
        }))
    }

    /// Slope at `(x, y)` in radians above horizontal.
    ///
    /// # Errors
    /// As [`Field::gradient_at`].
    pub fn slope_at(&self, x: f64, y: f64, step: f64) -> Result<Option<f64>, FieldError> {
        Ok(self
            .gradient_at(x, y, step)?
            .map(|g| g.slope[0].hypot(g.slope[1]).atan()))
    }

    /// Downslope compass bearing in radians clockwise from +y, in `[0, 2 pi)`;
    /// `Ok(None)` off the surface or where the gradient is exactly zero.
    ///
    /// # Errors
    /// As [`Field::gradient_at`].
    pub fn aspect_at(&self, x: f64, y: f64, step: f64) -> Result<Option<f64>, FieldError> {
        Ok(self.gradient_at(x, y, step)?.and_then(|g| {
            let [dx, dy] = g.slope;
            if dx == 0.0 && dy == 0.0 {
                return None;
            }
            // Downslope is -gradient; bearing = atan2(east, north).
            Some((-dx).atan2(-dy).rem_euclid(std::f64::consts::TAU))
        }))
    }

    /// Number of input vertices.
    pub(super) fn vertex_count(&self) -> usize {
        self.heights.len()
    }

    /// An input vertex's height.
    pub(super) fn vertex_height(&self, vertex: usize) -> Option<f64> {
        self.heights.get(vertex).copied()
    }

    /// Bounds of the upward faces' vertices.
    #[must_use]
    pub const fn bounds(&self) -> Bounds {
        self.bounds
    }

    /// Plan area and counts of the faces the field covers.
    #[must_use]
    pub fn coverage(&self) -> Coverage {
        let area = self
            .faces
            .iter()
            .map(|face| {
                let [[ax, ay, _], [bx, by, _], [cx, cy, _]] = face.points;
                0.5 * ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))
            })
            .sum();
        Coverage {
            area,
            faces: u32::try_from(self.faces.len()).unwrap_or(u32::MAX),
            excluded: self.excluded,
        }
    }

    /// Plan boundary edges of the upward faces (edges with one upward face),
    /// as xyz segments in ascending vertex-pair order.
    pub(super) fn boundary(&self) -> Vec<Segment> {
        let mut edges: Vec<((u32, u32), Segment)> = Vec::new();
        for face in &self.faces {
            let [first, second, third] = face.corners;
            let [p_first, p_second, p_third] = face.points;
            for ((from, to), segment) in [
                ((first, second), [p_first, p_second]),
                ((second, third), [p_second, p_third]),
                ((third, first), [p_third, p_first]),
            ] {
                edges.push(((from.min(to), from.max(to)), segment));
            }
        }
        edges.sort_by_key(|&(key, _)| key);
        let mut out = Vec::new();
        let mut index = 0;
        while let Some(&(key, segment)) = edges.get(index) {
            let mut run = 1;
            while edges
                .get(index + run)
                .is_some_and(|&(other, _)| other == key)
            {
                run += 1;
            }
            if run == 1 {
                out.push(segment);
            }
            index += run;
        }
        out
    }

    /// Node heights over `nodes` (x, y) nodes at `cell` spacing from `origin`.
    ///
    /// # Errors
    /// [`FieldError::Step`] for a cell that is not finite and positive, or
    /// [`FieldError::Shape`] for a raster too large to index.
    pub fn raster(
        &self,
        origin: [f64; 2],
        cell: f64,
        nodes: [u32; 2],
    ) -> Result<Raster, FieldError> {
        if !(cell.is_finite() && cell > 0.0) || origin.iter().any(|value| !value.is_finite()) {
            return Err(FieldError::Step);
        }
        let count = (nodes[0] as usize)
            .checked_mul(nodes[1] as usize)
            .ok_or(FieldError::Shape)?;
        let mut heights = Vec::with_capacity(count);
        for row in 0..nodes[1] {
            for column in 0..nodes[0] {
                let x = origin[0] + f64::from(column) * cell;
                let y = origin[1] + f64::from(row) * cell;
                heights.push(self.height_at(x, y).map(|sample| sample.height));
            }
        }
        Ok(Raster {
            origin,
            cell,
            nodes,
            heights,
        })
    }
}

/// `(dz/dx, dz/dy)` of a face's plane (upward faces have positive plan area).
fn plane(points: &[[f64; 3]; 3]) -> [f64; 2] {
    let [[ax, ay, az], [bx, by, bz], [cx, cy, cz]] = *points;
    let area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if area == 0.0 {
        return [0.0, 0.0];
    }
    let east = ((bz - az) * (cy - ay) - (cz - az) * (by - ay)) / area;
    let north = ((cz - az) * (bx - ax) - (bz - az) * (cx - ax)) / area;
    [east, north]
}

#[cfg(test)]
mod tests;
