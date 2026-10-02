#include "SunStudy/SunStudyOccluders.hpp"

#include <algorithm>
#include <map>

namespace evp::sunstudy {
namespace {
using MeshMap = std::map<std::string, const geomsrv::Mesh*>;

bool AddMesh (MeshMap& meshes, const geomsrv::Mesh& mesh)
{
    const std::string key = CanonicalGuid (mesh.guid);
    return !key.empty () && meshes.emplace (key, &mesh).second;
}

bool SameContext (const geomsrv::Snapshot& snapshot, const ElementRoles& roles, const SunStudyOccluders* previous,
                  const std::function<bool ()>& isCancelled)
{
    if (previous == nullptr || previous->context == nullptr || previous->contextSnapshot == nullptr)
        return false;
    MeshMap before, after;
    for (const auto& mesh : previous->contextSnapshot->meshes) {
        if ((isCancelled && isCancelled ()) || !AddMesh (before, mesh))
            return false;
    }
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        if (isCancelled && isCancelled ())
            return false;
        if (roles.roles[m] == ElementRole::Context && !AddMesh (after, snapshot.meshes[m]))
            return false;
    }
    if (before.size () != after.size ())
        return false;
    for (const auto& [guid, mesh] : after) {
        if (isCancelled && isCancelled ())
            return false;
        const auto found = before.find (guid);
        if (found == before.end () || mesh->vertices != found->second->vertices ||
            mesh->triangles != found->second->triangles)
            return false;
    }
    return true;
}

std::shared_ptr<const geomsrv::Snapshot> RoleSnapshot (const geomsrv::Snapshot& snapshot, const ElementRoles& roles,
                                                       ElementRole role, const std::function<bool ()>& isCancelled)
{
    auto subset = std::make_shared<geomsrv::Snapshot> ();
    subset->id = snapshot.id;
    subset->scope = snapshot.scope;
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        if (isCancelled && isCancelled ())
            return nullptr;
        if (roles.roles[m] == role)
            subset->meshes.push_back (snapshot.meshes[m]);
    }
    return subset;
}
} // namespace

std::shared_ptr<const SunStudyOccluders> BuildSunStudyOccluders (const geomsrv::Snapshot& snapshot,
                                                                 const ElementRoles& roles,
                                                                 const SunStudyOccluders* previous,
                                                                 const std::function<bool ()>& isCancelled)
{
    if (roles.roles.size () != snapshot.meshes.size () || (isCancelled && isCancelled ()))
        return nullptr;
    auto parts = std::make_shared<SunStudyOccluders> ();
    const auto analysis = RoleSnapshot (snapshot, roles, ElementRole::Analysis, isCancelled);
    if (analysis == nullptr)
        return nullptr;
    parts->analysis = std::make_shared<const geomsrv::QueryEngine> (analysis);
    if (roles.context > 0) {
        parts->contextReused = SameContext (snapshot, roles, previous, isCancelled);
        if (parts->contextReused) {
            parts->context = previous->context;
            parts->contextSnapshot = previous->contextSnapshot;
        }
        else {
            parts->contextSnapshot = RoleSnapshot (snapshot, roles, ElementRole::Context, isCancelled);
            if (parts->contextSnapshot == nullptr)
                return nullptr;
            parts->context = std::make_shared<const geomsrv::QueryEngine> (parts->contextSnapshot);
        }
    }
    return isCancelled && isCancelled () ? nullptr : parts;
}

SunStudyPartitionTraversal::SunStudyPartitionTraversal (std::shared_ptr<const geomsrv::QueryEngine> analysis,
                                                        std::shared_ptr<const geomsrv::QueryEngine> context)
    : analysis_ (std::move (analysis)), context_ (std::move (context)), analysisCpu_ (analysis_), contextCpu_ (context_)
{
}

bool SunStudyPartitionTraversal::Occluded (const double origin[3], const double dir[3], double tmin, double tmax) const
{
    return (analysis_ != nullptr && analysis_->Occluded (origin, dir, tmin, tmax)) ||
           (context_ != nullptr && context_->Occluded (origin, dir, tmin, tmax));
}

void SunStudyPartitionTraversal::OccludeDirectional (const double* origins, size_t count, const double dir[3],
                                                     double tmin, double tmax, uint8_t* out, size_t maxParallel) const
{
    analysisCpu_.OccludeDirectional (origins, count, dir, tmin, tmax, out, maxParallel);
    if (context_ == nullptr || out == nullptr || count == 0)
        return;
    std::vector<uint8_t> context (count);
    contextCpu_.OccludeDirectional (origins, count, dir, tmin, tmax, context.data (), maxParallel);
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<uint8_t> (out[i] | context[i]);
}

void SunStudyPartitionTraversal::OccludeRays (const OcclusionRay* rays, size_t count, uint8_t* out,
                                              size_t maxParallel) const
{
    analysisCpu_.OccludeRays (rays, count, out, maxParallel);
    if (context_ == nullptr || out == nullptr || count == 0)
        return;
    std::vector<uint8_t> context (count);
    contextCpu_.OccludeRays (rays, count, context.data (), maxParallel);
    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<uint8_t> (out[i] | context[i]);
}

uint64_t SunStudyPartitionTraversal::SceneVersion () const
{
    return analysisCpu_.SceneVersion ();
}
} // namespace evp::sunstudy
