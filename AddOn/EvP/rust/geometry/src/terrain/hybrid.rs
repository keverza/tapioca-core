//! S4.1 hybrid build (P1): one path band materialised on the base terrain as a
//! constrained TIN whose breaklines are the design's real edges.
//!
//! **Lines.** From the validated [`Design`]: each side's band edge, its shoulder
//! end (only when a shoulder is non-zero), one daylight line per root, and a
//! cross-section at the first and last station (the band's ends are creases
//! too), plus the centreline when a station is crowned (the crown's crease;
//! the end sections then carry a centre vertex). Daylight must be met on every transition side: a side that is not
//! reached within its limit, or that needs a vertical face, refuses rather than
//! being clamped. Every station of a side must have the same root count; a
//! changing count (a locus that splits or merges) is a later phase.
//!
//! **Mesh.** [`retriangulate_region`] rebuilds the region interior with those
//! lines and every base edge as constraints and keeps the outside verbatim.
//!
//! **Heights.** Vertices of any outside triangle (including the shared ring)
//! and every daylight-line vertex keep the base height exactly; every other
//! inside vertex takes [`Design::height`]. Constraint vertices are therefore
//! exact; the remaining error is the chord error between stations.
//!
//! **Volumes.** The base is affine on every output triangle (its edges are
//! constraints), so [`exact_cut_fill`] gives the exact cut/fill of the two
//! triangulated surfaces: site inside the region, context outside it.
//!
//! Conflicts are located ([`conflict`]): the build refuses on the first hard
//! one; [`assess`] lists them all, and soft ones ride on [`Built::conflicts`].
//!
//! P1 rebuilds the whole region interior per call; region-limited rebuilds,
//! areas, grading spots and several bands are later phases. [`build_locked`]
//! adds fixed-height point/line constraints without a second surface authority.

use super::Error;
use super::daylight::Locus;
use super::design::{Band, Design, DesignError};
use super::edit::Hand;
use super::field::Field;
use super::plan;
use super::region::{RegionError, RegionMesh, RegionRequest, retriangulate_region_with_points};
use super::volume::VolumeError;
use super::volume::exact::{ExactReport, ExactRequest, exact_cut_fill};

/// Borrowed build request.
#[derive(Clone, Copy, Debug)]
pub struct BuildRequest<'a> {
    /// Flat xyz base mesh vertices, world metres.
    pub points: &'a [f64],
    /// Counter-clockwise (in plan) vertex triples of a plan-view terrain.
    pub triangles: &'a [u32],
    /// Closed straight ring (z ignored) bounding the rebuild; the band and its
    /// daylight lines must lie strictly inside it.
    pub region: &'a [f64],
    /// The band.
    pub band: Band<'a>,
    /// At-grade height band, metres.
    pub tolerance: f64,
}

/// What a breakline of the build is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LineKind {
    /// A band edge.
    Edge(Hand),
    /// The end of a side's level shoulder.
    Shoulder(Hand),
    /// Where a side's face meets the ground; `root` counts outward.
    Daylight {
        /// The side.
        side: Hand,
        /// Root index, ascending outward.
        root: u32,
    },
    /// The cross-section at the first (`false`) or last (`true`) station.
    End(bool),
    /// The centreline, a crease of a crowned band.
    Crown,
    /// A fixed-height line, indexed in [`Locked::lines`].
    Locked(u32),
}

/// The built surface and its mapping.
#[derive(Clone, Debug, PartialEq)]
pub struct Built {
    /// Flat xyz proposed vertices; the plan positions are `mesh.points`'.
    pub points: Vec<f64>,
    /// The rebuilt base mesh with its vertex/triangle provenance.
    pub mesh: RegionMesh,
    /// Per breakline of `mesh` (its `line` index), what it is.
    pub lines: Vec<LineKind>,
    /// Per vertex, `Some(0)` when the band changed its height (bitwise).
    pub owners: Vec<Option<u32>>,
    /// `[left, right]` daylight per station (`None` at grade).
    pub daylight: Vec<[Option<Locus>; 2]>,
    /// Exact cut/fill: site inside the region, context outside.
    pub volumes: ExactReport,
    /// Soft located conflicts: where the band is held back but still built.
    pub conflicts: Vec<Conflict>,
    /// Fixed-height vertices, with every contributing control. Their heights
    /// are owned by those controls, not by the band (`owners` is `None`).
    pub locked: Vec<LockedVertex>,
}

/// Build refusals.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum HybridError {
    /// The base mesh is not a field.
    Field,
    /// The band refused.
    Design(DesignError),
    /// The region rebuild refused.
    Region(RegionError),
    /// The exact volume refused.
    Volume(VolumeError),
    /// A side's face does not meet the ground within its limit.
    DaylightNotReached {
        /// Station index.
        station: u32,
        /// The side.
        side: Hand,
    },
    /// A side needs a vertical face (a wall is a separate surface).
    Vertical {
        /// Station index.
        station: u32,
        /// The side.
        side: Hand,
    },
    /// A side's root count or classification changes along the band.
    DaylightTopology {
        /// The side.
        side: Hand,
    },
    /// Invalid fixed-height point/line pool or unusable line interpolation.
    LockedParameters,
    /// The grading surface or another control disagrees with a fixed height.
    LockedConflict(LockedConflict),
}

impl HybridError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Field => "HybridField: the base mesh has no upward face or is malformed",
            Self::Design(error) => error.message(),
            Self::Region(error) => error.message(),
            Self::Volume(error) => error.message(),
            Self::DaylightNotReached { .. } => {
                "DaylightNotReached: a face does not meet the ground within its limit; steepen it, widen the limit or add a wall"
            }
            Self::Vertical { .. } => {
                "HybridVertical: a vertical face needs a separate wall surface"
            }
            Self::DaylightTopology { .. } => {
                "HybridDaylightTopology: a side's daylight count or classification changes along the band"
            }
            Self::LockedParameters => {
                "LockedParameters: finite xyz controls and nonzero open line segments required"
            }
            Self::LockedConflict(_) => {
                "LockedConflict: the grading surface or another control disagrees with a fixed height; narrow the band or align the elevations"
            }
        }
    }
}

pub mod conflict;
pub use conflict::{Conflict, ConflictCode, assess};
mod locked;
pub use locked::{LockRef, Locked, LockedConflict, LockedVertex};

/// Per station of one side: its shoulder and daylight roots; `None` at grade.
type SideOffsets = Option<Vec<(f64, Vec<f64>)>>;

const SIDES: [(usize, Hand, f64); 2] = [(0, Hand::Left, 1.0), (1, Hand::Right, -1.0)];

fn index(value: usize) -> Result<u32, HybridError> {
    u32::try_from(value).map_err(|_| HybridError::Region(RegionError::Terrain(Error::Shape)))
}

/// Per side, per station, the outward offsets of its lines from the edge:
/// `None` for an at-grade side, else `(shoulder, roots)`.
fn offsets(
    design: &Design<'_>,
    daylight: &[[Option<Locus>; 2]],
) -> Result<[SideOffsets; 2], HybridError> {
    let mut result = [None, None];
    for (slot, side, _) in SIDES {
        let mut stations = Vec::new();
        let mut graded = 0;
        for (number, sides) in daylight.iter().enumerate() {
            let station = index(number)?;
            let shoulder = design.station(number).map_or(0.0, |s| match side {
                Hand::Left => s.shoulder_left,
                Hand::Right => s.shoulder_right,
            });
            match sides.get(slot) {
                Some(None) => graded += 1,
                Some(Some(Locus::Met { roots })) => stations.push((shoulder, roots.clone())),
                Some(Some(Locus::Vertical)) => return Err(HybridError::Vertical { station, side }),
                _ => return Err(HybridError::DaylightNotReached { station, side }),
            }
        }
        let counts_differ = stations
            .windows(2)
            .any(|pair| matches!(pair, [a, b] if a.1.len() != b.1.len()));
        if counts_differ || (graded != 0 && !stations.is_empty()) {
            return Err(HybridError::DaylightTopology { side });
        }
        if let Some(target) = result.get_mut(slot) {
            *target = (!stations.is_empty()).then_some(stations);
        }
    }
    Ok(result)
}

/// The design lines in plan (z ignored) and what each one is.
fn lines(
    design: &Design<'_>,
    offsets: &[SideOffsets; 2],
) -> Result<(Vec<Vec<f64>>, Vec<LineKind>), HybridError> {
    let count = design.stations();
    let missing = HybridError::Design(DesignError::Stations);
    // A side carries a shoulder line when any of its stations has a shoulder.
    let shoulders = SIDES.map(|(slot, _, _)| {
        offsets
            .get(slot)
            .and_then(Option::as_ref)
            .is_some_and(|stations| stations.iter().any(|(s, _)| *s > 0.0))
    });
    // Station -> side -> ordered outward points (edge, shoulder, roots).
    let mut sections: Vec<[Vec<[f64; 2]>; 2]> = Vec::with_capacity(count);
    for station in 0..count {
        let edges = design.edges(station).ok_or(missing)?;
        let [nx, ny] = design.normal(station).ok_or(missing)?;
        let mut pair: [Vec<[f64; 2]>; 2] = [Vec::new(), Vec::new()];
        for (slot, _, sign) in SIDES {
            let (Some(edge), Some(points)) = (edges.get(slot), pair.get_mut(slot)) else {
                return Err(missing);
            };
            let at = |d: f64| [edge[0] + sign * nx * d, edge[1] + sign * ny * d];
            points.push(at(0.0));
            if let Some(Some(stations)) = offsets.get(slot)
                && let Some((shoulder, roots)) = stations.get(station)
            {
                if shoulders.get(slot).copied().unwrap_or(false) {
                    points.push(at(*shoulder));
                }
                points.extend(roots.iter().map(|&root| at(root)));
            }
        }
        sections.push(pair);
    }
    let mut polylines = Vec::new();
    let mut kinds = Vec::new();
    for (slot, side, _) in SIDES {
        let width = sections
            .first()
            .and_then(|pair| pair.get(slot))
            .map_or(0, Vec::len);
        let shoulder = shoulders.get(slot).copied().unwrap_or(false);
        for position in 0..width {
            let kind = match position {
                0 => LineKind::Edge(side),
                1 if shoulder => LineKind::Shoulder(side),
                _ => LineKind::Daylight {
                    side,
                    root: index(position - 1 - usize::from(shoulder))?,
                },
            };
            let mut line = Vec::with_capacity(count * 3);
            for pair in &sections {
                let &[x, y] = pair
                    .get(slot)
                    .and_then(|points| points.get(position))
                    .ok_or(HybridError::DaylightTopology { side })?;
                line.extend([x, y, 0.0]);
            }
            polylines.push(line);
            kinds.push(kind);
        }
    }
    let crowned = design.crowned();
    if crowned {
        let mut line = Vec::with_capacity(count * 3);
        for station in 0..count {
            let [x, y, _] = design.station(station).ok_or(missing)?.position;
            line.extend([x, y, 0.0]);
        }
        polylines.push(line);
        kinds.push(LineKind::Crown);
    }
    for (last, station) in [(false, 0), (true, count.saturating_sub(1))] {
        let pair = sections.get(station).ok_or(missing)?;
        let mut line: Vec<f64> = Vec::new();
        let (left, right) = (pair.first().ok_or(missing)?, pair.get(1).ok_or(missing)?);
        let [cx, cy, _] = design.station(station).ok_or(missing)?.position;
        let centre: &[[f64; 2]] = if crowned { &[[cx, cy]] } else { &[] };
        for &[x, y] in left.iter().rev().chain(centre).chain(right) {
            let duplicate = line.len() >= 3
                && matches!(line.get(line.len() - 3..), Some(&[px, py, _]) if (px - x).hypot(py - y) <= plan::ON);
            if !duplicate {
                line.extend([x, y, 0.0]);
            }
        }
        polylines.push(line);
        kinds.push(LineKind::End(last));
    }
    Ok((polylines, kinds))
}

/// Build the band on the base terrain inside `request.region`.
///
/// One daylight search per station side, one region rebuild and one exact
/// volume pass; worker-classified. Nothing partial is returned.
///
/// # Errors
/// See [`HybridError`].
pub fn build(request: &BuildRequest<'_>) -> Result<Built, HybridError> {
    build_locked(request, Locked::default())
}

/// Build with a fixed-height tier. Controls are constrained in the same TIN as
/// the band; independent points are inserted as vertices and every split of a
/// line takes its linearly interpolated fixed height. Conflicting controls, or
/// grading that would move a control by more than `request.tolerance`, return a
/// located [`HybridError::LockedConflict`]. Compatible values retain the fixed
/// height exactly. No implicit ramp, clamping or objective penalty is introduced.
/// At daylight vertices the stronger bitwise base-preservation contract wins:
/// any different fixed height refuses, even within the compatibility tolerance.
/// Controls are constraints on the proposed surface, not independent grading
/// tools; use a band/profile to reach a control that differs from the base.
///
/// # Errors
/// See [`HybridError`]. Controls on/outside the region refuse through
/// [`RegionError`]; surface/area locks and soft-tier composition are not provided.
pub fn build_locked(request: &BuildRequest<'_>, locked: Locked<'_>) -> Result<Built, HybridError> {
    locked::validate(locked)?;
    let field = Field::new(request.points, request.triangles).map_err(|_| HybridError::Field)?;
    let design =
        Design::new(&request.band, &field, request.tolerance).map_err(HybridError::Design)?;
    let daylight = design.daylight().map_err(HybridError::Design)?;
    let offsets = offsets(&design, &daylight)?;
    let conflicts: Vec<Conflict> = conflict::locate(&design, &field, &daylight)?
        .into_iter()
        .filter(|c| !c.code.hard())
        .collect();
    let (polylines, design_kinds) = lines(&design, &offsets)?;
    let mut kinds = (0..locked.lines.len())
        .map(|line| index(line).map(LineKind::Locked))
        .collect::<Result<Vec<_>, _>>()?;
    kinds.extend(design_kinds);
    let breaklines: Vec<&[f64]> = locked
        .lines
        .iter()
        .copied()
        .chain(polylines.iter().map(Vec::as_slice))
        .collect();
    let mesh = retriangulate_region_with_points(
        &RegionRequest {
            points: request.points,
            triangles: request.triangles,
            region: request.region,
            breaklines: &breaklines,
        },
        locked.points,
    )
    .map_err(HybridError::Region)?;
    let controls = locked::resolve(&mesh, locked, request.tolerance)?;

    let vertices = plan::vertices(&mesh.points);
    let mut fixed = vec![false; vertices.len()];
    for (&[a, b, c], _) in mesh
        .triangles
        .as_chunks::<3>()
        .0
        .iter()
        .zip(&mesh.inside)
        .filter(|(_, inside)| !**inside)
    {
        for vertex in [a, b, c] {
            if let Some(flag) = fixed.get_mut(vertex as usize) {
                *flag = true;
            }
        }
    }
    for edge in &mesh.line_edges {
        if matches!(
            kinds.get(edge.line as usize),
            Some(LineKind::Daylight { .. })
        ) {
            for vertex in edge.vertices {
                if let Some(flag) = fixed.get_mut(vertex as usize) {
                    *flag = true;
                }
            }
        }
    }
    let mut points = Vec::with_capacity(mesh.points.len());
    let mut base = Vec::with_capacity(vertices.len());
    let mut owners = Vec::with_capacity(vertices.len());
    for (&[x, y, z], &keep) in vertices.iter().zip(&fixed) {
        let height = if keep {
            z
        } else {
            design
                .height(x, y)
                .map_err(HybridError::Design)?
                .ok_or(HybridError::Design(DesignError::Coverage))?
                .height
        };
        points.extend([x, y, height]);
        base.push(z);
        owners.push((height.to_bits() != z.to_bits()).then_some(0));
    }
    locked::apply(&mut points, &controls, request.tolerance, &fixed)?;
    for control in &controls {
        if let Some(owner) = owners.get_mut(control.vertex as usize) {
            *owner = None;
        }
    }
    let volumes = exact_cut_fill(&ExactRequest {
        points: &points,
        base: &base,
        triangles: &mesh.triangles,
        inside: Some(&mesh.inside),
    })
    .map_err(HybridError::Volume)?;
    Ok(Built {
        points,
        mesh,
        lines: kinds,
        owners,
        daylight,
        volumes,
        conflicts,
        locked: controls,
    })
}

#[cfg(test)]
mod tests;
