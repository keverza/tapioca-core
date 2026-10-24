//! Preparing a core before it is shared, and advancing it afterwards: the
//! session sync, the sample-to-mesh map (`SetSampleMeshes`), reuse from a
//! completed study (`ReuseUnaffectedSamples`) and the bounded advance.

use super::{ReuseContext, ReuseOutcome, StudyCore, StudyDomain, StudyError};
use crate::occlusion::Traversal;
use crate::reuse::{
    ReuseMesh, ReuseRequest, ReuseStudy, SampleMeshError, SpanOwner, patch_sample_meshes,
    plan_reuse,
};
use crate::session::{AdvanceOptions, StepObserver, StudyInputs};

impl StudyCore {
    fn domain_code(&self) -> u8 {
        u8::from(matches!(self.domain, StudyDomain::Patch(_)))
    }

    /// Point the session at the scene, the core's day and its sample count.
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn sync(&self, inputs: StudyInputs) -> Result<(), StudyError> {
        let count = self.samples().count;
        Ok(self.session.sync(inputs, &self.series, count)?)
    }

    /// The owning mesh index per sample. A triangle study takes its grid's
    /// groups; a patch study maps each span's element to its mesh, leaving the
    /// map unchanged for ambiguous identities and clearing it for a bad span
    /// (as C++); a ground study has none.
    pub fn set_sample_meshes(&mut self, meshes: &[ReuseMesh<'_>]) {
        match &self.domain {
            StudyDomain::Ground { .. } => self.sample_meshes.clear(),
            StudyDomain::Triangle(t) => self.sample_meshes.clone_from(&t.grid.groups),
            StudyDomain::Patch(p) => {
                let owners: Vec<SpanOwner<'_>> = p
                    .grid
                    .spans
                    .iter()
                    .map(|span| SpanOwner {
                        element: &span.key.element,
                        first: span.first,
                        count: span.count,
                    })
                    .collect();
                match patch_sample_meshes(meshes, self.samples().count, &owners) {
                    Ok(map) => self.sample_meshes = map,
                    Err(SampleMeshError::Ambiguous) => {}
                    Err(SampleMeshError::BadSpan) => self.sample_meshes.clear(),
                }
            }
        }
    }

    fn reuse_view<'a>(&'a self, meshes: &'a [ReuseMesh<'a>], roles: &'a [u8]) -> ReuseStudy<'a> {
        let samples = self.samples();
        ReuseStudy {
            meshes,
            roles,
            positions: samples.positions,
            normals: samples.normals.unwrap_or(&[]),
            sample_meshes: &self.sample_meshes,
            series: &self.series,
            grid_spacing: self.spacing,
            domain: self.domain_code(),
        }
    }

    /// Seed this unpublished core from a completed `source`: exact sample
    /// identity on unchanged elements, conservative shadow envelopes, then
    /// only the dirty steps are traced. Call after `sync` and
    /// `set_sample_meshes`, before the first advance. Returns `None` when
    /// reuse does not apply or the session refused the seed (nothing seeded).
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn reuse_from(
        &mut self,
        source: &Self,
        context: &ReuseContext<'_>,
    ) -> Result<Option<ReuseOutcome>, StudyError> {
        let request = ReuseRequest {
            source: source.reuse_view(context.source_meshes, context.source_roles),
            target: self.reuse_view(context.target_meshes, context.target_roles),
            source_converged: source.session.progress()?.converged,
            source_default_ray_bounds: context.source_default_ray_bounds,
            is_cancelled: context.is_cancelled,
        };
        let Some(plan) = plan_reuse(&request) else {
            return Ok(None);
        };
        if !self
            .session
            .seed_reusable(&source.session, &plan.map, &plan.dirty_steps)?
        {
            return Ok(None);
        }
        self.reuse = ReuseOutcome {
            reused_samples: plan.reused_samples,
            reused_sample_steps: plan.reused_sample_steps,
        };
        Ok(Some(self.reuse))
    }

    /// Resolve up to `options.max_steps` more steps over the core's own
    /// samples; returns how many (0 also when another advance holds it).
    ///
    /// # Errors
    /// [`StudyError::Poisoned`].
    pub fn advance(
        &self,
        traversal: &dyn Traversal,
        options: &AdvanceOptions<'_>,
        observer: Option<StepObserver<'_>>,
    ) -> Result<usize, StudyError> {
        Ok(self
            .session
            .advance(traversal, &self.samples(), options, observer)?)
    }
}
