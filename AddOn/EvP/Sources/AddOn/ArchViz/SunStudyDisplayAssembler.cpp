#include "ArchViz/SunStudyDisplayAssembler.hpp"
#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <algorithm>

namespace geomsrv::archviz {

std::unique_ptr<SunStudyAtlasUpload> SunStudyDisplayAssembler::Prepare (const evp::sunstudy::StudyRecord& record,
                                                                        const Snapshot& snapshot,
                                                                        const SunStudyDisplayOptions& options,
                                                                        std::string& error,
                                                                        const std::function<bool ()>& isCancelled)
{
    const auto cancelled = [&] { return record.cancelRequested.load () || (isCancelled && isCancelled ()); };
    const auto progress = record.session.Progress ();
    if (record.snapshotId != snapshot.id || (options.preview && !progress.converged)) {
        error = "sun study display source is stale or the preview has not completed its day";
        return nullptr;
    }
    const auto data = evp::sunstudy::PrepareStudyDisplayData (record, error, cancelled);
    if (data == nullptr)
        return nullptr;
    const auto& tiles = record.IsPatchDomain () ? data->patchTiles : record.atlas.tiles;
    const auto& layouts = record.IsPatchDomain () ? data->patchLayouts : record.sampleGrid.layouts;
    if (snapshot.TotalTriangles () != tiles.size ()) {
        error = "sun study face mapping does not match its captured snapshot";
        return nullptr;
    }
    auto upload = std::make_unique<SunStudyAtlasUpload> ();
    upload->studyId = record.id;
    upload->version = progress.generation;
    upload->width = data->width;
    upload->height = data->height;
    upload->texels = data->image;
    upload->stepMasks = data->stepMasks;
    upload->stepWords = data->stepWords;
    upload->stepCount = data->stepCount;
    upload->stepMinutes = data->stepMinutes;
    upload->noonStep = data->noonStep;
    const double rampTop = options.hoursMax > 0.0 ? options.hoursMax : record.series.DaylightHours ();
    upload->hoursMax = static_cast<float> (rampTop > 0.0 ? rampTop : 1.0);
    upload->debugMode = std::min (options.debug, static_cast<uint32_t> (SunStudyDebugMode::ShadowFan));
    upload->depthMode = std::min (options.depth, static_cast<uint32_t> (SunStudyDepthMode::Always));
    upload->quantumHours = static_cast<float> (record.timestepMinutes) / 60.0f;
    upload->preview = options.preview;
    upload->sampleCount = record.positions.size () / 3;
    upload->analysedArea = data->area;
    upload->patchDomain = record.IsPatchDomain ();
    const bool roles = upload->debugMode == static_cast<uint32_t> (SunStudyDebugMode::Roles);
    if (roles && record.elementRoles.size () != snapshot.meshes.size ()) {
        error = "sun study carries no element roles for this snapshot";
        return nullptr;
    }
    std::lock_guard<std::mutex> lock (mutex_);
    if (source_.lock () != data) {
        source_ = data;
        maps_[0].reset ();
        maps_[1].reset ();
    }
    upload->sharedElements = maps_[roles ? 1 : 0].lock ();
    if (upload->sharedElements == nullptr) {
        auto maps = std::make_shared<std::vector<SunStudyElementMap>> ();
        maps->reserve (snapshot.meshes.size ());
        uint32_t faceBase = 0;
        for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
            if (cancelled ())
                return nullptr;
            const auto& mesh = snapshot.meshes[m];
            SunStudyElementMap map;
            map.guid = mesh.guid;
            const bool valid =
                roles ? BuildSunStudyRoleMap (record.elementRoles[m], mesh.triangles, mesh.triMaterial, map)
                      : BuildSunStudyElementMap (tiles, layouts, record.gridSpacing, mesh.triangles, mesh.triMaterial,
                                                 faceBase, map);
            if (!valid) {
                error = "sun study display refused an incomplete element map";
                return nullptr;
            }
            maps->push_back (std::move (map));
            faceBase += static_cast<uint32_t> (mesh.TriangleCount ());
        }
        upload->sharedElements = maps;
        maps_[roles ? 1 : 0] = maps;
    }
    if (width_ == upload->width && height_ == upload->height) {
        if (auto base = previousImage_.lock (); base != nullptr && base->size () == upload->texels->size ()) {
            upload->baseTexels = std::move (base);
            if (upload->baseTexels != upload->texels)
                upload->atlasRegions = evp::sunstudy::AtlasChangedRegions (width_, height_, 1, *upload->baseTexels,
                                                                           *upload->texels, 512, cancelled);
        }
        if (words_ == upload->stepWords) {
            if (auto base = previousSteps_.lock (); base != nullptr && base->size () == upload->stepMasks->size ()) {
                upload->baseStepMasks = std::move (base);
                if (upload->baseStepMasks != upload->stepMasks)
                    upload->stepRegions = evp::sunstudy::AtlasChangedRegions (
                        width_, height_, words_, *upload->baseStepMasks, *upload->stepMasks, 512, cancelled);
            }
        }
    }
    if (cancelled ())
        return nullptr;
    previousImage_ = upload->texels;
    previousSteps_ = upload->stepMasks;
    width_ = upload->width;
    height_ = upload->height;
    words_ = upload->stepWords;
    return upload;
}

} // namespace geomsrv::archviz
