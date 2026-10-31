#ifndef EVP_SUNSTUDY_SUNSTUDYRESULTKEY_HPP
#define EVP_SUNSTUDY_SUNSTUDYRESULTKEY_HPP

// SunStudy/SunStudyResultKey -- what of a snapshot a study's NUMBERS depend on (#97, #98).
//
// ⚠️ NOT THE SNAPSHOT ID. Every re-extraction makes a new snapshot, including
// one that only changed a surface material, and keying the study on the id
// re-ran preparation and display for a result that could not change (live
// 2026-10-31 17:38: a material edit, 0 dirty samples, 4.7 s refresh).
//
// What the result depends on: every element's triangles and their provenance
// (occlusion is BINARY, whatever the material -- glass casts shadow too, see
// SunStudyPreset.hpp), and, in the Late preset only, which faces are glass,
// because that is the receiver mask. Material otherwise is display: a material
// edit keeps the study and only re-tints (SunStudyDisplayAssembler).
//
// ⚠️ ONE KEY FOR THE STUDY AND FOR #98's SunStudyCleanupPlan. A cleanup plan
// keyed on anything else would invalidate on a different rule from the study it
// prunes.

#include "Geometry/Mesh.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

namespace evp::sunstudy {

namespace resultkey {

constexpr uint64_t kPrime = 0x100000001b3ull;

inline void Mix (uint64_t& hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    hash *= kPrime;
}

inline void MixDouble (uint64_t& hash, double value)
{
    uint64_t bits = 0;
    std::memcpy (&bits, &value, sizeof bits);
    Mix (hash, bits);
}

inline uint64_t MeshKey (const geomsrv::Mesh& mesh, bool glassOnly, const std::map<int32_t, double>& transparency,
                         double threshold)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : mesh.guid)
        Mix (hash, c);
    Mix (hash, mesh.vertices.size ());
    for (const double v : mesh.vertices)
        MixDouble (hash, v);
    Mix (hash, mesh.triangles.size ());
    for (const uint32_t i : mesh.triangles)
        Mix (hash, i);
    for (const int32_t b : mesh.triSourceBody)
        Mix (hash, uint64_t (uint32_t (b)));
    for (const int32_t p : mesh.triSourcePgon)
        Mix (hash, uint64_t (uint32_t (p)));
    if (glassOnly) {
        // The same test BuildSunStudyReceivers makes: unknown is NOT glass.
        const size_t faces = mesh.triangles.size () / 3;
        for (size_t face = 0; face < faces; ++face) {
            const auto found =
                face < mesh.triMaterial.size () ? transparency.find (mesh.triMaterial[face]) : transparency.end ();
            const bool glass =
                found != transparency.end () && std::isfinite (found->second) && found->second >= threshold;
            Mix (hash, glass ? 1u : 0u);
        }
    }
    return hash;
}

} // namespace resultkey

// The key -- pure. ORDER-INDEPENDENT across meshes, because an incremental
// snapshot lists its updated elements first and the rest after
// (SnapshotAssembly::Finish); per-face order within a mesh is kept.
inline uint64_t SunStudyGeometryKey (const geomsrv::Snapshot& snapshot, bool glassOnly, double threshold)
{
    uint64_t sum = 0, mixed = 0;
    for (const geomsrv::Mesh& mesh : snapshot.meshes) {
        const uint64_t key = resultkey::MeshKey (mesh, glassOnly, snapshot.materialTransparency, threshold);
        sum += key;
        mixed ^= key * resultkey::kPrime;
    }
    uint64_t hash = 1469598103934665603ull;
    resultkey::Mix (hash, snapshot.meshes.size ());
    resultkey::Mix (hash, sum);
    resultkey::Mix (hash, mixed);
    if (glassOnly)
        resultkey::MixDouble (hash, threshold);
    return hash == 0 ? 1 : hash; // 0 means "no snapshot" to the follower
}

} // namespace evp::sunstudy

#endif
