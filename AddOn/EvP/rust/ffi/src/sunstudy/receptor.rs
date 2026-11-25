//! `tapioca_sunstudy_receptor_layer`: one planting height layer (#278).
//!
//! The receptor count equals the input sample count, so one call fills every
//! array; a zero capacity asks for the summary only. Host-measured overhead
//! distances, when given, mark receptors blocked in the same call.

use super::{Regions, flag};
use crate::borrow::SliceView;
use crate::boundary::{Failure, call};
use tapioca_sunstudy::receptor::{
    self, HostSamples, ReceptorArea, ReceptorLayer, ReceptorNormal, ReceptorOptions,
    ReceptorRefusal, ReceptorStatus,
};

/// Layer inputs. Empty `ceilings` means no overhead test.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReceptorLayerRequest {
    /// xyz per sample, as the sampler emitted them.
    pub positions: SliceView<f64>,
    /// xyz unit host normals per sample.
    pub normals: SliceView<f64>,
    /// m^2 per sample.
    pub areas: SliceView<f64>,
    /// Per sample, overhead distance from `host + OVERHEAD_START` straight up
    /// (negative for none), or empty.
    pub ceilings: SliceView<f64>,
    /// The sampler's normal offset, metres.
    pub normal_offset: f64,
    /// Receptor height above the host, metres.
    pub height: f64,
    /// Least host-normal Z for an `up` receptor.
    pub min_host_up: f64,
    /// 0 surface normal, 1 up.
    pub normal: u8,
    /// 0 surface area, 1 plan area.
    pub area: u8,
}

/// Counts and areas; written by every successful call.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ReceptorLayerSummary {
    /// Receptors (the input sample count).
    pub count: usize,
    /// Traceable receptors.
    pub available: usize,
    /// Hosts too steep for an `up` receptor.
    pub steep_host: usize,
    /// Blocked overhead.
    pub blocked: usize,
    /// Non-finite host samples.
    pub invalid_host: usize,
    /// Summed weight of available receptors, m^2.
    pub available_area: f64,
    /// Summed weight of the rest, m^2.
    pub unavailable_area: f64,
}

/// Caller-owned outputs. With `capacity == 0` only the summary is written;
/// otherwise every array holds `count` receptors (xyz arrays three each).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReceptorLayerOutput {
    /// xyz host points.
    pub hosts: *mut f64,
    /// xyz receptor positions.
    pub positions: *mut f64,
    /// xyz receptor normals.
    pub normals: *mut f64,
    /// One weight per receptor.
    pub areas: *mut f64,
    /// One `ReceptorStatus` byte per receptor.
    pub status: *mut u8,
    /// Capacity in receptors.
    pub capacity: usize,
    /// Required summary record.
    pub summary: *mut ReceptorLayerSummary,
}

fn options(request: &ReceptorLayerRequest) -> Result<ReceptorOptions, Failure> {
    let up = flag(request.normal, "ReceptorNormal: expected 0 or 1")?;
    let plan = flag(request.area, "ReceptorArea: expected 0 or 1")?;
    Ok(ReceptorOptions {
        height: request.height,
        normal: if up {
            ReceptorNormal::Up
        } else {
            ReceptorNormal::Surface
        },
        area: if plan {
            ReceptorArea::Plan
        } else {
            ReceptorArea::Surface
        },
        min_host_up: request.min_host_up,
    })
}

fn refusal(reason: ReceptorRefusal) -> Failure {
    match reason {
        ReceptorRefusal::Shape => Failure::Argument("ReceptorLayerShape"),
        ReceptorRefusal::Options => Failure::Argument("ReceptorLayerOptions"),
    }
}

fn summarize(layer: &ReceptorLayer) -> ReceptorLayerSummary {
    let (available_area, unavailable_area) = layer.area_split();
    let count = |wanted: ReceptorStatus| layer.status.iter().filter(|s| **s == wanted).count();
    ReceptorLayerSummary {
        count: layer.count(),
        available: count(ReceptorStatus::Available),
        steep_host: count(ReceptorStatus::SteepHost),
        blocked: count(ReceptorStatus::Blocked),
        invalid_host: count(ReceptorStatus::InvalidHost),
        available_area,
        unavailable_area,
    }
}

// SAFETY: destination holds at least `source.len()` elements, checked disjoint.
unsafe fn publish<T: Copy>(source: &[T], destination: *mut T) {
    if !source.is_empty() {
        // SAFETY: forwarded from the caller.
        unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), destination, source.len()) };
    }
}

/// Build one receptor layer from a sampled grid.
///
/// # Safety
/// The request, its arrays and the output descriptor must be live, initialized
/// and immutable. Every output array at its declared capacity and the summary
/// must be live, aligned, exclusively writable and disjoint from every other
/// region and the error buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tapioca_sunstudy_receptor_layer(
    request: *const ReceptorLayerRequest,
    output: *const ReceptorLayerOutput,
    error: *mut u8,
    error_capacity: usize,
) -> i32 {
    // SAFETY: envelope contains unwinding; every region is checked before use.
    unsafe {
        call(error, error_capacity, || {
            let mut regions = Regions::new(error, error_capacity)?;
            regions.input(request, 1);
            regions.input(output, 1);
            regions.check()?;
            let (request, target) = (
                regions.context().record(request)?,
                regions.context().record(output)?,
            );
            regions.input(request.positions.data, request.positions.count);
            regions.input(request.normals.data, request.normals.count);
            regions.input(request.areas.data, request.areas.count);
            regions.input(request.ceilings.data, request.ceilings.count);
            let capacity = target.capacity;
            let scalars = capacity
                .checked_mul(3)
                .ok_or(Failure::Argument("ReceptorLayerCapacity"))?;
            regions.output(target.hosts, scalars);
            regions.output(target.positions, scalars);
            regions.output(target.normals, scalars);
            regions.output(target.areas, capacity);
            regions.output(target.status, capacity);
            regions.output(target.summary, 1);
            regions.check()?;
            let context = regions.context();
            let settings = options(&request)?;
            let samples = HostSamples {
                positions: context.slice(request.positions)?,
                normals: context.slice(request.normals)?,
                areas: context.slice(request.areas)?,
                normal_offset: request.normal_offset,
            };
            let mut layer = receptor::build_receptor_layer(&samples, &settings).map_err(refusal)?;
            let ceilings = context.slice(request.ceilings)?;
            if !ceilings.is_empty() {
                receptor::mark_overhead(&mut layer, ceilings, settings.height).map_err(refusal)?;
            }
            if capacity != 0 {
                if capacity < layer.count() {
                    return Err(Failure::Argument("ReceptorLayerOutputTooSmall"));
                }
                publish(&layer.hosts, target.hosts);
                publish(&layer.positions, target.positions);
                publish(&layer.normals, target.normals);
                publish(&layer.areas, target.areas);
                let status: Vec<u8> = layer.status.iter().map(|s| *s as u8).collect();
                publish(&status, target.status);
            }
            target.summary.write(summarize(&layer));
            Ok(())
        })
    }
}
