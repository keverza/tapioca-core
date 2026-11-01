//! Plan-only clipping of paved TIN supports. Reuse the same constrained
//! triangulation as the region owner, rather than inventing another intersection
//! tolerance. Its ordered boundary paths supply all stitch subdivisions.

use std::collections::BTreeSet;

use super::{OutlineError, OutlineRequest, plan};
use crate::terrain::{Constraint, Request, Role, triangulate};

pub(super) struct Supports {
    pub ring: Vec<f64>,
    pub lines: Vec<Vec<f64>>,
    pub sources: Vec<[u32; 2]>,
}

pub(super) fn prepare(request: &OutlineRequest<'_>) -> Result<Supports, OutlineError> {
    let ring = crate::terrain::split::ring(request.region)
        .map_err(|e| OutlineError::Region(super::RegionError::Terrain(e)))?;
    let mut edges = BTreeSet::new();
    for &[a, b, c] in request.surface_triangles.as_chunks::<3>().0 {
        for (a, b) in [(a, b), (b, c), (c, a)] {
            edges.insert([a.min(b), a.max(b)]);
        }
    }
    let count = ring.len();
    let index = |i| u32::try_from(i).map_err(|_| OutlineError::Surface);
    let boundary = (0..count).map(index).collect::<Result<Vec<_>, _>>()?;
    let chains = edges
        .iter()
        .map(|&[a, b]| Ok([index(count + a as usize)?, index(count + b as usize)?]))
        .collect::<Result<Vec<_>, OutlineError>>()?;
    let mut points = Vec::with_capacity(request.region.len() + request.surface_points.len());
    // Heights must not participate in planar intersection validation.
    for &[x, y, _] in plan::vertices(request.region)
        .iter()
        .chain(plan::vertices(request.surface_points))
    {
        points.extend([x, y, 0.0]);
    }
    let mut constraints = vec![Constraint {
        role: Role::Boundary,
        vertices: &boundary,
    }];
    constraints.extend(chains.iter().map(|chain| Constraint {
        role: Role::Breakline,
        vertices: chain,
    }));
    let tin = triangulate(&Request {
        points: &points,
        constraints: &constraints,
        z_tolerance: 0.0,
        refinement: None,
    })
    .map_err(|e| OutlineError::Region(super::RegionError::Terrain(e)))?;
    let vertices = plan::vertices(&tin.points);
    let point = |v: u32| {
        vertices
            .get(v as usize)
            .copied()
            .ok_or(OutlineError::Surface)
    };
    let sources: Vec<_> = edges.into_iter().collect();
    let mut result = Supports {
        ring: Vec::new(),
        lines: Vec::new(),
        sources: Vec::new(),
    };
    for row in &tin.constraint_edges {
        if row.segment.constraint == 0 {
            result.ring.extend(point(row.vertices[0])?);
        } else if row.kept {
            let source = sources
                .get(row.segment.constraint as usize - 1)
                .ok_or(OutlineError::Surface)?;
            let [a, b] = row.vertices.map(point);
            result.lines.push(a?.into_iter().chain(b?).collect());
            result.sources.push(*source);
        }
    }
    Ok(result)
}
