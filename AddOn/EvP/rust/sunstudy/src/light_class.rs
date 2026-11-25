//! Area-weighted sunlight classes with an exact comparator (#278).
//!
//! A planting sunlight class is a band of direct-sun hours: with edges
//! `[3, 6]`, `H < 3` is shade, `3 <= H < 6` partial shade and `H >= 6` full
//! sun. A sample's class is the number of edges it reaches (`H >= e`), so an
//! exact boundary value always belongs to the upper class.
//!
//! ⚠️ THIS IS NOT THE DISPLAY BAND. The tint shader widens each band by a
//! small slack so a value a rounding error short of an edge still draws in
//! it; that is a picture, not a classification. Class areas, inspector labels
//! and exports come from here, from the raw hours.
//!
//! ⚠️ AREA, NOT COUNTS. Samples carry different weights (a triangle's cells
//! split its area; a planting layer carries plan area), so a class share is
//! summed weight over summed weight, never a sample fraction.

/// Most edges accepted: a band table, not a histogram.
pub const MAX_EDGES: usize = 16;

/// Why a classification was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClassRefusal {
    /// `hours` and `areas` differ in length.
    Shape,
    /// No edges, too many, a non-finite edge, or edges not strictly ascending.
    Edges,
}

/// Totals per class; `edges.len() + 1` classes, lowest first.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ClassAreas {
    /// Summed weight per class.
    pub areas: Vec<f64>,
    /// Samples per class.
    pub counts: Vec<usize>,
    /// Summed weight of classified samples.
    pub evaluated_area: f64,
    /// Samples whose hours or weight were not finite (or weight negative):
    /// left out of every class and of `evaluated_area`.
    pub unclassified: usize,
}

fn check_edges(edges: &[f64]) -> Result<(), ClassRefusal> {
    let ascending = edges.windows(2).all(|pair| match pair {
        [a, b] => a < b,
        _ => true,
    });
    if edges.is_empty()
        || edges.len() > MAX_EDGES
        || !edges.iter().all(|e| e.is_finite())
        || !ascending
    {
        return Err(ClassRefusal::Edges);
    }
    Ok(())
}

/// The class of one value: the number of edges it reaches. `edges` must be
/// valid (finite, strictly ascending).
#[must_use]
pub fn class_of(hours: f64, edges: &[f64]) -> usize {
    edges.partition_point(|edge| hours >= *edge)
}

/// Area-weighted totals per class.
///
/// # Errors
/// [`ClassRefusal`] for mismatched arrays or invalid edges; nothing partial.
pub fn class_areas(
    hours: &[f64],
    areas: &[f64],
    edges: &[f64],
) -> Result<ClassAreas, ClassRefusal> {
    if hours.len() != areas.len() {
        return Err(ClassRefusal::Shape);
    }
    check_edges(edges)?;
    let classes = edges.len() + 1;
    let mut out = ClassAreas {
        areas: vec![0.0; classes],
        counts: vec![0; classes],
        ..ClassAreas::default()
    };
    for (h, a) in hours.iter().zip(areas) {
        if !h.is_finite() || !a.is_finite() || *a < 0.0 {
            out.unclassified += 1;
            continue;
        }
        let class = class_of(*h, edges);
        if let (Some(area), Some(count)) = (out.areas.get_mut(class), out.counts.get_mut(class)) {
            *area += a;
            *count += 1;
            out.evaluated_area += a;
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests;
