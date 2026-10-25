#ifndef TAPIOCA_GEOMETRY_RUST_RAYCAST_HPP
#define TAPIOCA_GEOMETRY_RUST_RAYCAST_HPP

// The Rust ray-tracing scene (raycast plan E0): built from a snapshot the way
// QueryEngine concatenates it, and queried with QueryEngine::Occluded's
// semantics. Opt-in parity service; no production consumer is switched.

#include "Geometry/Mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct TapiocaRaycastScene;

namespace tapioca::geometry {

class RustRaycastScene final {
  public:
    // nullptr with `error` set when the snapshot is refused (non-finite
    // coordinates, an index out of range) or the archive fails.
    static std::unique_ptr<RustRaycastScene> Build (const geomsrv::Snapshot& snapshot, std::string& error);
    ~RustRaycastScene ();
    RustRaycastScene (const RustRaycastScene&) = delete;
    RustRaycastScene& operator= (const RustRaycastScene&) = delete;

    // CpuTraversal::OccludeDirectionalCancellable: false when cancelled or
    // refused (`error` set only when refused). Safe from many threads at once.
    bool OccludeDirectional (const double* origins, size_t count, const double direction[3], double tmin, double tmax,
                             uint8_t* out, size_t maxParallel, const std::function<bool ()>& isCancelled,
                             std::string& error) const;

  private:
    explicit RustRaycastScene (TapiocaRaycastScene* handle) : handle_ (handle)
    {
    }
    TapiocaRaycastScene* handle_;
};

} // namespace tapioca::geometry

#endif
