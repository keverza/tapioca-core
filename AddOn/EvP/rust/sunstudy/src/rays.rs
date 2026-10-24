//! Arbitrary-ray queries the visibility and viewpoint studies ask of the
//! host's scene index: first hits (with the mesh hit, optionally restricted
//! to a mesh mask) and any-hit occlusion. Like [`crate::occlusion::Traversal`]
//! the index stays with the host; the studies own only the policy.

/// One first-hit query: from `origin` along unit `dir`, up to `length`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FirstHitRay {
    /// Start, world metres.
    pub origin: [f64; 3],
    /// Unit direction.
    pub dir: [f64; 3],
    /// Farthest distance, metres.
    pub length: f64,
}

/// One first-hit answer.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RayHit {
    /// Whether anything was hit.
    pub hit: bool,
    /// Distance to the hit.
    pub t: f64,
    /// Snapshot mesh index of the hit.
    pub mesh: usize,
}

/// One any-hit occlusion query (`ITraversal::OcclusionRay`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OcclusionRay {
    /// Start.
    pub origin: [f64; 3],
    /// Unit direction.
    pub dir: [f64; 3],
    /// Near distance.
    pub tmin: f64,
    /// Far distance; <= 0 unbounded.
    pub tmax: f64,
}

/// Why a batch did not complete.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RayError {
    /// Cancelled between rays; the outputs are not usable.
    Cancelled,
    /// The host index failed or could not be acquired.
    Failed,
}

/// The host's scene index. Implementations are safe for concurrent calls.
pub trait RayQueries {
    /// Acquire the index; called where the C++ study acquires it, so the
    /// refusal order matches.
    ///
    /// # Errors
    /// [`RayError::Failed`] when the index is unavailable.
    fn prepare(&self) -> Result<(), RayError>;

    /// First hits; `mask` (one byte per snapshot mesh, nonzero = hittable)
    /// restricts the hittable meshes, `None` is the whole model.
    ///
    /// # Errors
    /// [`RayError`] when cancelled or failed.
    fn first_hits(
        &self,
        rays: &[FirstHitRay],
        mask: Option<&[u8]>,
        out: &mut [RayHit],
        max_parallel: usize,
    ) -> Result<(), RayError>;

    /// Any-hit occlusion over the whole model: 1 blocked, 0 clear.
    ///
    /// # Errors
    /// [`RayError`] when cancelled or failed.
    fn occlude_rays(
        &self,
        rays: &[OcclusionRay],
        out: &mut [u8],
        max_parallel: usize,
    ) -> Result<(), RayError>;

    /// Polled between batches.
    fn cancelled(&self) -> bool;
}
