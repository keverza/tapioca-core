#include "ArchViz/VisibilityStudyDisplay.hpp"

#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"

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
    if (!result.valid || result.snapshot == nullptr || result.AtlasWidth () == 0 || result.AtlasHeight () == 0 ||
        result.values.size () != result.Count ()) {
        error = "visibility display needs a complete result and square-cell atlas";
        return nullptr;
    }
    std::vector<evp::sunstudy::AtlasTile> patchTiles;
    std::vector<evp::sunstudy::FaceLayout> patchLayouts;
    std::vector<float> image;
    if (result.IsPatchDomain ()) {
        if (!evp::sunstudy::PatchFaceArrays (result.patchGrid, result.patchAtlas, patchTiles, patchLayouts)) {
            error = "visibility display needs a complete packed patch atlas";
            return nullptr;
        }
        image.assign (result.patchAtlas.TexelCount (), -1.0f);
        for (size_t sample = 0; sample < result.Count (); ++sample) {
            if (sample % 4096 == 0 && cancelled ())
                return nullptr;
            const auto texel = result.patchAtlas.TexelOf (result.patchGrid, sample);
            if (texel < 0 || static_cast<size_t> (texel) >= image.size ()) {
                error = "visibility patch sample has no atlas texel";
                return nullptr;
            }
            image[static_cast<size_t> (texel)] = static_cast<float> (result.values[sample]);
        }
    }
    else
        image = evp::sunstudy::ScatterToAtlas (result.atlas, result.values, -1.0f);
    if (image.size () != static_cast<size_t> (result.AtlasWidth ()) * result.AtlasHeight ()) {
        error = "visibility values could not be scattered into their atlas";
        return nullptr;
    }

    auto upload = std::make_unique<SunStudyAtlasUpload> ();
    upload->studyId = result.id;
    upload->version = 1;
    upload->analysisKind = 1;
    upload->captureStamp = result.snapshot->captureStamp;
    upload->width = result.AtlasWidth ();
    upload->height = result.AtlasHeight ();
    upload->patchDomain = result.IsPatchDomain ();
    upload->texels = std::make_shared<const std::vector<float>> (std::move (image));
    upload->hoursMax = 1.0f;
    upload->debugMode = static_cast<uint32_t> (SunStudyDebugMode::Visibility);
    upload->depthMode = static_cast<uint32_t> (SunStudyDepthMode::Equal);
    upload->quantumHours = 0.01f;
    upload->sampleCount = result.Count ();
    upload->analysedArea = result.analysedArea;

    std::set<std::string> displayed;
    const auto& tiles = result.IsPatchDomain () ? patchTiles : result.atlas.tiles;
    const auto& layouts = result.IsPatchDomain () ? patchLayouts : result.grid.layouts;
    for (const auto& guid : result.displayElements)
        displayed.insert (evp::sunstudy::CanonicalGuid (guid));
    uint32_t faceBase = 0;
    for (const auto& mesh : result.snapshot->meshes) {
        if (cancelled ())
            return nullptr;
        if (displayed.find (evp::sunstudy::CanonicalGuid (mesh.guid)) != displayed.end ()) {
            SunStudyElementMap map;
            map.guid = mesh.guid;
            if (!BuildSunStudyElementMap (tiles, layouts, result.spacing, mesh.triangles, mesh.triMaterial, faceBase,
                                          map)) {
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
