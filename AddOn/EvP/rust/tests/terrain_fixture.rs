//! Test/measurement-only parser for the shared neutral landscape fixture corpus.
use tapioca_geometry::terrain::{Constraint, Refinement, Request, Role};

pub(crate) const CORPUS: &str = include_str!("fixtures/landscape.terrain");

pub(crate) struct Fixture {
    pub(crate) name: String,
    pub(crate) area: f64,
    pub(crate) inside_area: f64,
    pub(crate) crossings: usize,
    pub(crate) max_area: f64,
    pub(crate) points: Vec<f64>,
    pub(crate) constraints: Vec<(Role, Vec<u32>)>,
    pub(crate) line: Vec<f64>,
}

impl Fixture {
    pub(crate) fn constraints(&self) -> Vec<Constraint<'_>> {
        self.constraints
            .iter()
            .map(|(role, vertices)| Constraint {
                role: *role,
                vertices,
            })
            .collect()
    }
    pub(crate) fn request<'a>(&'a self, constraints: &'a [Constraint<'a>]) -> Request<'a> {
        Request {
            points: &self.points,
            constraints,
            z_tolerance: 1e-8,
            refinement: (self.max_area > 0.0).then_some(Refinement {
                min_angle_degrees: 0.0,
                max_area_square_metres: Some(self.max_area),
                max_added_vertices: 20_000,
            }),
        }
    }
}

fn token<'a>(tokens: &mut impl Iterator<Item = &'a str>) -> Result<&'a str, String> {
    tokens
        .next()
        .ok_or_else(|| "truncated terrain fixture".to_owned())
}
fn number<'a, T: std::str::FromStr>(
    tokens: &mut impl Iterator<Item = &'a str>,
) -> Result<T, String> {
    token(tokens)?
        .parse()
        .map_err(|_| "invalid terrain fixture number".to_owned())
}
fn keyword<'a>(tokens: &mut impl Iterator<Item = &'a str>, wanted: &str) -> Result<(), String> {
    if token(tokens)? == wanted {
        Ok(())
    } else {
        Err(format!("expected {wanted}"))
    }
}
fn points<'a>(tokens: &mut impl Iterator<Item = &'a str>) -> Result<Vec<f64>, String> {
    let count: u32 = number(tokens)?;
    if count > 100_000 {
        return Err("fixture point limit".to_owned());
    }
    let mut result = Vec::new();
    for _ in 0..count {
        result.extend([
            number::<f64>(tokens)? + 500_000.0,
            number::<f64>(tokens)? + 6_000_000.0,
            number(tokens)?,
        ]);
    }
    Ok(result)
}

pub(crate) fn corpus() -> Result<Vec<Fixture>, String> {
    let mut tokens = CORPUS
        .lines()
        .filter(|line| !line.trim_start().starts_with('#'))
        .flat_map(str::split_whitespace);
    let mut result = Vec::new();
    while let Some(start) = tokens.next() {
        if start != "case" {
            return Err("expected case".to_owned());
        }
        let name = token(&mut tokens)?.to_owned();
        let (area, inside_area, crossings, max_area) = (
            number(&mut tokens)?,
            number(&mut tokens)?,
            number(&mut tokens)?,
            number(&mut tokens)?,
        );
        keyword(&mut tokens, "points")?;
        let positions = points(&mut tokens)?;
        keyword(&mut tokens, "constraints")?;
        let count: u32 = number(&mut tokens)?;
        let mut constraints = Vec::new();
        for _ in 0..count {
            let role = match number::<u32>(&mut tokens)? {
                1 => Role::Boundary,
                2 => Role::Hole,
                3 => Role::Breakline,
                _ => return Err("invalid role".to_owned()),
            };
            let count: u32 = number(&mut tokens)?;
            let vertices = (0..count)
                .map(|_| number(&mut tokens))
                .collect::<Result<_, _>>()?;
            constraints.push((role, vertices));
        }
        keyword(&mut tokens, "line")?;
        let line = points(&mut tokens)?;
        keyword(&mut tokens, "end")?;
        result.push(Fixture {
            name,
            area,
            inside_area,
            crossings,
            max_area,
            points: positions,
            constraints,
            line,
        });
    }
    Ok(result)
}

/// Deterministic 100x100 survey grid, 500 two-point breakline polylines.
/// z=.1*x+.05*y; all constraints are existing grid edges, no crossing explosion.
pub(crate) fn workload() -> Fixture {
    let mut points = Vec::new();
    for y in 0..100_u32 {
        for x in 0..100_u32 {
            points.extend([
                500_000.0 + f64::from(x),
                6_000_000.0 + f64::from(y),
                f64::from(x) * 0.1 + f64::from(y) * 0.05,
            ]);
        }
    }
    let constraints = (0..500_u32)
        .map(|n| {
            let a = (n / 99 * 17) * 100 + n % 99;
            (Role::Breakline, vec![a, a + 1])
        })
        .collect();
    Fixture {
        name: "grid_100x100_500".to_owned(),
        area: 9801.0,
        inside_area: 0.0,
        crossings: 0,
        max_area: 0.0,
        points,
        constraints,
        line: Vec::new(),
    }
}

pub(crate) fn digest(points: &[f64], triangles: &[u32]) -> u64 {
    points
        .iter()
        .flat_map(|value| value.to_bits().to_le_bytes())
        .chain(triangles.iter().flat_map(|value| value.to_le_bytes()))
        .fold(14_695_981_039_346_656_037, |hash, byte| {
            (hash ^ u64::from(byte)).wrapping_mul(1_099_511_628_211)
        })
}
