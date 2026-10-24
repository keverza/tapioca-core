//! S4.1 design band: the authoritative height field of one path band, evaluated
//! per query against a base [`Field`]. Public because the hybrid build and its
//! C ABI consume it; nothing here is stored as a grid or second height field.
//!
//! **Band.** A station sequence (the caller samples its own profile and
//! sections; nothing is re-solved here). Between two stations every parameter is
//! linear in the centreline parameter, so the band edge is a straight 3D segment.
//! Inside `-half_right <= lateral <= half_left` the height is the centreline
//! height plus the [`Camber`] twist, `lateral` positive to the left of the travel
//! direction. A one-way fall uses the [`Crossfall`] convention of the edit
//! operator: `-g * lateral` when it falls to the left, `+g * lateral` to the
//! right. A crown falls both ways from the centreline, `-g * |lateral|`, so the
//! centreline is a crease (the hybrid build makes it a breakline).
//!
//! **Classification** is per side, from the edge against the ground:
//! `delta = z_edge - z_base(edge)`, cut below `-tolerance`, fill above
//! `+tolerance`, at grade otherwise (no transition).
//!
//! **Transition.** Outside the band the side's face runs from the edge after a
//! level shoulder (see [`super::daylight`]): a cut keeps `min(base, cone)`, a
//! fill `max(base, cone)`, which reproduces every daylight interval at any query
//! point. Beyond the side's limit, or past either end station (end caps are a
//! later phase), the design leaves the base untouched. A vertical face ends the
//! design at the edge. [`Effect`] then restricts the result as in the operator.
//!
//! **Rounding.** A station's `rounding` (plan metres, 0 sharp) rounds a face's
//! two creases. Its top: the face's first `rounding` metres are the cubic that
//! leaves the edge (or the level shoulder) at the band's own outward grade and
//! arrives on the straight face with the face's grade, so beyond it the face and
//! its daylight are exactly the sharp ones. Its toe: where the face is within
//! `grade * rounding` of the ground it blends into it as `base +/- gap * s(gap /
//! (grade * rounding))`, `s` the smoothstep -- the same height at the daylight
//! and on the face, and the same slope at both ends of the blend.
//!
//! Plan positions are world metres in `f64`; heights in metres.

use super::daylight::{DaylightError, Locus, Ray, Sense, daylight_roots};
use super::edit::{Crossfall, Daylight, Effect, Hand};
use super::field::Field;
use super::plan;
use crate::slope::Slope;

/// One sampled cross-section of a path band.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Station {
    /// Centreline plan position and design height, world metres.
    pub position: [f64; 3],
    /// Band width left of the centreline, metres, non-negative.
    pub half_left: f64,
    /// Band width right of the centreline, metres, non-negative.
    pub half_right: f64,
    /// Cross slope about the centreline; `None` is level.
    pub crossfall: Option<Camber>,
    /// Level run beyond the left edge before its face, metres.
    pub shoulder_left: f64,
    /// Level run beyond the right edge before its face, metres.
    pub shoulder_right: f64,
    /// Face grade where the design is below the ground.
    pub cut: Slope,
    /// Face grade where the design is above the ground.
    pub fill: Slope,
    /// Plan reach of a cut face from the edge, metres.
    pub cut_limit: f64,
    /// Plan reach of a fill face from the edge, metres.
    pub fill_limit: f64,
    /// Plan length over which each face rounds into the band and into the
    /// ground, metres, non-negative; 0 keeps both creases sharp.
    pub rounding: f64,
}

/// How a cross-section falls across the band.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Camber {
    /// One plane falling to one side.
    OneWay(Crossfall),
    /// Both halves fall away from the centreline at this grade.
    Crown(Slope),
}

/// A path band: at least two stations with distinct consecutive plan positions.
#[derive(Clone, Copy, Debug)]
pub struct Band<'a> {
    /// Stations in travel order.
    pub stations: &'a [Station],
    /// Which way the band may move the surface.
    pub effect: Effect,
}

/// How a band edge sits against the base surface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Classification {
    /// The edge is below the ground.
    Cut,
    /// The edge is above the ground.
    Fill,
    /// The edge is on the ground within the tolerance; no transition.
    AtGrade,
}

/// One edge's classification.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EdgeClass {
    /// Cut, fill or at grade.
    pub classification: Classification,
    /// `z_edge - z_base(edge)`, metres.
    pub delta: f64,
}

/// Where a query point lies relative to the band.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Region {
    /// Inside the band.
    Band,
    /// In a side's transition reach.
    Transition {
        /// The side.
        side: Hand,
        /// That edge's classification at the query's cross-section.
        classification: Classification,
    },
    /// Where the band does not act.
    Base,
}

/// A design height at one plan position.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DesignSample {
    /// Design height, metres; the base height where the band does not act.
    pub height: f64,
    /// Where the query lies.
    pub region: Region,
}

/// Design refusals.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DesignError {
    /// Fewer than two stations, or two consecutive stations at one plan position.
    Stations,
    /// A non-finite or negative width, shoulder, limit, tolerance or position.
    Parameters,
    /// A face or crossfall slope is not a valid grade, or a crossfall is vertical.
    Slope,
    /// A band edge is off the base surface.
    Coverage,
    /// A daylight search refused.
    Daylight(DaylightError),
}

impl DesignError {
    /// Static diagnostic beginning with the stable refusal code.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Stations => {
                "DesignStations: a band needs two or more stations with distinct consecutive plan positions"
            }
            Self::Parameters => {
                "DesignParameters: positions, widths, shoulders, limits and tolerance must be finite and non-negative"
            }
            Self::Slope => "DesignSlope: a face or crossfall slope is not a valid grade",
            Self::Coverage => "DesignCoverage: a band edge is off the base surface",
            Self::Daylight(error) => error.message(),
        }
    }
}

/// Gradients resolved once per station.
#[derive(Clone, Copy, Debug)]
struct Grades {
    cut: f64,
    fill: f64,
    fall: Option<Fall>,
}

/// A resolved [`Camber`].
#[derive(Clone, Copy, Debug)]
enum Fall {
    OneWay(f64, Hand),
    Crown(f64),
}

/// A validated band over a base surface.
#[derive(Clone, Debug)]
pub struct Design<'a> {
    stations: &'a [Station],
    effect: Effect,
    field: &'a Field,
    tolerance: f64,
    grades: Vec<Grades>,
    /// Unit left normal per station (averaged tangent at interior stations).
    normals: Vec<[f64; 2]>,
    /// Centreline xyz for the shared nearest-point query.
    centreline: Vec<[f64; 3]>,
}

/// Interpolated cross-section at a centreline position. Pairs are
/// `[left, right]` except `grades`/`limits`, which are `[cut, fill]`.
#[derive(Clone, Copy, Debug)]
struct Section {
    centre: [f64; 3],
    /// Unit left normal of the segment.
    normal: [f64; 2],
    half: [f64; 2],
    shoulder: [f64; 2],
    grades: [f64; 2],
    limits: [f64; 2],
    rounding: f64,
    /// Twist at the left edge, the right edge and the clamped query offset.
    twist: [f64; 3],
}

fn twist(fall: Option<Fall>, lateral: f64) -> f64 {
    match fall {
        None => 0.0,
        Some(Fall::OneWay(gradient, Hand::Left)) => -gradient * lateral,
        Some(Fall::OneWay(gradient, Hand::Right)) => gradient * lateral,
        Some(Fall::Crown(gradient)) => -gradient * lateral.abs(),
    }
}

/// The rounded top of a face: the cubic Hermite over `[0, length]` from the edge
/// height leaving at grade `leaving` to the straight face (`edge + rise * length`)
/// arriving at its grade `rise`.
fn top(edge: f64, leaving: f64, rise: f64, length: f64, run: f64) -> f64 {
    let t = run / length;
    let (t2, t3) = (t * t, t * t * t);
    let h00 = 2.0f64.mul_add(t3, -3.0 * t2) + 1.0;
    let h10 = t3 - 2.0 * t2 + t;
    let h01 = (-2.0f64).mul_add(t3, 3.0 * t2);
    let h11 = t3 - t2;
    h00 * edge + h10 * leaving * length + h01 * (edge + rise * length) + h11 * rise * length
}

/// The face against the ground: a cut keeps the lower, a fill the higher; within
/// `blend` metres of height of the ground the face eases onto it (0 is sharp).
fn toe(base: f64, face: f64, cut: bool, blend: f64) -> f64 {
    let gap = if cut { base - face } else { face - base };
    if gap <= 0.0 {
        return base;
    }
    if blend <= 0.0 || blend.is_nan() || gap >= blend {
        return face;
    }
    let s = gap / blend;
    let eased = gap * s * s * 2.0f64.mul_add(-s, 3.0);
    if cut { base - eased } else { base + eased }
}

fn unit(x: f64, y: f64) -> Option<[f64; 2]> {
    let length = x.hypot(y);
    (length > plan::ON).then(|| [x / length, y / length])
}

fn lerp(a: f64, b: f64, t: f64) -> f64 {
    a + (b - a) * t
}

fn grades(station: &Station) -> Result<Grades, DesignError> {
    let numbers = [
        station.half_left,
        station.half_right,
        station.shoulder_left,
        station.shoulder_right,
        station.cut_limit,
        station.fill_limit,
        station.rounding,
    ];
    if station.position.iter().any(|v| !v.is_finite())
        || numbers.iter().any(|v| !(v.is_finite() && *v >= 0.0))
    {
        return Err(DesignError::Parameters);
    }
    let cut = station.cut.gradient().map_err(|_| DesignError::Slope)?;
    let fill = station.fill.gradient().map_err(|_| DesignError::Slope)?;
    let finite = |slope: Slope| match slope.gradient() {
        Ok(g) if g.is_finite() => Ok(g),
        _ => Err(DesignError::Slope),
    };
    let crossfall = match station.crossfall {
        None => None,
        Some(Camber::OneWay(oneway)) => Some(Fall::OneWay(finite(oneway.slope)?, oneway.falls_to)),
        Some(Camber::Crown(slope)) => Some(Fall::Crown(finite(slope)?)),
    };
    Ok(Grades {
        cut,
        fill,
        fall: crossfall,
    })
}

impl<'a> Design<'a> {
    /// Validate `band` over `field`; `tolerance` is the at-grade height band in
    /// metres.
    ///
    /// # Errors
    /// See [`DesignError`].
    pub fn new(band: &Band<'a>, field: &'a Field, tolerance: f64) -> Result<Self, DesignError> {
        let stations = band.stations;
        if !(tolerance.is_finite() && tolerance >= 0.0) {
            return Err(DesignError::Parameters);
        }
        let grades = stations.iter().map(grades).collect::<Result<Vec<_>, _>>()?;
        if stations.len() < 2 || u32::try_from(stations.len()).is_err() {
            return Err(DesignError::Stations);
        }
        let centreline: Vec<[f64; 3]> = stations.iter().map(|s| s.position).collect();
        if centreline
            .windows(2)
            .any(|pair| matches!(pair, [[ax, ay, _], [bx, by, _]] if (bx - ax).hypot(by - ay) <= plan::ON))
        {
            return Err(DesignError::Stations);
        }
        let last = centreline.len() - 1;
        let normals = (0..centreline.len())
            .map(|i| {
                let (Some(&[ax, ay, _]), Some(&[bx, by, _])) = (
                    centreline.get(i.saturating_sub(1)),
                    centreline.get(if i == last { i } else { i + 1 }),
                ) else {
                    return Err(DesignError::Stations);
                };
                // A reversing centreline has no tangent at the turn.
                unit(-(by - ay), bx - ax).ok_or(DesignError::Stations)
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Self {
            stations,
            effect: band.effect,
            field,
            tolerance,
            grades,
            normals,
            centreline,
        })
    }

    /// Unit left normal of station `index` (averaged tangent at interior
    /// stations); the left side's outward direction.
    #[must_use]
    pub fn normal(&self, index: usize) -> Option<[f64; 2]> {
        self.normals.get(index).copied()
    }

    /// Number of stations.
    #[must_use]
    pub const fn stations(&self) -> usize {
        self.stations.len()
    }

    /// Station `index`.
    #[must_use]
    pub fn station(&self, index: usize) -> Option<&Station> {
        self.stations.get(index)
    }

    /// Which way the band may move the surface.
    #[must_use]
    pub const fn effect(&self) -> Effect {
        self.effect
    }

    /// Whether any station has a nonzero crown, so the centreline is a crease.
    #[must_use]
    pub fn crowned(&self) -> bool {
        self.grades
            .iter()
            .any(|g| matches!(g.fall, Some(Fall::Crown(gradient)) if gradient != 0.0))
    }

    /// Left and right edge positions and design heights of station `index`.
    #[must_use]
    pub fn edges(&self, index: usize) -> Option<[[f64; 3]; 2]> {
        let station = self.stations.get(index)?;
        let [nx, ny] = *self.normals.get(index)?;
        let fall = self.grades.get(index)?.fall;
        let [x, y, z] = station.position;
        let (left, right) = (station.half_left, station.half_right);
        Some([
            [x + nx * left, y + ny * left, z + twist(fall, left)],
            [x - nx * right, y - ny * right, z + twist(fall, -right)],
        ])
    }

    fn classify_edge(&self, edge: [f64; 3]) -> Result<EdgeClass, DesignError> {
        let [x, y, z] = edge;
        let base = self
            .field
            .height_at(x, y)
            .ok_or(DesignError::Coverage)?
            .height;
        let delta = z - base;
        let classification = if delta < -self.tolerance {
            Classification::Cut
        } else if delta > self.tolerance {
            Classification::Fill
        } else {
            Classification::AtGrade
        };
        Ok(EdgeClass {
            classification,
            delta,
        })
    }

    /// `[left, right]` edge classification per station.
    ///
    /// # Errors
    /// [`DesignError::Coverage`] when an edge is off the base surface.
    pub fn classify(&self) -> Result<Vec<[EdgeClass; 2]>, DesignError> {
        (0..self.stations.len())
            .map(|index| {
                let [left, right] = self.edges(index).ok_or(DesignError::Stations)?;
                Ok([self.classify_edge(left)?, self.classify_edge(right)?])
            })
            .collect()
    }

    /// `[left, right]` daylight per station; `None` for an at-grade edge.
    ///
    /// # Errors
    /// As [`Design::classify`], or a refused daylight search.
    pub fn daylight(&self) -> Result<Vec<[Option<Locus>; 2]>, DesignError> {
        let classes = self.classify()?;
        let mut result = Vec::with_capacity(classes.len());
        for (index, (station, sides)) in self.stations.iter().zip(&classes).enumerate() {
            let edges = self.edges(index).ok_or(DesignError::Stations)?;
            let [nx, ny] = *self.normals.get(index).ok_or(DesignError::Stations)?;
            let side = |slot: usize, outward: [f64; 2], shoulder: f64| {
                let (Some(class), Some(&edge)) = (sides.get(slot), edges.get(slot)) else {
                    return Err(DesignError::Stations);
                };
                let (sense, slope, limit) = match class.classification {
                    Classification::AtGrade => return Ok(None),
                    Classification::Cut => (Sense::Cut, station.cut, station.cut_limit),
                    Classification::Fill => (Sense::Fill, station.fill, station.fill_limit),
                };
                let ray = Ray {
                    edge,
                    outward,
                    shoulder,
                };
                let face = Daylight {
                    slope,
                    extension: Some(limit),
                };
                daylight_roots(&ray, sense, face, self.field)
                    .map(Some)
                    .map_err(DesignError::Daylight)
            };
            result.push([
                side(0, [nx, ny], station.shoulder_left)?,
                side(1, [-nx, -ny], station.shoulder_right)?,
            ]);
        }
        Ok(result)
    }

    /// The cross-section through `(x, y)` and the query's lateral offset, or
    /// `None` past either end of the band.
    fn section(&self, x: f64, y: f64) -> Option<(Section, f64)> {
        let near = plan::nearest(&self.centreline, false, x, y)?;
        let index = near.segment;
        let (&[ax, ay, _], &[bx, by, _]) =
            (self.centreline.get(index)?, self.centreline.get(index + 1)?);
        let (dx, dy) = (bx - ax, by - ay);
        let along = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
        let last = self.centreline.len() - 2;
        if (index == 0 && along < 0.0) || (index == last && along > 1.0) {
            return None;
        }
        let share = along.clamp(0.0, 1.0);
        let (first, second) = (self.stations.get(index)?, self.stations.get(index + 1)?);
        let (from, to) = (self.grades.get(index)?, self.grades.get(index + 1)?);
        let mix = |p: f64, q: f64| lerp(p, q, share);
        // A face that is vertical at either station is vertical over the segment;
        // interpolating towards infinity would be meaningless.
        let grade = |p: f64, q: f64| {
            if p.is_infinite() || q.is_infinite() {
                f64::INFINITY
            } else {
                mix(p, q)
            }
        };
        let half = [
            mix(first.half_left, second.half_left),
            mix(first.half_right, second.half_right),
        ];
        let lateral = near.left;
        let clamped = lateral.clamp(-half[1], half[0]);
        let section = Section {
            centre: [
                mix(first.position[0], second.position[0]),
                mix(first.position[1], second.position[1]),
                mix(first.position[2], second.position[2]),
            ],
            normal: unit(-dy, dx)?,
            half,
            shoulder: [
                mix(first.shoulder_left, second.shoulder_left),
                mix(first.shoulder_right, second.shoulder_right),
            ],
            grades: [grade(from.cut, to.cut), grade(from.fill, to.fill)],
            limits: [
                mix(first.cut_limit, second.cut_limit),
                mix(first.fill_limit, second.fill_limit),
            ],
            rounding: mix(first.rounding, second.rounding),
            twist: [
                mix(twist(from.fall, half[0]), twist(to.fall, half[0])),
                mix(twist(from.fall, -half[1]), twist(to.fall, -half[1])),
                mix(twist(from.fall, clamped), twist(to.fall, clamped)),
            ],
        };
        Some((section, lateral))
    }

    /// Design height at `(x, y)`; `Ok(None)` off the base surface.
    ///
    /// # Errors
    /// [`DesignError::Coverage`] when the query's band edge is off the surface.
    pub fn height(&self, x: f64, y: f64) -> Result<Option<DesignSample>, DesignError> {
        let Some(base) = self.field.height_at(x, y).map(|sample| sample.height) else {
            return Ok(None);
        };
        let untouched = DesignSample {
            height: base,
            region: Region::Base,
        };
        let Some((section, lateral)) = self.section(x, y) else {
            return Ok(Some(untouched));
        };
        let [cx, cy, cz] = section.centre;
        let [nx, ny] = section.normal;
        let (slot, side, sign) = if lateral > section.half[0] {
            (0, Hand::Left, 1.0)
        } else if lateral < -section.half[1] {
            (1, Hand::Right, -1.0)
        } else {
            let design = cz + section.twist[2];
            return Ok(Some(self.restrict(base, design, Region::Band)));
        };
        let (Some(&half), Some(&shoulder), Some(&edge_twist)) = (
            section.half.get(slot),
            section.shoulder.get(slot),
            section.twist.get(slot),
        ) else {
            return Ok(Some(untouched));
        };
        let edge = [
            cx + nx * sign * half,
            cy + ny * sign * half,
            cz + edge_twist,
        ];
        let class = self.classify_edge(edge)?;
        let distance = lateral.abs() - half;
        let (kind, grade, limit) = match class.classification {
            Classification::AtGrade => return Ok(Some(untouched)),
            Classification::Cut => (0, section.grades[0], section.limits[0]),
            Classification::Fill => (1, section.grades[1], section.limits[1]),
        };
        if grade.is_infinite() || distance > limit {
            return Ok(Some(untouched));
        }
        let run = (distance - shoulder).max(0.0);
        // The face's rise per metre outward, and the band's own where the face leaves it.
        let rise = if kind == 0 { grade } else { -grade };
        let leaving = if shoulder > 0.0 || half <= 0.0 {
            0.0
        } else {
            edge_twist / half
        };
        let rounding = section.rounding;
        let cone = if rounding > 0.0 && run < rounding {
            top(edge[2], leaving, rise, rounding, run)
        } else {
            edge[2] + rise * run
        };
        let design = toe(base, cone, kind == 0, grade * rounding);
        let region = Region::Transition {
            side,
            classification: class.classification,
        };
        Ok(Some(self.restrict(base, design, region)))
    }

    /// Rounding length at station `index`, metres.
    #[must_use]
    pub fn rounding(&self, index: usize) -> f64 {
        self.stations.get(index).map_or(0.0, |s| s.rounding)
    }

    fn restrict(&self, base: f64, design: f64, region: Region) -> DesignSample {
        let height = match self.effect {
            Effect::Cut => base.min(design),
            Effect::Fill => base.max(design),
            Effect::Both => design,
        };
        DesignSample { height, region }
    }
}

#[cfg(test)]
mod tests;
