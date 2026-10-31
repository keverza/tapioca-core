#ifndef EVP_SUNSTUDY_SUNSTUDYRESULTKEY_HPP
#define EVP_SUNSTUDY_SUNSTUDYRESULTKEY_HPP

// SunStudy/SunStudyResultKey -- what of a snapshot a study's NUMBERS depend on (#97, #98).
//
// ⚠️ NOT THE SNAPSHOT ID. Every re-extraction makes a new snapshot, including
// one that only changed a surface material, and keying the study on the id
// re-ran preparation and display for a result that could not change (live
// 2026-10-31 17:38: a material edit, 0 dirty samples, 4.7 s refresh).
//
// What the result depends on: the triangles every element presents (occlusion
// is BINARY, whatever the material -- glass casts shadow too, see
// SunStudyPreset.hpp), and, in the Late preset only, which faces are glass,
// because that is the receiver mask. Material otherwise is display: a material
// edit keeps the study and only re-tints (SunStudyDisplayAssembler).
//
// ⚠️ THE SET OF TRIANGLES, NOT THEIR ORDER. Painting one face of a wall
// re-emitted the wall with its triangles in another order (live 18:42:36), so
// an order-sensitive key moved for a wall whose shape had not. Each triangle is
// hashed by its corner coordinates, rotated to start at its smallest corner
// (winding kept), and an element's key is the multiset of those. The retint
// maps the study's old face order onto the new one (MatchFaces).
//
// ⚠️ ONE KEY FOR THE STUDY AND FOR #98's SunStudyCleanupPlan. A cleanup plan
// keyed on anything else would invalidate on a different rule from the study it
// prunes.

#include "Geometry/Mesh.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

inline const double* Corner (const geomsrv::Mesh& mesh, size_t face, int corner)
{
    return &mesh.vertices[3 * size_t (mesh.triangles[3 * face + size_t (corner)])];
}

inline bool Less (const double* a, const double* b)
{
    for (int k = 0; k < 3; ++k)
        if (a[k] != b[k])
            return a[k] < b[k];
    return false;
}

// The corner a face's canonical rotation starts at: its smallest, so the same
// triangle from another extraction (other vertex indices, other first corner)
// reads the same, and the winding -- which side faces out -- is kept.
inline int FirstCorner (const geomsrv::Mesh& mesh, size_t face)
{
    int first = 0;
    for (int c = 1; c < 3; ++c)
        if (Less (Corner (mesh, face, c), Corner (mesh, face, first)))
            first = c;
    return first;
}

inline uint64_t TriangleKey (const geomsrv::Mesh& mesh, size_t face)
{
    uint64_t hash = 1469598103934665603ull;
    const int first = FirstCorner (mesh, face);
    for (int c = 0; c < 3; ++c) {
        const double* p = Corner (mesh, face, (first + c) % 3);
        for (int k = 0; k < 3; ++k)
            MixDouble (hash, p[k]);
    }
    return hash;
}

inline bool SameTriangle (const geomsrv::Mesh& a, size_t fa, const geomsrv::Mesh& b, size_t fb)
{
    const int ra = FirstCorner (a, fa), rb = FirstCorner (b, fb);
    for (int c = 0; c < 3; ++c) {
        const double* p = Corner (a, fa, (ra + c) % 3);
        const double* q = Corner (b, fb, (rb + c) % 3);
        for (int k = 0; k < 3; ++k)
            if (p[k] != q[k])
                return false;
    }
    return true;
}

inline bool Glass (const geomsrv::Mesh& mesh, size_t face, const std::map<int32_t, double>& transparency,
                   double threshold)
{
    // The same test BuildSunStudyReceivers makes: unknown is NOT glass.
    const auto found =
        face < mesh.triMaterial.size () ? transparency.find (mesh.triMaterial[face]) : transparency.end ();
    return found != transparency.end () && std::isfinite (found->second) && found->second >= threshold;
}

inline bool Valid (const geomsrv::Mesh& mesh)
{
    if (mesh.triangles.size () % 3 != 0)
        return false;
    const size_t vertices = mesh.vertices.size () / 3;
    for (const uint32_t i : mesh.triangles)
        if (i >= vertices)
            return false;
    return true;
}

} // namespace resultkey

// One element's two keys: the order-independent one the study compares, and
// the order-sensitive one, kept only so the log can say "reordered".
struct MeshResultKey {
    std::string guid;
    uint64_t set = 0;
    uint64_t order = 0;
    size_t triangles = 0;
};

inline MeshResultKey MeshKeyOf (const geomsrv::Mesh& mesh, bool glassOnly,
                                const std::map<int32_t, double>& transparency, double threshold)
{
    MeshResultKey key;
    key.guid = mesh.guid;
    key.triangles = mesh.triangles.size () / 3;
    uint64_t sum = 0, mixed = 0, order = 1469598103934665603ull;
    if (resultkey::Valid (mesh))
        for (size_t face = 0; face < key.triangles; ++face) {
            uint64_t tri = resultkey::TriangleKey (mesh, face);
            if (glassOnly)
                resultkey::Mix (tri, resultkey::Glass (mesh, face, transparency, threshold) ? 2u : 1u);
            sum += tri;
            mixed ^= tri * resultkey::kPrime;
            resultkey::Mix (order, tri);
        }
    uint64_t set = 1469598103934665603ull;
    for (const unsigned char c : mesh.guid)
        resultkey::Mix (set, c);
    resultkey::Mix (set, key.triangles);
    resultkey::Mix (set, sum);
    resultkey::Mix (set, mixed);
    key.set = set;
    key.order = order;
    return key;
}

inline std::vector<MeshResultKey> MeshKeys (const geomsrv::Snapshot& snapshot, bool glassOnly, double threshold)
{
    std::vector<MeshResultKey> keys;
    keys.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        keys.push_back (MeshKeyOf (mesh, glassOnly, snapshot.materialTransparency, threshold));
    return keys;
}

// The key -- pure. ORDER-INDEPENDENT across meshes too, because an incremental
// snapshot lists its updated elements first and the rest after
// (SnapshotAssembly::Finish).
inline uint64_t CombineMeshKeys (const std::vector<MeshResultKey>& keys, bool glassOnly, double threshold)
{
    uint64_t sum = 0, mixed = 0;
    for (const MeshResultKey& key : keys) {
        sum += key.set;
        mixed ^= key.set * resultkey::kPrime;
    }
    uint64_t hash = 1469598103934665603ull;
    resultkey::Mix (hash, keys.size ());
    resultkey::Mix (hash, sum);
    resultkey::Mix (hash, mixed);
    if (glassOnly)
        resultkey::MixDouble (hash, threshold);
    return hash == 0 ? 1 : hash; // 0 means "no snapshot" to the follower
}

inline uint64_t SunStudyGeometryKey (const geomsrv::Snapshot& snapshot, bool glassOnly, double threshold)
{
    return CombineMeshKeys (MeshKeys (snapshot, glassOnly, threshold), glassOnly, threshold);
}

// For each face of `later`, the face of `earlier` with the same triangle --
// pure. Empty when the two are not the same set of triangles (a re-triangulated
// face, a moved corner), which is the retint's cue to refuse.
inline std::vector<uint32_t> MatchFaces (const geomsrv::Mesh& earlier, const geomsrv::Mesh& later)
{
    const size_t faces = earlier.triangles.size () / 3;
    if (faces != later.triangles.size () / 3 || !resultkey::Valid (earlier) || !resultkey::Valid (later))
        return {};
    std::unordered_multimap<uint64_t, uint32_t> byKey;
    byKey.reserve (faces);
    for (size_t face = 0; face < faces; ++face)
        byKey.emplace (resultkey::TriangleKey (earlier, face), uint32_t (face));
    std::vector<uint32_t> map (faces);
    for (size_t face = 0; face < faces; ++face) {
        auto [first, last] = byKey.equal_range (resultkey::TriangleKey (later, face));
        auto hit = last;
        for (auto it = first; it != last; ++it)
            if (resultkey::SameTriangle (earlier, it->second, later, face)) {
                hit = it;
                break;
            }
        if (hit == last)
            return {};
        map[face] = hit->second;
        byKey.erase (hit); // each earlier face is used once
    }
    return map;
}

} // namespace evp::sunstudy

#endif
