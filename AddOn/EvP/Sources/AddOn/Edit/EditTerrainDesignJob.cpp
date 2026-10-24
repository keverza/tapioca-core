#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include <algorithm>
#include <cstdio>
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
        bool clipped = false, finer = false;
        const double limit = std::max (options.cutLimit, options.fillLimit);
        while (!outcome.built) {
            const auto& error = outcome.error;
            const auto is = [&error] (const char* code) { return error.rfind (code, 0) == 0; };
            char text[256];
            if (!clipped && options.region.empty () &&
                (is ("PropertyLineOutside") || is ("TerrainDesignRegion") || is ("RegionBreakline"))) {
                clipped = true;
                options.cover = TerrainFootprint (input.mesh);
                if (options.cover.empty ())
                    break;
                outcome.unmet.push_back ("The design area reaches past the captured terrain: it is built only "
                                         "where there is terrain.");
            }
            else if (is ("DaylightNotReached") && std::max (options.cutLimit, options.fillLimit) < kMaxSlopeReach) {
                // 4x at a time, at least 20 m, at most kMaxSlopeReach: a few retries, not a search.
                const double wider =
                    std::min (kMaxSlopeReach, std::max (std::max (options.cutLimit, options.fillLimit) * 4, 20.0));
                options.cutLimit = options.fillLimit = wider;
                std::snprintf (text, sizeof text,
                               "The side slopes do not meet the ground within %.1f m: built reaching up to %.1f m. "
                               "Steepen them or add a wall to keep them shorter.",
                               limit, options.cutLimit);
                if (!outcome.unmet.empty () && outcome.unmet.back ().rfind ("The side slopes", 0) == 0)
                    outcome.unmet.back () = text;
                else
                    outcome.unmet.push_back (text);
            }
            else if (!options.trimToCover && is ("DesignCoverage")) {
                options.trimToCover = true;
                if (options.cover.empty ())
                    options.cover = TerrainFootprint (input.mesh);
                outcome.unmet.push_back ("The path runs off the captured terrain: only the part over the terrain "
                                         "is graded.");
            }
            else if (!finer && is ("RegionBreakline") && options.maxStep > 0.5) {
                finer = true;
                options.maxStep = 0.5;
                outcome.unmet.push_back ("The path has too few stations inside the design area: sampled every "
                                         "0.5 m.");
            }
            else
                break;
            outcome.result = {};
            outcome.built = EvaluateTerrainDesign (*job.doc, input, options, outcome.result, outcome.error);
        }
        if (!outcome.built) {
            // What was relaxed did not build either: only the condition still in the way is listed.
            outcome.unmet = { TerrainConditionText (outcome.error) };
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

std::string TerrainConditionText (const std::string& error)
{
    static const std::pair<const char*, const char*> kConditions[] = {
        { "PropertyLineOutside", "The property line must be one closed outline without crossings, over the "
                                 "captured terrain." },
        { "TerrainDesignRegion", "The path's reach must leave one area inside the property line and the terrain." },
        { "RegionBreakline", "The path must run inside the terrain and the property line for at least two "
                             "stations." },
        { "DaylightNotReached", "A side slope does not meet the ground even 100 m out: steepen it or add a wall." },
        { "DesignCoverage", "The path must run over the captured terrain." },
        { "HybridVertical", "A side would need a vertical face: add a retaining wall there." },
        { "HybridDaylightTopology", "A side changes between cut and fill along the path in a way one slope cannot "
                                    "follow: split the path's cross-section ranges there." },
        { "LockedConflict", "A fixed height disagrees with the path or another fixed height." },
        { "TerrainCaptureStale", "The Archicad terrain changed: set it again." },
    };
    for (const auto& [code, text] : kConditions)
        if (error.rfind (code, 0) == 0)
            return text;
    return error;
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
