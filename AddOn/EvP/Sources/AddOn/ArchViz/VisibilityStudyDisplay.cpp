#include "ArchViz/VisibilityStudyDisplay.hpp"

#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyRoles.hpp"

#include <set>

namespace geomsrv::archviz {

std::unique_ptr<SunStudyAtlasUpload> BuildVisibilityStudyUpload (const evp::sunstudy::VisibilityStudyResult& result,
                                                                 std::string& error,
                                                                 const std::function<bool ()>& isCancelled)
{
    const auto cancelled = [&] {
        if (isCancelled && isCancelled ()) {
            error = "visibility display cancelled";
            return true;
        }
        return false;
    };
    if (cancelled ())
        return nullptr;
    if (!result.valid || result.snapshot == nullptr || !result.atlas.valid ||
        result.values.size () != result.grid.Count ()) {
        error = "visibility display needs a complete result and square-cell atlas";
        return nullptr;
    }
    auto image = evp::sunstudy::ScatterToAtlas (result.atlas, result.values, -1.0f);
    if (image.size () != result.atlas.TexelCount ()) {
        error = "visibility values could not be scattered into their atlas";
        return nullptr;
    }

    auto upload = std::make_unique<SunStudyAtlasUpload> ();
    upload->studyId = result.id;
    upload->version = 1;
    upload->analysisKind = 1;
    upload->captureStamp = result.snapshot->captureStamp;
    upload->width = result.atlas.width;
    upload->height = result.atlas.height;
    upload->texels = std::make_shared<const std::vector<float>> (std::move (image));
    upload->hoursMax = 1.0f;
    upload->debugMode = static_cast<uint32_t> (SunStudyDebugMode::Visibility);
    upload->depthMode = static_cast<uint32_t> (SunStudyDepthMode::Equal);
    upload->quantumHours = 0.01f;
    upload->sampleCount = result.grid.Count ();
    upload->analysedArea = result.analysedArea;

    std::set<std::string> displayed;
    for (const auto& guid : result.displayElements)
        displayed.insert (evp::sunstudy::CanonicalGuid (guid));
    uint32_t faceBase = 0;
    for (const auto& mesh : result.snapshot->meshes) {
        if (cancelled ())
            return nullptr;
        if (displayed.find (evp::sunstudy::CanonicalGuid (mesh.guid)) != displayed.end ()) {
            SunStudyElementMap map;
            map.guid = mesh.guid;
            if (!BuildSunStudyElementMap (result.atlas.tiles, result.grid.layouts, result.spacing, mesh.triangles,
                                          mesh.triMaterial, faceBase, map)) {
                error = "visibility display refused an incomplete element map";
                return nullptr;
            }
            upload->elements.push_back (std::move (map));
        }
        faceBase += static_cast<uint32_t> (mesh.TriangleCount ());
    }
    if (upload->elements.empty ()) {
        error = "visibility display named no live elements";
        return nullptr;
    }
    return cancelled () ? nullptr : std::move (upload);
}

} // namespace geomsrv::archviz
