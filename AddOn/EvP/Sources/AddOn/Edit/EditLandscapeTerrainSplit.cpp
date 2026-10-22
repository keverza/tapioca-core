#include "Edit/EditLandscapeTerrain.hpp"
#include <exception>

namespace tapioca::geometry {

bool EvaluateSplitLandscapeTerrain (const geomsrv::edit::Document& doc, const LandscapeTerrainRequest& request,
                                    const EditTerrainSplitOptions& options, SplitLandscapeTerrainResult& result,
                                    std::string& error)
{
    try {
        SplitLandscapeTerrainResult candidate;
        if (!SplitEditTerrain (doc, request.input, options, candidate.baseline, error))
            return false;
        const auto& split = candidate.baseline;
        const EditTerrainInput input {
            { split.split.points, split.split.triangles }, split.meshProvenance, split.revision, split.sourceTerrainGuid
        };
        LandscapeTerrainRequest prepared { input };
        prepared.edits = request.edits;
        prepared.surfaces = request.surfaces;
        prepared.smoothing = request.smoothing;
        prepared.queriesXY = request.queriesXY;
        prepared.sampleStep = request.sampleStep;
        prepared.volumes = request.volumes;
        if (!EvaluateLandscapeTerrain (doc, prepared, candidate.evaluation, error) ||
            !VerifyEditTerrainOutside (candidate.baseline, candidate.evaluation.terrain, candidate.preservation, error))
            return false;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainSplitRefused: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
