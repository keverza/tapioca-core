#include "Edit/EditVolumetricDetail.hpp"

#include <cstring>

namespace tapioca::geometry {
namespace {

void HashBytes (uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*> (data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void HashPod (uint64_t& hash, const T& value)
{
    HashBytes (hash, &value, sizeof value);
}

void HashNumber (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    static_assert (sizeof bits == sizeof value);
    std::memcpy (&bits, &value, sizeof bits);
    HashPod (hash, bits);
}

void HashVec (uint64_t& hash, Vec3 value)
{
    HashNumber (hash, value.x);
    HashNumber (hash, value.y);
    HashNumber (hash, value.z);
}

void HashCount (uint64_t& hash, size_t count)
{
    HashPod (hash, static_cast<uint64_t> (count));
}

void HashFrame (uint64_t& hash, const VolumetricFrame& frame)
{
    HashVec (hash, frame.origin);
    for (const auto& axis : frame.axes)
        HashVec (hash, axis);
}

void HashPrimitive (uint64_t& hash, const VolumetricPrimitive& primitive)
{
    HashPod (hash, primitive.kind);
    HashVec (hash, primitive.centerOrA);
    HashVec (hash, primitive.b);
    HashFrame (hash, primitive.frame);
    HashVec (hash, primitive.half);
    HashNumber (hash, primitive.radius);
    HashNumber (hash, primitive.halfHeight);
    HashCount (hash, primitive.profile.size ());
    for (const auto& point : primitive.profile) {
        HashNumber (hash, point[0]);
        HashNumber (hash, point[1]);
    }
}

} // namespace

uint64_t HashVolumetricDetail (const VolumetricDetail& detail)
{
    uint64_t hash = 1469598103934665603ull;
    HashNumber (hash, detail.voxelSize);
    HashCount (hash, detail.entries.size ());
    for (const auto& entry : detail.entries) {
        if (const auto* operation = std::get_if<VolumetricOperation> (&entry)) {
            HashPod (hash, uint32_t { 1 });
            HashPod (hash, operation->id);
            HashPrimitive (hash, operation->primitive);
            HashPod (hash, operation->operation);
            HashPod (hash, operation->profile);
            HashNumber (hash, operation->blendRadius);
            HashPod (hash, operation->materialId);
            HashPod (hash, operation->enabled);
        }
        else {
            const auto& group = std::get<VolumetricGroup> (entry);
            HashPod (hash, uint32_t { 2 });
            HashPod (hash, group.id);
            HashPod (hash, group.enabled);
            HashPod (hash, group.isolated);
            HashPod (hash, group.profile);
            HashNumber (hash, group.blendRadius);
        }
    }
    HashPod (hash, detail.anchor.has_value ());
    if (detail.anchor) {
        HashPod (hash, detail.anchor->terrainRevision);
        HashPod (hash, detail.anchor->terrainHash);
        HashNumber (hash, detail.anchor->x);
        HashNumber (hash, detail.anchor->y);
        HashNumber (hash, detail.anchor->zOffset);
    }
    return hash;
}

} // namespace tapioca::geometry
