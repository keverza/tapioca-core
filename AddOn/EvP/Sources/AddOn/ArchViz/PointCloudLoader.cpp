#include "ArchViz/PointCloudLoader.hpp"

#include "ArchViz/PointCloudHierarchy.hpp"
#include "ArchViz/SceneCmdQueue.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace geomsrv {
namespace archviz {

namespace {

constexpr size_t kLeafCapacity = 65536;
constexpr size_t kRepresentativeCapacity = 4096;

std::string SourceIdentity (const std::filesystem::path& path)
{
    std::error_code error;
    const uintmax_t bytes = std::filesystem::file_size (path, error);
    if (error)
        return {};
    const auto writeTime = std::filesystem::last_write_time (path, error);
    if (error)
        return {};
    return std::to_string (bytes) + ":" + std::to_string (writeTime.time_since_epoch ().count ());
}

double MillisecondsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
}

} // namespace

PointCloudBoundsResult InspectPointCloudPly (const std::wstring& pathText)
{
    PointCloudBoundsResult result;
    std::ifstream input (std::filesystem::path (pathText), std::ios::binary);
    if (!input) {
        result.error = "Could not open the point-cloud PLY.";
        return result;
    }

    PointCloudData cloud;
    if (!LoadPointCloudPly (input, cloud, result.error))
        return result;
    result.points = cloud.vertices.size ();
    for (size_t axis = 0; axis < 3; ++axis) {
        result.boundsMin[axis] = cloud.origin[axis] + cloud.boundsMin[axis];
        result.boundsMax[axis] = cloud.origin[axis] + cloud.boundsMax[axis];
    }
    result.succeeded = true;
    return result;
}

PointCloudLoadResult LoadPointCloudForDiligent (const std::wstring& pathText, const std::string& layerId,
                                                const double projectToSurvey[12])
{
    PointCloudLoadResult result;
    if (pathText.empty () || layerId.empty ()) {
        result.error = "Point-cloud path and layer id are required.";
        return result;
    }

    const std::filesystem::path path (pathText);
    std::ifstream input (path, std::ios::binary);
    if (!input) {
        result.error = "Could not open the point-cloud PLY.";
        return result;
    }

    PointCloudData cloud;
    const auto parseStart = std::chrono::steady_clock::now ();
    if (!LoadPointCloudPly (input, cloud, result.error))
        return result;
    if (!PlacePointCloudInProject (cloud, projectToSurvey, result.error))
        return result;
    result.parseMilliseconds = MillisecondsSince (parseStart);
    result.points = cloud.vertices.size ();
    result.sourceId = SourceIdentity (path);

    PointCloudHierarchy hierarchy;
    const auto hierarchyStart = std::chrono::steady_clock::now ();
    if (!BuildPointCloudHierarchy (cloud, kLeafCapacity, kRepresentativeCapacity, hierarchy, result.error))
        return result;
    result.hierarchyMilliseconds = MillisecondsSince (hierarchyStart);
    result.nodes = hierarchy.nodes.size ();

    auto layer = std::make_unique<PointLayerUpload> ();
    layer->layerId = layerId;
    layer->sourceId = result.sourceId;
    const std::u8string utf8Path = path.u8string ();
    layer->sourcePath.assign (utf8Path.begin (), utf8Path.end ());
    for (size_t axis = 0; axis < 3; ++axis) {
        result.projectOrigin[axis] = cloud.origin[axis];
        result.projectBoundsMin[axis] = cloud.origin[axis] + cloud.boundsMin[axis];
        result.projectBoundsMax[axis] = cloud.origin[axis] + cloud.boundsMax[axis];
        layer->rtcOrigin[axis] = cloud.origin[axis];
        layer->boundsMin[axis] = cloud.boundsMin[axis];
        layer->boundsMax[axis] = cloud.boundsMax[axis];
    }
    result.queuedBytes += layer->Bytes ();

    std::vector<uint32_t> parents (hierarchy.nodes.size (), UINT32_MAX);
    for (const PointCloudHierarchyNode& node : hierarchy.nodes) {
        for (uint32_t child : node.children) {
            if (child < parents.size ())
                parents[child] = node.id;
        }
    }

    SceneCmdQueue& queue = SceneCmdQueue::Get ();
    queue.PushBeginPointLayer (std::move (layer));
    for (const PointCloudHierarchyNode& node : hierarchy.nodes) {
        auto upload = std::make_unique<PointNodeUpload> ();
        upload->layerId = layerId;
        upload->nodeId = node.id;
        upload->parentId = parents[node.id];
        upload->level = node.level;
        upload->geometricError = node.geometricError;
        for (size_t axis = 0; axis < 3; ++axis) {
            upload->boundsMin[axis] = node.boundsMin[axis];
            upload->boundsMax[axis] = node.boundsMax[axis];
        }
        upload->vertices.reserve (node.pointIndices.size ());
        for (uint32_t pointIndex : node.pointIndices)
            upload->vertices.push_back (cloud.vertices[pointIndex]);
        result.queuedBytes += upload->Bytes ();
        queue.PushUpsertPointNode (std::move (upload));
    }
    queue.PushEndPointLayer (layerId);
    result.succeeded = true;
    return result;
}

} // namespace archviz
} // namespace geomsrv
