#include "ArchViz/SunStudyDisplayAssembler.hpp"
#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyResultKey.hpp" // MatchFaces
#include "ArchViz/ArchVizLog.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>

namespace geomsrv::archviz {

std::unique_ptr<SunStudyAtlasUpload>
SunStudyDisplayAssembler::Prepare (const evp::sunstudy::StudyRecord& record, const Snapshot& snapshot,
                                   const SunStudyDisplayOptions& options, std::string& error,
                                   const std::function<bool ()>& isCancelled, const Snapshot* materials)
{
    const auto cancelled = [&] { return record.cancelRequested.load () || (isCancelled && isCancelled ()); };
    const auto progress = record.CurrentProgress ();
    if (record.snapshotId != snapshot.id || (options.preview && !progress.converged)) {
        error = "sun study display source is stale or the preview has not completed its day";
        return nullptr;
    }
    // The parts of sun-display, logged as sun-display-parts: on a one-element
    // edit of a 1.6M-sample study the whole display costs 2.2 s, and the fix
    // differs for each part (#97).
    using Clock = std::chrono::steady_clock;
    const auto msSince = [] (Clock::time_point since) {
        return std::chrono::duration<double, std::milli> (Clock::now () - since).count ();
    };
    const auto started = Clock::now ();
    const auto data = evp::sunstudy::PrepareStudyDisplayData (record, error, cancelled);
    if (data == nullptr)
        return nullptr;
    const double dataMs = msSince (started);
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
    // Packed step k is series step k; anything else would aim the preview's
    // shadow map at the wrong time, so a mismatch carries no suns at all.
    const auto& steps = record.series.Steps ();
    if (steps.size () == data->stepCount) {
        upload->stepSuns.reserve (steps.size ());
        for (const auto& step : steps)
            upload->stepSuns.push_back ({ static_cast<float> (step.direction[0]),
                                          static_cast<float> (step.direction[1]),
                                          static_cast<float> (step.direction[2]) });
    }
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
    const auto mapping = Clock::now ();
    upload->sharedElements = maps_[roles ? 1 : 0].lock ();
    bool mapsRebuilt = upload->sharedElements == nullptr;
    size_t retinted = 0;
    if (materials != nullptr) {
        // ⚠️ A RETINT, NOT A NEW STUDY: the record's tiles and face order stand;
        // only the permutation the viewer draws them in moved with the material.
        std::unordered_map<std::string, const Mesh*> later;
        for (const Mesh& mesh : materials->meshes)
            later.emplace (mesh.guid, &mesh);
        const auto base = upload->sharedElements;
        auto maps = std::make_shared<std::vector<SunStudyElementMap>> ();
        maps->reserve (snapshot.meshes.size ());
        uint32_t faceBase = 0;
        for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
            if (cancelled ())
                return nullptr;
            const auto& mesh = snapshot.meshes[m];
            const auto found = later.find (mesh.guid);
            if (found == later.end ()) {
                error = "sun study retint found element " + mesh.guid + " missing";
                return nullptr;
            }
            const Mesh& now = *found->second;
            const bool same = now.triangles == mesh.triangles && now.vertices == mesh.vertices;
            if (same && base != nullptr && m < base->size () && now.triMaterial == mesh.triMaterial) {
                maps->push_back ((*base)[m]);
                faceBase += static_cast<uint32_t> (mesh.TriangleCount ());
                continue;
            }
            SunStudyElementMap map;
            map.guid = mesh.guid;
            bool valid = false;
            if (roles)
                valid = BuildSunStudyRoleMap (record.elementRoles[m], now.triangles, now.triMaterial, map);
            else if (same)
                valid = BuildSunStudyElementMap (tiles, layouts, record.gridSpacing, now.triangles, now.triMaterial,
                                                 faceBase, map);
            else {
                // ⚠️ THE SAME TRIANGLES IN ANOTHER ORDER (one painted face re-emits
                // a wall that way): each new face takes the tile its old twin was
                // measured into. A face with no twin is a changed shape -- refused,
                // and the study reruns.
                const std::vector<uint32_t> twin = evp::sunstudy::MatchFaces (mesh, now);
                if (twin.empty ()) {
                    error = "sun study retint found element " + mesh.guid + " changed in geometry";
                    return nullptr;
                }
                std::vector<evp::sunstudy::AtlasTile> ownTiles (twin.size ());
                std::vector<evp::sunstudy::FaceLayout> ownLayouts (twin.size ());
                for (size_t face = 0; face < twin.size (); ++face) {
                    ownTiles[face] = tiles[faceBase + twin[face]];
                    ownLayouts[face] = layouts[faceBase + twin[face]];
                }
                valid = BuildSunStudyElementMap (ownTiles, ownLayouts, record.gridSpacing, now.triangles,
                                                 now.triMaterial, 0, map);
            }
            if (!valid) {
                error = "sun study retint refused an incomplete element map";
                return nullptr;
            }
            maps->push_back (std::move (map));
            ++retinted;
            faceBase += static_cast<uint32_t> (mesh.TriangleCount ());
        }
        upload->sharedElements = maps; // never cached: the cache is the record's own order
        mapsRebuilt = false;
    }
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
    const double mapsMs = msSince (mapping);
    const auto diffing = Clock::now ();
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
    ArchVizLog ("pipeline: stage=sun-display-parts study=" + record.id + " dataMs=" + std::to_string (dataMs) +
                " mapsMs=" + std::to_string (mapsMs) + " mapsRebuilt=" + std::to_string (mapsRebuilt) +
                (materials != nullptr ? " retintedElements=" + std::to_string (retinted) : std::string ()) +
                " regionsMs=" + std::to_string (msSince (diffing)) +
                " atlasRegions=" + std::to_string (upload->atlasRegions.size ()) +
                " stepRegions=" + std::to_string (upload->stepRegions.size ()));
    previousImage_ = upload->texels;
    previousSteps_ = upload->stepMasks;
    width_ = upload->width;
    height_ = upload->height;
    words_ = upload->stepWords;
    return upload;
}

} // namespace geomsrv::archviz
