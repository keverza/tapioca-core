#include "Edit/EditVolumetricDetail.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

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

bool FiniteVector (Vec3 value)
{
    return std::isfinite (value.x) && std::isfinite (value.y) && std::isfinite (value.z);
}

double DotVector (Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 CrossVector (Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

bool ValidFrame (const VolumetricFrame& frame)
{
    if (!FiniteVector (frame.origin) || !std::all_of (frame.axes.begin (), frame.axes.end (), FiniteVector))
        return false;
    const auto& x = frame.axes[0];
    const auto& y = frame.axes[1];
    const auto& z = frame.axes[2];
    const double actual[] = { DotVector (x, x), DotVector (y, y), DotVector (z, z), DotVector (x, y),
                              DotVector (x, z), DotVector (y, z), DotVector (x, CrossVector (y, z)) };
    const double expected[] = { 1, 1, 1, 0, 0, 0, 1 };
    // Dimensionless absolute dot-product tolerance shared with the field compiler.
    return std::equal (std::begin (actual), std::end (actual), std::begin (expected), [] (double a, double b) {
        return std::isfinite (a) && std::abs (a - b) <= 1e-12;
    });
}

using Point2 = std::array<double, 2>;

double Cross2 (Point2 a, Point2 b, Point2 c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

bool OnSegment (Point2 a, Point2 b, Point2 point)
{
    return point[0] >= std::min (a[0], b[0]) && point[0] <= std::max (a[0], b[0]) &&
           point[1] >= std::min (a[1], b[1]) && point[1] <= std::max (a[1], b[1]);
}

bool SegmentsIntersect (Point2 a, Point2 b, Point2 c, Point2 d)
{
    const double abC = Cross2 (a, b, c), abD = Cross2 (a, b, d);
    const double cdA = Cross2 (c, d, a), cdB = Cross2 (c, d, b);
    if (!std::isfinite (abC) || !std::isfinite (abD) || !std::isfinite (cdA) || !std::isfinite (cdB))
        return true;
    if (((abC > 0 && abD < 0) || (abC < 0 && abD > 0)) &&
        ((cdA > 0 && cdB < 0) || (cdA < 0 && cdB > 0)))
        return true;
    return (abC == 0 && OnSegment (a, b, c)) || (abD == 0 && OnSegment (a, b, d)) ||
           (cdA == 0 && OnSegment (c, d, a)) || (cdB == 0 && OnSegment (c, d, b));
}

bool ValidProfile (const std::vector<Point2>& profile)
{
    if (profile.size () < 3)
        return false;
    double twiceArea = 0;
    for (size_t index = 0; index < profile.size (); ++index) {
        const auto& a = profile[index];
        const auto& b = profile[(index + 1) % profile.size ()];
        if (!std::isfinite (a[0]) || !std::isfinite (a[1]) || a == b)
            return false;
        twiceArea += a[0] * b[1] - a[1] * b[0];
    }
    if (!std::isfinite (twiceArea) || twiceArea == 0)
        return false;
    for (size_t first = 0; first < profile.size (); ++first) {
        for (size_t second = first + 1; second < profile.size (); ++second) {
            const bool adjacent = second == first + 1 || (first == 0 && second + 1 == profile.size ());
            if (!adjacent && SegmentsIntersect (profile[first], profile[(first + 1) % profile.size ()],
                                                profile[second], profile[(second + 1) % profile.size ()]))
                return false;
        }
    }
    return true;
}

bool ValidPrimitive (const VolumetricPrimitive& primitive)
{
    const bool profileFinite = std::all_of (primitive.profile.begin (), primitive.profile.end (), [] (const Point2& point) {
        return std::isfinite (point[0]) && std::isfinite (point[1]);
    });
    const bool finiteFields = profileFinite && FiniteVector (primitive.centerOrA) && FiniteVector (primitive.b) &&
                              FiniteVector (primitive.half) && std::isfinite (primitive.radius) &&
                              std::isfinite (primitive.halfHeight) && ValidFrame (primitive.frame);
    if (!finiteFields)
        return false;
    switch (primitive.kind) {
    case VolumetricPrimitiveKind::Sphere:
        return primitive.radius > 0;
    case VolumetricPrimitiveKind::RoundedBox:
        return primitive.half.x > 0 && primitive.half.y > 0 && primitive.half.z > 0 && primitive.radius >= 0 &&
               primitive.radius <= std::min ({ primitive.half.x, primitive.half.y, primitive.half.z });
    case VolumetricPrimitiveKind::Capsule: {
        const Vec3 delta { primitive.b.x - primitive.centerOrA.x, primitive.b.y - primitive.centerOrA.y,
                           primitive.b.z - primitive.centerOrA.z };
        const double lengthSquared = DotVector (delta, delta);
        return primitive.radius > 0 && FiniteVector (delta) && std::isfinite (lengthSquared) && lengthSquared > 0;
    }
    case VolumetricPrimitiveKind::Extrude:
        return primitive.halfHeight > 0 && ValidProfile (primitive.profile);
    }
    return false;
}

bool ValidBoolean (VolumetricBoolean value)
{
    return value == VolumetricBoolean::Union || value == VolumetricBoolean::Subtract ||
           value == VolumetricBoolean::Intersect;
}

bool ValidProfileKind (VolumetricBlendProfile value)
{
    return value == VolumetricBlendProfile::Sharp || value == VolumetricBlendProfile::Round;
}

bool Refuse (std::string& error, const char* message)
{
    error = message;
    return false;
}

VolumetricOperation PresetOperation (VolumetricPrimitiveKind kind, int32_t materialId)
{
    VolumetricOperation operation;
    operation.primitive.kind = kind;
    operation.materialId = materialId;
    return operation;
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

bool ValidateVolumetricDetail (const VolumetricDetail& detail, std::string& error)
{
    error.clear ();
    if (detail.id == kNoId || detail.revision == 0 ||
        (detail.kind != VolumetricDetailKind::Custom && detail.kind != VolumetricDetailKind::Rock &&
         detail.kind != VolumetricDetailKind::Cave))
        return Refuse (error, "volumetric detail requires a durable ID, revision and known kind");
    if (!std::isfinite (detail.voxelSize) || detail.voxelSize <= 0)
        return Refuse (error, "volumetric detail voxel size must be positive and finite");
    if (detail.entries.empty () || detail.entries.size () > 256)
        return Refuse (error, "volumetric detail requires 1-256 ordered stack entries");
    if (detail.name.find ('\0') != std::string::npos)
        return Refuse (error, "volumetric detail name contains a null character");
    if (detail.anchor && (detail.anchor->terrainRevision == 0 || !std::isfinite (detail.anchor->x) ||
                          !std::isfinite (detail.anchor->y) || !std::isfinite (detail.anchor->zOffset)))
        return Refuse (error, "volumetric terrain anchor requires a revision and finite placement");

    std::set<Id> entryIds;
    bool rootHasField = false;
    bool inGroup = false;
    bool groupEnabled = false;
    bool groupIsolated = false;
    bool groupHasField = false;
    for (const auto& entry : detail.entries) {
        if (const auto* group = std::get_if<VolumetricGroup> (&entry)) {
            if (group->id == kNoId || !entryIds.insert (group->id).second || !ValidProfileKind (group->profile) ||
                !std::isfinite (group->blendRadius) || group->blendRadius < 0)
                return Refuse (error, "volumetric group has an invalid ID, profile or blend radius");
            inGroup = true;
            groupEnabled = group->enabled;
            groupIsolated = group->isolated;
            groupHasField = false;
            continue;
        }
        const auto& operation = std::get<VolumetricOperation> (entry);
        if (operation.id == kNoId || !entryIds.insert (operation.id).second ||
            !ValidPrimitive (operation.primitive) || !ValidBoolean (operation.operation) ||
            !ValidProfileKind (operation.profile) || !std::isfinite (operation.blendRadius) ||
            operation.blendRadius < 0 || operation.materialId <= 0)
            return Refuse (error, "volumetric operation has an invalid ID, primitive, operation, blend or material");
        if (!operation.enabled || (inGroup && !groupEnabled))
            continue;
        if (inGroup) {
            if (!groupHasField && operation.operation != VolumetricBoolean::Union)
                return Refuse (error, "volumetric group's first enabled operation must be Union");
            groupHasField = true;
            if (groupIsolated)
                rootHasField = true;
            else if (!rootHasField) {
                if (operation.operation != VolumetricBoolean::Union)
                    return Refuse (error, "volumetric root's first enabled operation must be Union");
                rootHasField = true;
            }
        }
        else if (!rootHasField) {
            if (operation.operation != VolumetricBoolean::Union)
                return Refuse (error, "volumetric root's first enabled operation must be Union");
            rootHasField = true;
        }
    }
    if (!rootHasField)
        return Refuse (error, "volumetric detail has no enabled field");
    return true;
}

VolumetricDetail MakeVolumetricRockPreset ()
{
    VolumetricDetail detail;
    detail.kind = VolumetricDetailKind::Rock;
    detail.name = "Rock";
    detail.voxelSize = 0.15;
    auto first = PresetOperation (VolumetricPrimitiveKind::Sphere, 1);
    first.primitive.centerOrA = { -0.5, 0, 0 };
    first.primitive.radius = 1.5;
    detail.entries.emplace_back (first);
    auto second = PresetOperation (VolumetricPrimitiveKind::Sphere, 1);
    second.primitive.centerOrA = { 0.8, 0.2, 0.1 };
    second.primitive.radius = 1.2;
    second.profile = VolumetricBlendProfile::Round;
    second.blendRadius = 0.35;
    detail.entries.emplace_back (second);
    return detail;
}

VolumetricDetail MakeVolumetricCavePreset ()
{
    VolumetricDetail detail;
    detail.kind = VolumetricDetailKind::Cave;
    detail.name = "Cave";
    detail.voxelSize = 0.15;
    auto body = PresetOperation (VolumetricPrimitiveKind::RoundedBox, 1);
    body.primitive.half = { 2, 1.5, 1 };
    body.primitive.radius = 0.1;
    detail.entries.emplace_back (body);
    auto cutter = PresetOperation (VolumetricPrimitiveKind::Capsule, 2);
    cutter.operation = VolumetricBoolean::Subtract;
    cutter.profile = VolumetricBlendProfile::Round;
    cutter.blendRadius = 0.1;
    cutter.primitive.centerOrA = { -3, 0, 0 };
    cutter.primitive.b = { 3, 0, 0 };
    cutter.primitive.radius = 0.6;
    detail.entries.emplace_back (cutter);
    return detail;
}

} // namespace tapioca::geometry
