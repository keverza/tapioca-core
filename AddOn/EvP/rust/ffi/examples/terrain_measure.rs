//! Opt-in S3a.3 pure-kernel measurement, never linked into the shipped archive.
#![deny(unsafe_code)]
#[path = "../../tests/terrain_fixture.rs"]
mod fixture;

// Only this standalone process installs a probe; production crates/ABI unchanged.
#[allow(
    unsafe_code,
    reason = "Standalone System allocator delegation and requested-byte accounting"
)]
mod allocation {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
    pub(super) struct Probe;
    static ACTIVE: AtomicBool = AtomicBool::new(false);
    static CALLS: AtomicUsize = AtomicUsize::new(0);
    static BYTES: AtomicUsize = AtomicUsize::new(0);
    static LIVE: AtomicUsize = AtomicUsize::new(0);
    static PEAK: AtomicUsize = AtomicUsize::new(0);
    fn add(bytes: usize) {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(bytes, Relaxed);
        let live = LIVE.fetch_add(bytes, Relaxed) + bytes;
        PEAK.fetch_max(live, Relaxed);
    }
    // SAFETY: every pointer/layout is delegated unchanged to System. Atomics
    // allocate nothing. The single worker enables counting only around work
    // whose allocations are created AND destroyed within that same interval.
    unsafe impl GlobalAlloc for Probe {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            // SAFETY: caller supplies valid layout, System owns the allocation.
            let pointer = unsafe { System.alloc(layout) };
            if !pointer.is_null() && ACTIVE.load(Relaxed) {
                add(layout.size());
            }
            pointer
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            // SAFETY: same delegated allocation contract, requesting zeroed bytes.
            let pointer = unsafe { System.alloc_zeroed(layout) };
            if !pointer.is_null() && ACTIVE.load(Relaxed) {
                add(layout.size());
            }
            pointer
        }
        unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
            if ACTIVE.load(Relaxed) {
                LIVE.fetch_sub(layout.size(), Relaxed);
            }
            // SAFETY: pointer/layout are precisely the original System allocation.
            unsafe {
                System.dealloc(pointer, layout);
            }
        }
        unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
            // SAFETY: valid System pointer/layout and nonzero requested size.
            let result = unsafe { System.realloc(pointer, layout, size) };
            if !result.is_null() && ACTIVE.load(Relaxed) {
                LIVE.fetch_sub(layout.size(), Relaxed);
                add(size);
            }
            result
        }
    }
    pub(super) fn start() {
        CALLS.store(0, Relaxed);
        BYTES.store(0, Relaxed);
        LIVE.store(0, Relaxed);
        PEAK.store(0, Relaxed);
        ACTIVE.store(true, Relaxed);
    }
    pub(super) fn stop() -> [usize; 4] {
        ACTIVE.store(false, Relaxed);
        [
            CALLS.load(Relaxed),
            BYTES.load(Relaxed),
            PEAK.load(Relaxed),
            LIVE.load(Relaxed),
        ]
    }
}

#[global_allocator]
static ALLOCATOR: allocation::Probe = allocation::Probe;

fn measure(input: &fixture::Fixture, iterations: usize) -> Result<(), String> {
    use std::{hint::black_box, time::Instant};
    use tapioca_geometry::terrain;
    let constraints = input.constraints();
    let request = input.request(&constraints);
    let expected = terrain::triangulate(&request).map_err(terrain::Error::message)?;
    terrain::verify_constraints(&request, &expected).map_err(terrain::Error::message)?;
    let hash = fixture::digest(&expected.points, &expected.triangles);
    for _ in 0..3 {
        black_box(terrain::triangulate(&request).map_err(terrain::Error::message)?);
    }
    let mut samples = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        let result = terrain::triangulate(black_box(&request)).map_err(terrain::Error::message)?;
        let ms = start.elapsed().as_secs_f64() * 1000.0;
        if fixture::digest(&result.points, &result.triangles) != hash {
            return Err("nondeterministic result".into());
        }
        samples.push(ms);
        black_box(result);
    }
    // Separate allocation pass: construction and final destruction, excluding
    // fixture, timing vector and correctness comparisons. No concurrent workers.
    allocation::start();
    let result = terrain::triangulate(&request);
    let success = result.is_ok();
    drop(result);
    let [calls, bytes, peak, live] = allocation::stop();
    if !success || live != 0 {
        return Err("allocator interval not balanced".into());
    }
    samples.sort_by(f64::total_cmp);
    let median = samples.get(iterations / 2).ok_or("no median")?;
    let p95 = samples
        .get((iterations * 95).div_ceil(100) - 1)
        .ok_or("no p95")?;
    println!(
        "{{\"lane\":\"kernel\",\"fixture\":\"{}\",\"vertices\":{},\"triangles\":{},\"constraints\":{},\"crossings\":{},\"line_vertices\":{},\"expected_area\":{},\"expected_inside_area\":{},\"required_crossings\":{},\"hash\":\"{hash:016x}\",\"iterations\":{iterations},\"median_ms\":{median},\"p95_ms\":{p95},\"rust_alloc_calls\":{calls},\"rust_requested_bytes\":{bytes},\"rust_peak_live_requested_bytes\":{peak},\"copy_bytes\":null}}",
        input.name,
        expected.points.len() / 3,
        expected.triangles.len() / 3,
        constraints.len(),
        expected.crossings.len(),
        input.line.len() / 3,
        input.area,
        input.inside_area,
        input.crossings
    );
    println!(
        "{{\"samples_for\":\"{}\",\"samples_ms\":{samples:?}}}",
        input.name
    );
    Ok(())
}

fn main() -> Result<(), String> {
    if cfg!(debug_assertions) {
        return Err("release measurement build required".into());
    }
    let iterations = std::env::args().nth(1).map_or(Ok(21), |s| {
        s.parse::<usize>().map_err(|_| "invalid iterations")
    })?;
    if !(3..=1000).contains(&iterations) {
        return Err("iterations must be 3..1000".into());
    }
    // Independent allocator sanity check before measuring any algorithm.
    allocation::start();
    let bytes = std::hint::black_box(vec![1_u8; 4096]);
    drop(bytes);
    if allocation::stop() != [1, 4096, 4096, 0] {
        return Err("allocator sanity failure".into());
    }
    let selected = std::env::args().nth(2);
    let mut found = false;
    for input in fixture::corpus()?
        .iter()
        .chain(std::iter::once(&fixture::workload()))
    {
        if selected.as_ref().is_none_or(|name| name == &input.name) {
            measure(input, iterations)?;
            found = true;
        }
    }
    if !found {
        return Err("unknown fixture".into());
    }
    Ok(())
}
