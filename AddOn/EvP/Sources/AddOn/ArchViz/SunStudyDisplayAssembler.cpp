#include "ArchViz/SunStudyDisplayAssembler.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyDisplayData.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyBounds.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyResultKey.hpp" // MatchFaces
#include "ArchViz/ArchVizLog.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>

namespace geomsrv::archviz {

static std::shared_ptr<const tapioca::sunstudy::CapturePlan>
BuildCapturePlan (const Snapshot& snapshot, const std::vector<uint8_t>& roles,
                  const tapioca::sunstudy::AnalysisRoi& roi)
{
    if (!roi.IsValid () || roles.size () != snapshot.meshes.size ())
        return {};
    using Plan = tapioca::sunstudy::CapturePlan;
    const double span = std::max (roi.maxX - roi.minX, roi.maxY - roi.minY) * 1.12;
    const double cell = span / Plan::side, left = (roi.minX + roi.maxX - span) / 2,
                 top = (roi.minY + roi.maxY + span) / 2;
    std::array<double, Plan::side * Plan::side> heights;
    heights.fill (-std::numeric_limits<double>::infinity ());
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        if (roles[m] == uint8_t (evp::sunstudy::ElementRole::Ignored))
            continue;
        const auto& mesh = snapshot.meshes[m];
        for (size_t t = 0; t + 2 < mesh.triangles.size (); t += 3) {
            double x[3], y[3], z[3];
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                const size_t v = size_t (mesh.triangles[t + k]) * 3;
                if (v + 2 >= mesh.vertices.size ()) {
                    valid = false;
                    break;
                }
                x[k] = (mesh.vertices[v] - left) / cell;
                y[k] = (top - mesh.vertices[v + 1]) / cell;
                z[k] = mesh.vertices[v + 2];
                valid &= std::isfinite (x[k]) && std::isfinite (y[k]) && std::isfinite (z[k]);
            }
            if (!valid)
                continue;
            const double area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
            if (std::abs (area) < 1e-12)
                continue;
            const auto first = [] (const double (&v)[3]) {
                return int (std::clamp (std::ceil (*std::min_element (v, v + 3) - 0.5), 0., double (Plan::side)));
            };
            const auto last = [] (const double (&v)[3]) {
                return int (std::clamp (std::floor (*std::max_element (v, v + 3) - 0.5), -1., double (Plan::side - 1)));
            };
            const int x0 = first (x), x1 = last (x), y0 = first (y), y1 = last (y);
            for (int j = y0; j <= y1; ++j)
                for (int i = x0; i <= x1; ++i) {
                    const double dx = i + 0.5 - x[0], dy = j + 0.5 - y[0];
                    const double b = (dx * (y[2] - y[0]) - dy * (x[2] - x[0])) / area,
                                 c = ((x[1] - x[0]) * dy - (y[1] - y[0]) * dx) / area;
                    if (b >= 0 && c >= 0 && b + c <= 1) {
                        auto& h = heights[j * Plan::side + i];
                        h = std::max (h, z[0] + b * (z[1] - z[0]) + c * (z[2] - z[0]));
                    }
                }
        }
    }
    double lo = std::numeric_limits<double>::infinity (), hi = -lo;
    for (double h : heights)
        if (std::isfinite (h)) {
            lo = std::min (lo, h);
            hi = std::max (hi, h);
        }
    if (lo > hi)
        return {};
    auto plan = std::make_shared<Plan> ();
    for (size_t i = 0; i < heights.size (); ++i)
        if (std::isfinite (heights[i]))
            plan->cells[i] = uint8_t (1 + (hi > lo ? 100 * (heights[i] - lo) / (hi - lo) : 0));
    return plan;
}

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
        captureRoi_ = {};
        if (record.clipRadius > 0) {
            captureRoi_ = { record.clipX - record.clipRadius, record.clipY - record.clipRadius,
                            record.clipX + record.clipRadius, record.clipY + record.clipRadius };
        }
        else if (record.elementRoles.size () == snapshot.meshes.size ()) {
            std::vector<std::string> receivers;
            for (size_t i = 0; i < snapshot.meshes.size (); ++i)
                if (record.elementRoles[i] == uint8_t (evp::sunstudy::ElementRole::Analysis))
                    receivers.push_back (snapshot.meshes[i].guid);
            if (auto roi = evp::sunstudy::ResolveAnalysisRoi (snapshot, receivers, {}))
                captureRoi_ = *roi;
        }
        maps_[0].reset ();
        capturePlan_ = BuildCapturePlan (snapshot, record.elementRoles, captureRoi_);
        maps_[1].reset ();
    }
    const auto mapping = Clock::now ();
    upload->captureRoi = captureRoi_;
    upload->capturePlan = capturePlan_;
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
                upload->atlasRegions = evp::sunstudy::kernels::AtlasChangedRegions (
                    width_, height_, 1, *upload->baseTexels, *upload->texels, 512, cancelled);
        }
        if (words_ == upload->stepWords) {
            if (auto base = previousSteps_.lock (); base != nullptr && base->size () == upload->stepMasks->size ()) {
                upload->baseStepMasks = std::move (base);
                if (upload->baseStepMasks != upload->stepMasks)
                    upload->stepRegions = evp::sunstudy::kernels::AtlasChangedRegions (
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
