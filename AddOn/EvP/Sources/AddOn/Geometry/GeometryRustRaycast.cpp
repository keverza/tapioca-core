#include "Geometry/GeometryRustRaycast.hpp"

#include "Geometry/RaycastRustFfi.h"

#include <array>
#include <vector>

namespace tapioca::geometry {

namespace {

constexpr size_t kErrorCapacity = 256;

uint8_t PollCancel (void* context)
{
    const auto* poll = static_cast<const std::function<bool ()>*> (context);
    try {
        return (*poll) () ? 1 : 0;
    }
    catch (...) {
        return 1; // a failing poll stops the batch rather than unwinding into Rust
    }
}

} // namespace

std::unique_ptr<RustRaycastScene> RustRaycastScene::Build (const geomsrv::Snapshot& snapshot, std::string& error)
{
    // QueryEngine's concatenation: meshes in order, indices rebased per mesh.
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    for (const geomsrv::Mesh& mesh : snapshot.meshes) {
        const auto base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
        for (const uint32_t index : mesh.triangles)
            triangles.push_back (base + index);
    }
    const TapiocaRaycastSceneRequest request { { vertices.data (), vertices.size () },
                                               { triangles.data (), triangles.size () },
                                               snapshot.id };
    TapiocaRaycastScene* handle = nullptr;
    std::array<char, kErrorCapacity> message {};
    if (tapioca_raycast_scene_build (&request, &handle, message.data (), message.size ()) != TAPIOCA_OK) {
        error = message.data ();
        return nullptr;
    }
    return std::unique_ptr<RustRaycastScene> (new RustRaycastScene (handle));
}

RustRaycastScene::~RustRaycastScene ()
{
    tapioca_raycast_scene_destroy (handle_, nullptr, 0);
}

bool RustRaycastScene::OccludeDirectional (const double* origins, size_t count, const double direction[3], double tmin,
                                           double tmax, uint8_t* out, size_t maxParallel,
                                           const std::function<bool ()>& isCancelled, std::string& error) const
{
    TapiocaRaycastDirectionalRequest request {};
    request.origins = { origins, count * 3 };
    request.direction[0] = direction[0];
    request.direction[1] = direction[1];
    request.direction[2] = direction[2];
    request.tmin = tmin;
    request.tmax = tmax;
    request.maxParallel = maxParallel;
    if (isCancelled) {
        request.cancel.context = const_cast<std::function<bool ()>*> (&isCancelled);
        request.cancel.isCancelled = &PollCancel;
    }
    uint8_t completed = 0;
    std::array<char, kErrorCapacity> message {};
    if (tapioca_raycast_occlude_directional (handle_, &request, out, count, &completed, message.data (),
                                             message.size ()) != TAPIOCA_OK) {
        error = message.data ();
        return false;
    }
    return completed != 0;
}

} // namespace tapioca::geometry
