#ifndef TAPIOCA_EDIT_VOLUMETRICDETAIL_HPP
#define TAPIOCA_EDIT_VOLUMETRICDETAIL_HPP

// #140 SDF-1d: authored volumetric landscape definition. This is not a mesh
// document, evaluated snapshot, terrain field entry or native Accept record.
// The ordered stack below is the only authority; generated meshes are derived.
// Units and operation/group semantics are fixed by
// docs/guides/volumetric-detail-sdf.md.

#include "Edit/EditTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tapioca::geometry {

using geomsrv::edit::Id;
using geomsrv::edit::kNoId;
using geomsrv::edit::Vec3;

enum class VolumetricDetailKind : uint32_t { Custom = 0, Rock = 1, Cave = 2 };
enum class VolumetricBoolean : uint32_t { Union = 1, Subtract = 2, Intersect = 3 };
enum class VolumetricBlendProfile : uint32_t { Sharp = 1, Round = 2 };
enum class VolumetricPrimitiveKind : uint32_t { Sphere = 2, RoundedBox = 3, Capsule = 4, Extrude = 5 };

struct VolumetricFrame {
    Vec3 origin;
    // Local x/y/z axes in world space; finite, orthonormal and right-handed.
    std::array<Vec3, 3> axes = { Vec3 { 1, 0, 0 }, Vec3 { 0, 1, 0 }, Vec3 { 0, 0, 1 } };
    bool operator== (const VolumetricFrame&) const = default;
};

struct VolumetricPrimitive {
    VolumetricPrimitiveKind kind = VolumetricPrimitiveKind::Sphere;
    Vec3 centerOrA; // Sphere center / capsule endpoint A / frame origin is in `frame`.
    Vec3 b;         // Capsule endpoint B.
    VolumetricFrame frame;
    Vec3 half; // Rounded-box outer half extents, metres.
    double radius = 0.0;     // Sphere/capsule radius or rounded-box edge radius.
    double halfHeight = 0.0; // Extrusion half height, metres.
    // Simple closed local-xy polygon, no repeated closing vertex.
    std::vector<std::array<double, 2>> profile;
    bool operator== (const VolumetricPrimitive&) const = default;
};

struct VolumetricOperation {
    Id id = kNoId;
    VolumetricPrimitive primitive;
    VolumetricBoolean operation = VolumetricBoolean::Union;
    VolumetricBlendProfile profile = VolumetricBlendProfile::Sharp;
    double blendRadius = 0.0; // Metres; zero is a hard Boolean.
    int32_t materialId = 0;   // Positive material pool ID, never vertex colour.
    bool enabled = true;
    bool operator== (const VolumetricOperation&) const = default;
};

struct VolumetricGroup {
    Id id = kNoId;
    bool enabled = true;
    bool isolated = true;
    VolumetricBlendProfile profile = VolumetricBlendProfile::Sharp;
    double blendRadius = 0.0; // Isolated group's one Union into the parent.
    bool operator== (const VolumetricGroup&) const = default;
};

using VolumetricStackEntry = std::variant<VolumetricOperation, VolumetricGroup>;

struct VolumetricTerrainAnchor {
    uint64_t terrainRevision = 0;
    uint64_t terrainHash = 0;
    double x = 0.0, y = 0.0; // Authored plan anchor, world metres.
    double zOffset = 0.0;    // Above the sampled published TIN, metres.
    bool operator== (const VolumetricTerrainAnchor&) const = default;
};

struct VolumetricDetail {
    Id id = kNoId;
    VolumetricDetailKind kind = VolumetricDetailKind::Custom;
    std::string name;
    std::vector<VolumetricStackEntry> entries;
    double voxelSize = 0.1; // Final metric resolution; raw NX/NY/NZ is never authored.
    std::optional<VolumetricTerrainAnchor> anchor;
    uint64_t revision = 0;
    bool operator== (const VolumetricDetail&) const = default;
};

// FNV-1a over every geometry/job-affecting definition field in stack order.
// Object ID, display name, kind label and revision are identity/presentation,
// not content. The anchor identity/position is included because a job's placed
// result depends on it. No evaluated mesh or worker schedule enters the hash.
uint64_t HashVolumetricDetail (const VolumetricDetail& detail);

} // namespace tapioca::geometry

#endif
