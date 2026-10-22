#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include <exception>

namespace tapioca::geometry {
namespace {

bool Describes (const geomsrv::edit::Document& doc, const TerrainDesignOutcome& outcome)
{
    return doc.landscape && doc.landscape->sessionUuid == outcome.session && doc.revision == outcome.revision &&
           doc.landscape->sourceTerrainGuid == outcome.sourceTerrainGuid;
}

} // namespace

TerrainDesignOutcome RunTerrainDesign (const TerrainDesignJob& job)
{
    TerrainDesignOutcome outcome;
    outcome.generation = job.generation;
    try {
        if (!job.doc || !job.doc->landscape || !job.capture) {
            outcome.error = "TerrainDesignJob: the job has no document snapshot or capture";
            return outcome;
        }
        outcome.revision = job.doc->revision;
        outcome.session = job.doc->landscape->sessionUuid;
        outcome.sourceTerrainGuid = job.doc->landscape->sourceTerrainGuid;
        auto options = job.options;
        options.region = job.region;
        outcome.capture = job.capture;
        if (job.surfaceTable != 0) {
            auto surface = std::make_shared<TerrainCapture> ();
            if (!PrepareTerrainSurface (job.capture, job.surfaceTable, *surface, outcome.error))
                return outcome;
            outcome.capture = std::move (surface);
        }
        const auto input = outcome.capture->Input ();
        outcome.built = EvaluateTerrainDesign (*job.doc, input, options, outcome.result, outcome.error);
        if (!outcome.built) {
            // Located places for the refusal; a refusal without a location keeps none.
            std::string ignored;
            if (!AssessTerrainDesign (*job.doc, input, options, outcome.conflicts, ignored))
                outcome.conflicts.clear ();
        }
    }
    catch (const std::exception& exception) {
        outcome.built = false;
        outcome.result = {};
        outcome.conflicts.clear ();
        outcome.error = std::string ("TerrainDesignJob: ") + exception.what ();
    }
    return outcome;
}

std::future<TerrainDesignOutcome> LaunchTerrainDesign (TerrainDesignJob job)
{
    // The job owns or shares-const everything it reads: nothing is captured by reference.
    return std::async (std::launch::async, [job = std::move (job)] { return RunTerrainDesign (job); });
}

bool TerrainDesignPublisher::Prepare (const geomsrv::edit::Document& doc, std::shared_ptr<const TerrainCapture> capture,
                                      const TerrainDesignOptions& options, std::span<const double> region,
                                      TerrainDesignJob& job, std::string& error)
{
    if (!doc.landscape || !capture || capture->revision != doc.revision ||
        capture->provenance.expectedSession != doc.landscape->sessionUuid ||
        capture->sourceTerrainGuid != doc.landscape->sourceTerrainGuid) {
        error = "ProvenanceStale: the terrain capture does not belong to this session, revision and terrain";
        return false;
    }
    TerrainDesignJob candidate;
    candidate.generation = latest + 1;
    candidate.doc = std::make_shared<const geomsrv::edit::Document> (doc);
    candidate.capture = std::move (capture);
    candidate.options = options;
    candidate.options.region = {};
    candidate.region.assign (region.begin (), region.end ());
    latest = candidate.generation;
    job = std::move (candidate);
    error.clear ();
    return true;
}

bool TerrainDesignPublisher::Publish (const geomsrv::edit::Document& current, TerrainDesignOutcome outcome,
                                      std::string& error)
{
    if (outcome.generation != latest) {
        error = "TerrainDesignSuperseded: a later design request replaced this one";
        return false;
    }
    if (!Describes (current, outcome)) {
        error = "ProvenanceStale: the document changed while the design was evaluated";
        return false;
    }
    published = std::move (outcome);
    error.clear ();
    return true;
}

const TerrainDesignOutcome* TerrainDesignPublisher::Current (const geomsrv::edit::Document& current) const
{
    return published && Describes (current, *published) ? &*published : nullptr;
}

} // namespace tapioca::geometry
