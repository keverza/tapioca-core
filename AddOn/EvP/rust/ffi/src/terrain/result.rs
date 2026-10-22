//! Lossless POD encoding of kernel support mappings; no identity allocation.
use super::{Failure, TerrainCounts, TerrainMapping, TerrainOutput, kernel};

pub(super) struct ResultData {
    pub(super) points: Vec<f64>,
    pub(super) triangles: Vec<u32>,
    pub(super) mappings: Vec<TerrainMapping>,
}

fn row(kind: u32, index: u32, values: [u32; 6]) -> TerrainMapping {
    TerrainMapping {
        kind,
        index,
        values,
    }
}
fn index(value: usize) -> Result<u32, Failure> {
    u32::try_from(value).map_err(|_| Failure::Argument("TerrainCountOverflow"))
}

pub(super) fn tin(tin: kernel::Tin) -> Result<ResultData, Failure> {
    let supports: std::collections::BTreeMap<_, _> = tin
        .constraint_edges
        .iter()
        .map(|row| row.segment)
        .collect::<std::collections::BTreeSet<_>>()
        .into_iter()
        .enumerate()
        .map(|(i, segment)| Ok((segment, index(i)?)))
        .collect::<Result<_, Failure>>()?;
    let support = |segment| {
        supports.get(&segment).copied().ok_or(Failure::State(
            "ProvenanceUnattributable: missing terrain segment",
        ))
    };
    let mut mappings = Vec::new();
    for (i, &vertex) in tin.input_vertices.iter().enumerate() {
        mappings.push(row(1, index(i)?, [vertex, 0, 0, 0, 0, 0]));
    }
    for (i, &boundary) in tin.face_boundary.iter().enumerate() {
        mappings.push(row(
            2,
            index(i)?,
            [boundary.unwrap_or(u32::MAX), 0, 0, 0, 0, 0],
        ));
    }
    for crossing in &tin.crossings {
        let [a, b] = crossing.segments;
        mappings.push(row(
            3,
            crossing.vertex,
            [
                a.constraint,
                a.segment,
                b.constraint,
                b.segment,
                support(a)?,
                support(b)?,
            ],
        ));
    }
    for edge in &tin.constraint_edges {
        mappings.push(row(
            4,
            edge.segment.constraint,
            [
                edge.segment.segment,
                edge.vertices[0],
                edge.vertices[1],
                u32::from(edge.kept),
                support(edge.segment)?,
                0,
            ],
        ));
    }
    for steiner in &tin.steiner {
        match &steiner.origin {
            kernel::SteinerOrigin::Segments(segments) => {
                for segment in segments {
                    mappings.push(row(
                        5,
                        steiner.vertex,
                        [
                            segment.constraint,
                            segment.segment,
                            support(*segment)?,
                            0,
                            0,
                            0,
                        ],
                    ));
                }
            }
            kernel::SteinerOrigin::Edge([a, b]) => {
                mappings.push(row(6, steiner.vertex, [*a, *b, 0, 0, 0, 0]));
            }
            kernel::SteinerOrigin::Face([a, b, c]) => {
                mappings.push(row(7, steiner.vertex, [*a, *b, *c, 0, 0, 0]));
            }
        }
    }
    Ok(ResultData {
        points: tin.points,
        triangles: tin.triangles,
        mappings,
    })
}

pub(super) fn split(mesh: kernel::SplitMesh, input_vertices: usize) -> Result<ResultData, Failure> {
    let mut mappings = Vec::new();
    for (i, (&parent, &inside)) in mesh.parents.iter().zip(&mesh.inside).enumerate() {
        mappings.push(row(8, index(i)?, [parent, u32::from(inside), 0, 0, 0, 0]));
    }
    for (i, vertex) in mesh.new_vertices.iter().enumerate() {
        let (origin, source) = match vertex.origin {
            kernel::Origin::LineVertex(n) => (1, n),
            kernel::Origin::Crossing(n) => (2, n),
        };
        let (host, a, b) = match vertex.host {
            kernel::Host::Edge([a, b]) => (1, a, b),
            kernel::Host::Face(n) => (2, n, 0),
        };
        mappings.push(row(
            9,
            index(
                input_vertices
                    .checked_add(i)
                    .ok_or(Failure::Argument("TerrainCountOverflow"))?,
            )?,
            [origin, source, host, a, b, 0],
        ));
    }
    for (i, &vertex) in mesh.line_vertices.iter().enumerate() {
        mappings.push(row(10, index(i)?, [vertex, 0, 0, 0, 0, 0]));
    }
    for edge in &mesh.line_edges {
        mappings.push(row(
            11,
            edge.segment,
            [edge.vertices[0], edge.vertices[1], 0, 0, 0, 0],
        ));
    }
    Ok(ResultData {
        points: mesh.points,
        triangles: mesh.triangles,
        mappings,
    })
}

// SAFETY: target full capacities/counts were preflighted disjoint from every
// input, descriptor and each other. All source vectors are live owned storage.
pub(super) unsafe fn publish(data: &ResultData, target: TerrainOutput) -> Result<(), Failure> {
    let counts = TerrainCounts {
        points: data.points.len(),
        triangles: data.triangles.len(),
        mappings: data.mappings.len(),
    };
    if target.query_only == 0
        && (target.point_capacity < counts.points
            || target.triangle_capacity < counts.triangles
            || target.mapping_capacity < counts.mappings)
    {
        return Err(Failure::Argument("TerrainOutputTooSmall"));
    }
    // SAFETY: capacity verified before ANY output write, including counts.
    // Copy primitive POD prefixes only; raw copies/writes cannot unwind.
    unsafe {
        if target.query_only == 0 {
            if !data.points.is_empty() {
                std::ptr::copy_nonoverlapping(
                    data.points.as_ptr(),
                    target.points,
                    data.points.len(),
                );
            }
            if !data.triangles.is_empty() {
                std::ptr::copy_nonoverlapping(
                    data.triangles.as_ptr(),
                    target.triangles,
                    data.triangles.len(),
                );
            }
            if !data.mappings.is_empty() {
                std::ptr::copy_nonoverlapping(
                    data.mappings.as_ptr(),
                    target.mappings,
                    data.mappings.len(),
                );
            }
        }
        target.counts.write(counts);
    }
    Ok(())
}
