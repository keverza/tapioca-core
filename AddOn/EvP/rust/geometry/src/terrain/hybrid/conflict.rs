//! Located conflicts of a band (hybrid design §7.4): where a station side
//! cannot be built as authored, with a magnitude and a suggested fix, instead of
//! a single refusal code or a count.
//!
//! Hard conflicts make [`super::build`] refuse (it still reports only the first,
//! with its existing [`super::HybridError`]); [`assess`] lists all of them so the
//! editor can show every place at once. Soft conflicts do not stop the build:
//! the band is restricted there, and [`super::Built::conflicts`] names where.

use super::{BuildRequest, HybridError, SIDES, index};
use crate::terrain::daylight::Locus;
use crate::terrain::design::{Classification, Design};
use crate::terrain::edit::{Effect, Hand};
use crate::terrain::field::Field;

/// What is wrong at a station side.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConflictCode {
    /// No ground intersection while the face stays on the surface. Magnitude:
    /// `z_cone - z_base` where the search stopped, metres. Hard.
    DaylightNotReached,
    /// The face meets the ground only beyond the side's limit. Magnitude: the
    /// reach it needs, metres from the edge. Hard.
    DaylightBeyondLimit,
    /// The face would be vertical; a wall is a separate surface. Magnitude 0.
    /// Hard.
    VerticalFace,
    /// The side's daylight count or at-grade state changes here. Magnitude: the
    /// root count at this station (0 at grade). Hard.
    DaylightTopology,
    /// The edge is in cut but the band may only fill, or the reverse, so the
    /// band is held at the ground there. Magnitude: `z_edge - z_base`, metres.
    /// Soft.
    EdgeContradictsEffect,
}

impl ConflictCode {
    /// Whether the build refuses on it.
    #[must_use]
    pub const fn hard(self) -> bool {
        !matches!(self, Self::EdgeContradictsEffect)
    }

    /// The stable code text.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::DaylightNotReached => "DaylightNotReached",
            Self::DaylightBeyondLimit => "DaylightBeyondLimit",
            Self::VerticalFace => "VerticalFace",
            Self::DaylightTopology => "DaylightTopology",
            Self::EdgeContradictsEffect => "EdgeContradictsEffect",
        }
    }

    /// The suggested fix.
    #[must_use]
    pub const fn suggestion(self) -> &'static str {
        match self {
            Self::DaylightNotReached => {
                "steepen the side slope, widen the limit or add a retaining wall"
            }
            Self::DaylightBeyondLimit => "raise the side's limit to the reported reach",
            Self::VerticalFace => "use a separate wall surface or a finite side slope",
            Self::DaylightTopology => {
                "split the band where the ground changes or adjust the profile so the side stays one kind"
            }
            Self::EdgeContradictsEffect => "clear the cut/fill restriction or change the elevation",
        }
    }
}

/// One located conflict.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Conflict {
    /// What is wrong.
    pub code: ConflictCode,
    /// Station index in the band.
    pub station: u32,
    /// The side.
    pub side: Hand,
    /// Plan position and height of the place, world metres: the band edge, or
    /// where a face search stopped (at the limit or off the surface).
    pub position: [f64; 3],
    /// See [`ConflictCode`] for its meaning per code.
    pub magnitude: f64,
}

/// A side's state at one station for the topology comparison.
#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    AtGrade,
    Met(usize),
}

fn at(field: &Field, edge: [f64; 3], outward: [f64; 2], distance: f64) -> [f64; 3] {
    let (x, y) = (
        edge[0] + outward[0] * distance,
        edge[1] + outward[1] * distance,
    );
    let z = field
        .height_at(x, y)
        .map_or(edge[2], |sample| sample.height);
    [x, y, z]
}

/// Every located conflict of a validated design, by side then station.
pub(super) fn locate(
    design: &Design<'_>,
    field: &Field,
    daylight: &[[Option<Locus>; 2]],
) -> Result<Vec<Conflict>, HybridError> {
    let classes = design.classify().map_err(HybridError::Design)?;
    let effect = design.effect();
    let missing = HybridError::Design(crate::terrain::design::DesignError::Stations);
    let mut conflicts = Vec::new();
    for (slot, side, sign) in SIDES {
        let mut previous: Option<State> = None;
        for (number, (sides, loci)) in classes.iter().zip(daylight).enumerate() {
            let station = index(number)?;
            let edges = design.edges(number).ok_or(missing)?;
            let [nx, ny] = design.normal(number).ok_or(missing)?;
            let outward = [sign * nx, sign * ny];
            let (Some(class), Some(&edge), Some(locus)) =
                (sides.get(slot), edges.get(slot), loci.get(slot))
            else {
                return Err(missing);
            };
            let mut push = |code, position, magnitude| {
                conflicts.push(Conflict {
                    code,
                    station,
                    side,
                    position,
                    magnitude,
                });
            };
            if matches!(
                (class.classification, effect),
                (Classification::Cut, Effect::Fill) | (Classification::Fill, Effect::Cut)
            ) {
                push(ConflictCode::EdgeContradictsEffect, edge, class.delta);
            }
            let state = match locus {
                None => Some(State::AtGrade),
                Some(Locus::Met { roots }) => Some(State::Met(roots.len())),
                Some(Locus::AtLimit { distance, .. }) => {
                    let limit = match class.classification {
                        Classification::Cut => design.station(number).map_or(0.0, |s| s.cut_limit),
                        _ => design.station(number).map_or(0.0, |s| s.fill_limit),
                    };
                    push(
                        ConflictCode::DaylightBeyondLimit,
                        at(field, edge, outward, limit),
                        *distance,
                    );
                    None
                }
                Some(Locus::NotReached { at_limit, residual }) => {
                    push(
                        ConflictCode::DaylightNotReached,
                        at(field, edge, outward, *at_limit),
                        *residual,
                    );
                    None
                }
                Some(Locus::Vertical) => {
                    push(ConflictCode::VerticalFace, edge, 0.0);
                    None
                }
            };
            if let Some(state) = state {
                if previous.is_some_and(|before| before != state) {
                    let count = match state {
                        State::AtGrade => 0.0,
                        State::Met(n) => f64::from(index(n)?),
                    };
                    push(ConflictCode::DaylightTopology, edge, count);
                }
                previous = Some(state);
            }
        }
    }
    Ok(conflicts)
}

/// Every located conflict of a band request, hard and soft, without building.
///
/// One daylight search per station side; worker-classified.
///
/// # Errors
/// [`HybridError::Field`] or a design refusal that has no location (stations,
/// parameters, slopes, an edge off the surface).
pub fn assess(request: &BuildRequest<'_>) -> Result<Vec<Conflict>, HybridError> {
    let field = Field::new(request.points, request.triangles).map_err(|_| HybridError::Field)?;
    let design =
        Design::new(&request.band, &field, request.tolerance).map_err(HybridError::Design)?;
    let daylight = design.daylight().map_err(HybridError::Design)?;
    locate(&design, &field, &daylight)
}
