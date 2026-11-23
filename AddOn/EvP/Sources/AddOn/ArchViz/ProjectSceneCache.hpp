#ifndef EVP_ARCHVIZ_PROJECTSCENECACHE_HPP
#define EVP_ARCHVIZ_PROJECTSCENECACHE_HPP

// ArchViz/ProjectSceneCache -- the viewer's per-project scene, kept on disk between sessions (#14).
//
// WHY. Every viewer open re-read the whole model through Archicad's per-polygon API on the main
// thread: ~84 s for one full pass on a real project (2026-11-21). Edits after that are cheap; the
// open is not, and live capture alone cannot make it so. This file holds what a finished pass
// produced -- each element's mesh, its substance observations and a fingerprint, the surface pool
// it was numbered against, the material table and the sun -- so the next open can draw it at once
// and the pass re-read only what changed.
//
// ⚠️ PURE: no ACAPI, no ModelerAPI, no Diligent, so tests/cpp pins the format, the pool remap, the
// match rule and the reconciliation. The readings come from ArchViz/SceneCacheFingerprint (main
// thread); the orchestration is ArchViz/ExtractionSceneCache (the extraction worker).
//
// ⚠️ A FINGERPRINT HAS TWO TIERS, AND REUSE NEEDS ONE OF THEM TO AGREE IN FULL.
//   cheap -- the element's generation id (ModelerAPI::Element::GetGenId) and its bodies' vertex,
//            polygon and edge counts: what the content sweep already reads, ~1.4 us an element.
//   deep  -- every body vertex, quantised to 10 um, and every polygon's SURFACE IDENTITY (name and
//            external reference, never the pool index, which Archicad renumbers): read only when
//            the counts agree and the generation id does not.
// Whether a generation id survives closing and reopening a project is NOT KNOWN; the deep tier is
// what keeps a reopen correct either way, and every pass logs which tier carried each element.
//
// ⚠️ THE POOL IS RENUMBERED, SO A CACHED INDEX IS MAPPED BY IDENTITY BEFORE IT IS DRAWN. An element
// carrying an index whose identity is missing, or not unique, in the live pool is re-captured,
// never guessed (SceneCmdQueue.hpp: the right building in another surface's colours).
//
// ⚠️ THE FILE IS REFUSED WHOLE on a wrong magic, another format version, a payload shorter than its
// header says, a checksum that disagrees, or another project's identity. A refused file is
// deleted and rebuilt by the next complete pass; nothing in it is half-trusted.

#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/SceneCmdQueue.hpp" // EnvironmentUpload
#include "ArchViz/SubstanceJoin.hpp" // SurfaceSubstanceObservation
#include "Geometry/Mesh.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace scenecache {

// Bump on ANY change to what Encode writes. An older or newer file is refused, never migrated.
constexpr uint32_t kFormatVersion = 2;
// A project bigger than this is not cached; all projects together are kept under the total, the
// least recently used file going first.
constexpr uint64_t kMaxFileBytes = 1ull << 30;
constexpr uint64_t kMaxTotalBytes = 2ull << 30;
// The deep tier's vertex quantum, metres.
constexpr double kDeepQuantumM = 1e-5;

// FNV-1a, 64 bit: the payload checksum, the fingerprints' hash and the file name.
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
inline uint64_t Fnv (const void* data, size_t bytes, uint64_t hash = kFnvOffset)
{
    const auto* p = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= kFnvPrime;
    }
    return hash;
}
template <typename T> uint64_t FnvValue (const T& value, uint64_t hash)
{
    return Fnv (&value, sizeof (T), hash);
}
inline uint64_t FnvString (const std::string& text, uint64_t hash)
{
    hash = FnvValue (uint64_t (text.size ()), hash);
    return Fnv (text.data (), text.size (), hash);
}

struct Fingerprint {
    uint32_t genId = 0;
    uint64_t shape = 0; // cheap tier: the bodies' counts
    uint64_t deep = 0;  // deep tier; 0 = never read
    bool operator== (const Fingerprint&) const = default;
};

// How a cached element compares with what the live model holds at its GUID -- pure.
enum class Match : uint8_t {
    None,     // re-capture
    Cheap,    // the counts and the generation id agree: reuse
    NeedDeep, // the counts agree, the generation id does not: read the deep tier and ask again
    Deep,     // the deep tier agrees: reuse
};
inline Match MatchCheap (const Fingerprint& cached, uint32_t genId, uint64_t shape)
{
    if (cached.shape != shape)
        return Match::None;
    if (cached.genId == genId)
        return Match::Cheap;
    return cached.deep != 0 ? Match::NeedDeep : Match::None;
}
inline Match MatchDeep (const Fingerprint& cached, uint64_t deep)
{
    return deep != 0 && cached.deep == deep ? Match::Deep : Match::None;
}

// What a cached element carries beside its mesh.
struct ElementRecord {
    Fingerprint fingerprint;
    std::vector<SurfaceSubstanceObservation> observations; // in `geometry`'s pool numbering
};

// One project's cached scene. `geometry` is a complete capture whose meshes are numbered against
// `geometry->materialIdentity`; `records` has one entry per mesh GUID.
struct State {
    std::string project;
    std::shared_ptr<const Snapshot> geometry;
    std::map<std::string, ElementRecord> records;
    std::shared_ptr<const MaterialTable> materials; // with its texture pixels; may be null
    bool haveEnvironment = false;
    EnvironmentUpload environment;
};

enum class DecodeStatus : uint8_t { Ok, BadMagic, WrongVersion, Truncated, Corrupt, WrongProject };
const char* DescribeStatus (DecodeStatus status);

// The whole state as bytes, header first. Little-endian, as written (x64 Windows only).
std::string Encode (const State& state);
// `expectedProject` empty accepts any project (tests and diagnostics only).
DecodeStatus Decode (const std::string& bytes, const std::string& expectedProject, State& out);

// ---- the pool remap ------------------------------------------------------------------------------
// Cached pool index -> live pool index, by identity. `identity` when every cached index maps to
// itself (the common case: nothing renumbered).
struct PoolRemap {
    std::map<int32_t, int32_t> to;
    std::set<int32_t> unmapped; // cached indices whose identity is missing or ambiguous live
    bool identity = true;
};
PoolRemap BuildPoolRemap (const std::map<int32_t, CapturedMaterialIdentity>& cached,
                          const std::map<int32_t, CapturedMaterialIdentity>& live);
// Whether every pool index the element uses can be mapped. An index the cached pool never held
// (0, a generated colour) is not a pool reference and passes through unchanged.
bool CanRemap (const Mesh& mesh, const ElementRecord& record, const PoolRemap& remap);
// In place; only after CanRemap said yes.
void Remap (Mesh& mesh, std::vector<SurfaceSubstanceObservation>& observations, const PoolRemap& remap);

// ---- reconciliation ------------------------------------------------------------------------------
// What a reconciled pass did with each element, by GUID -- pure, so tests pin the four cases.
struct Reconciliation {
    std::vector<std::string> reused;   // cached, fingerprint agreed, not captured
    std::vector<std::string> changed;  // cached, fingerprint disagreed, captured again
    std::vector<std::string> appeared; // not cached, captured
    std::vector<std::string> stale;    // cached, not in the model any more
    bool Any () const
    {
        return !changed.empty () || !appeared.empty () || !stale.empty ();
    }
};
Reconciliation Reconcile (const std::set<std::string>& cached, const std::vector<std::string>& reused,
                          const std::vector<std::string>& captured);

// The slice's question, read-only while a pass runs: the cached fingerprint of every element whose
// pool indices can be remapped.
using ReuseIndex = std::unordered_map<std::string, Fingerprint>;

// ---- the store -----------------------------------------------------------------------------------
// The file a project's cache lives in, under `directory`: its identity hashed, so a path never
// becomes a file name. The identity itself is in the header and checked on read.
std::filesystem::path FileFor (const std::filesystem::path& directory, const std::string& project);
// Temp file, then rename over the old one: a reader never sees half a file.
bool WriteAtomic (const std::filesystem::path& file, const std::string& bytes, std::string& error);
bool ReadWhole (const std::filesystem::path& file, std::string& bytes);
// Delete the least recently written cache files until the directory holds at most `maxTotal`
// bytes, never `keep`. Returns how many went.
size_t Evict (const std::filesystem::path& directory, uint64_t maxTotal, const std::filesystem::path& keep);

} // namespace scenecache
} // namespace archviz
} // namespace geomsrv

#endif
