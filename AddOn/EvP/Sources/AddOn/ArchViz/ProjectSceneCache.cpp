// ArchViz/ProjectSceneCache -- see the header.

#include "ArchViz/ProjectSceneCache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <type_traits>

namespace geomsrv {
namespace archviz {
namespace scenecache {

namespace {

constexpr char kMagic[8] = { 'T', 'P', 'S', 'C', 'A', 'C', 'H', 'E' };
// magic, version, reserved, payload bytes, payload checksum
constexpr size_t kHeaderBytes = sizeof (kMagic) + 4 + 4 + 8 + 8;

class Writer {
  public:
    template <typename T> void Pod (const T& value)
    {
        static_assert (std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const char*> (&value);
        out_.append (p, sizeof (T));
    }
    void Str (const std::string& text)
    {
        Pod (uint64_t (text.size ()));
        out_.append (text);
    }
    template <typename T> void Vec (const std::vector<T>& values)
    {
        static_assert (std::is_trivially_copyable_v<T>);
        Pod (uint64_t (values.size ()));
        if (!values.empty ())
            out_.append (reinterpret_cast<const char*> (values.data ()), values.size () * sizeof (T));
    }
    std::string& Bytes ()
    {
        return out_;
    }

  private:
    std::string out_;
};

// ⚠️ EVERY READ IS BOUNDS-CHECKED, AND A COUNT IS CHECKED AGAINST WHAT IS LEFT BEFORE ANYTHING IS
// ALLOCATED: a corrupt length must refuse the file, not ask for 2^60 bytes.
class Reader {
  public:
    Reader (const char* data, size_t size) : p_ (data), end_ (data + size) {}
    template <typename T> bool Pod (T& value)
    {
        static_assert (std::is_trivially_copyable_v<T>);
        if (size_t (end_ - p_) < sizeof (T))
            return false;
        std::memcpy (&value, p_, sizeof (T));
        p_ += sizeof (T);
        return true;
    }
    bool Str (std::string& text)
    {
        uint64_t n = 0;
        if (!Pod (n) || n > uint64_t (end_ - p_))
            return false;
        text.assign (p_, size_t (n));
        p_ += n;
        return true;
    }
    template <typename T> bool Vec (std::vector<T>& values)
    {
        uint64_t n = 0;
        if (!Pod (n) || n > uint64_t (end_ - p_) / sizeof (T))
            return false;
        values.resize (size_t (n));
        if (n != 0)
            std::memcpy (values.data (), p_, size_t (n) * sizeof (T));
        p_ += n * sizeof (T);
        return true;
    }
    // A count of items at least `minBytes` each, checked against what is left.
    bool Count (uint32_t& n, size_t minBytes)
    {
        return Pod (n) && uint64_t (n) * minBytes <= uint64_t (end_ - p_);
    }
    bool AtEnd () const
    {
        return p_ == end_;
    }

  private:
    const char* p_;
    const char* end_;
};

void WriteMaterial (Writer& w, const SurfaceMaterial& m)
{
    w.Pod (m.index);
    w.Pod (m.r);
    w.Pod (m.g);
    w.Pod (m.b);
    w.Pod (m.alpha);
    w.Pod (m.shininess);
    w.Pod (m.specular);
    w.Pod (m.diffuse);
    w.Pod (m.specularR);
    w.Pod (m.specularG);
    w.Pod (m.specularB);
    w.Pod (int32_t (m.substance));
    w.Pod (m.substanceConfidence);
    w.Pod (m.textureKey);
    w.Pod (m.textureSizeX);
    w.Pod (m.textureSizeY);
    w.Pod (uint8_t (m.textureMirrorX));
    w.Pod (uint8_t (m.textureMirrorY));
    w.Pod (m.textureRotation.cosA);
    w.Pod (m.textureRotation.sinA);
    w.Str (m.name);
}

bool ReadMaterial (Reader& r, SurfaceMaterial& m)
{
    int32_t substance = 0;
    uint8_t mirrorX = 0, mirrorY = 0;
    const bool ok = r.Pod (m.index) && r.Pod (m.r) && r.Pod (m.g) && r.Pod (m.b) && r.Pod (m.alpha) &&
                    r.Pod (m.shininess) && r.Pod (m.specular) && r.Pod (m.diffuse) && r.Pod (m.specularR) &&
                    r.Pod (m.specularG) && r.Pod (m.specularB) && r.Pod (substance) && r.Pod (m.substanceConfidence) &&
                    r.Pod (m.textureKey) && r.Pod (m.textureSizeX) && r.Pod (m.textureSizeY) && r.Pod (mirrorX) &&
                    r.Pod (mirrorY) && r.Pod (m.textureRotation.cosA) && r.Pod (m.textureRotation.sinA) &&
                    r.Str (m.name);
    m.substance = Substance (substance);
    m.textureMirrorX = mirrorX != 0;
    m.textureMirrorY = mirrorY != 0;
    return ok;
}

void WriteEnvironment (Writer& w, const EnvironmentUpload& e)
{
    w.Pod (e.sunX);
    w.Pod (e.sunY);
    w.Pod (e.sunZ);
    w.Pod (e.ambient);
    w.Pod (uint8_t (e.sunBelowHorizon));
    w.Pod (e.northDegrees);
    w.Pod (e.bearingDegrees);
    w.Pod (e.latitudeDegrees);
    w.Pod (e.longitudeDegrees);
    w.Pod (e.altitudeMetres);
    w.Pod (e.year);
    w.Pod (e.month);
    w.Pod (e.day);
    w.Pod (e.hour);
    w.Pod (e.minute);
    w.Pod (uint8_t (e.summerTime));
    w.Pod (e.timeZoneMinutes);
    w.Pod (uint8_t (e.haveComputedSun));
    w.Pod (e.computedAzimuthDegrees);
    w.Pod (e.computedAltitudeDegrees);
}

bool ReadEnvironment (Reader& r, EnvironmentUpload& e)
{
    uint8_t below = 0, summer = 0, computed = 0;
    const bool ok = r.Pod (e.sunX) && r.Pod (e.sunY) && r.Pod (e.sunZ) && r.Pod (e.ambient) && r.Pod (below) &&
                    r.Pod (e.northDegrees) && r.Pod (e.bearingDegrees) && r.Pod (e.latitudeDegrees) &&
                    r.Pod (e.longitudeDegrees) && r.Pod (e.altitudeMetres) && r.Pod (e.year) && r.Pod (e.month) &&
                    r.Pod (e.day) && r.Pod (e.hour) && r.Pod (e.minute) && r.Pod (summer) &&
                    r.Pod (e.timeZoneMinutes) && r.Pod (computed) && r.Pod (e.computedAzimuthDegrees) &&
                    r.Pod (e.computedAltitudeDegrees);
    e.sunBelowHorizon = below != 0;
    e.summerTime = summer != 0;
    e.haveComputedSun = computed != 0;
    return ok;
}

void WriteMesh (Writer& w, const Mesh& m)
{
    w.Str (m.guid);
    w.Pod (m.elemType);
    w.Vec (m.vertices);
    w.Vec (m.normals);
    w.Vec (m.triangles);
    w.Vec (m.triMaterial);
    w.Vec (m.uvs);
    w.Vec (m.triWireEdges);
    w.Vec (m.triSourceBody);
    w.Vec (m.triSourcePgon);
    w.Vec (m.triSurfaceAttribute);
    for (int k = 0; k < 3; ++k) {
        w.Pod (m.bounds.mn[k]);
        w.Pod (m.bounds.mx[k]);
    }
}

bool ReadMesh (Reader& r, Mesh& m)
{
    bool ok = r.Str (m.guid) && r.Pod (m.elemType) && r.Vec (m.vertices) && r.Vec (m.normals) &&
              r.Vec (m.triangles) && r.Vec (m.triMaterial) && r.Vec (m.uvs) && r.Vec (m.triWireEdges) &&
              r.Vec (m.triSourceBody) && r.Vec (m.triSourcePgon) && r.Vec (m.triSurfaceAttribute);
    for (int k = 0; ok && k < 3; ++k)
        ok = r.Pod (m.bounds.mn[k]) && r.Pod (m.bounds.mx[k]);
    if (!ok)
        return false;
    // ⚠️ A MESH THAT INDEXES PAST ITS OWN VERTICES IS CORRUPT, checksum or not: it would be read out
    // of bounds by every consumer downstream.
    const size_t vertices = m.vertices.size () / 3;
    if (m.vertices.size () % 3 != 0 || m.triangles.size () % 3 != 0)
        return false;
    for (uint32_t index : m.triangles)
        if (index >= vertices)
            return false;
    return m.triMaterial.size () == m.triangles.size () / 3;
}

std::string IdentityKey (const CapturedMaterialIdentity& identity)
{
    return identity.name + '\x1f' + std::to_string (identity.externalReference);
}

} // namespace

const char* DescribeStatus (DecodeStatus status)
{
    switch (status) {
        case DecodeStatus::Ok:
            return "ok";
        case DecodeStatus::BadMagic:
            return "not a scene cache file";
        case DecodeStatus::WrongVersion:
            return "another format version";
        case DecodeStatus::Truncated:
            return "shorter than its header says";
        case DecodeStatus::Corrupt:
            return "checksum or contents do not agree";
        case DecodeStatus::WrongProject:
            return "another project's cache";
    }
    return "unknown";
}

std::string Encode (const State& state)
{
    Writer w;
    w.Str (state.project);

    w.Pod (uint8_t (state.haveEnvironment));
    WriteEnvironment (w, state.environment);

    const std::vector<SurfaceMaterial> none;
    const auto& materials = state.materials != nullptr ? state.materials->All () : none;
    w.Pod (uint32_t (materials.size ()));
    for (const SurfaceMaterial& m : materials)
        WriteMaterial (w, m);
    std::vector<std::pair<uint64_t, TexturePixelsPtr>> textures;
    if (state.materials != nullptr)
        for (const auto& [key, pixels] : state.materials->Textures ())
            if (pixels != nullptr && pixels->Valid ())
                textures.emplace_back (key, pixels);
    w.Pod (uint32_t (textures.size ()));
    for (const auto& [key, pixels] : textures) {
        w.Pod (key);
        w.Pod (pixels->width);
        w.Pod (pixels->height);
        w.Pod (uint8_t (pixels->hasAlpha));
        w.Str (pixels->name);
        w.Vec (pixels->rgba);
    }

    static const Snapshot empty;
    const Snapshot& geometry = state.geometry != nullptr ? *state.geometry : empty;
    w.Pod (geometry.captureStamp);
    w.Pod (uint32_t (geometry.materialIdentity.size ()));
    for (const auto& [index, identity] : geometry.materialIdentity) {
        w.Pod (index);
        w.Str (identity.name);
        w.Pod (identity.externalReference);
    }
    w.Pod (uint32_t (geometry.materialTransparency.size ()));
    for (const auto& [index, transparency] : geometry.materialTransparency) {
        w.Pod (index);
        w.Pod (transparency);
    }
    w.Pod (uint32_t (geometry.surfaceAttributes.size ()));
    for (const auto& [index, surface] : geometry.surfaceAttributes) {
        w.Pod (index);
        w.Str (surface.name);
        w.Str (surface.guid);
        w.Pod (surface.rgba);
    }

    w.Pod (uint32_t (geometry.meshes.size ()));
    for (const Mesh& mesh : geometry.meshes) {
        WriteMesh (w, mesh);
        const auto found = state.records.find (mesh.guid);
        const ElementRecord record = found != state.records.end () ? found->second : ElementRecord {};
        w.Pod (record.fingerprint.genId);
        w.Pod (record.fingerprint.shape);
        w.Pod (record.fingerprint.deep);
        w.Pod (uint32_t (record.observations.size ()));
        for (const SurfaceSubstanceObservation& o : record.observations) {
            w.Pod (o.surface);
            w.Pod (int32_t (o.substance));
            w.Pod (o.weight);
        }
    }

    const std::string& payload = w.Bytes ();
    std::string out;
    out.reserve (kHeaderBytes + payload.size ());
    out.append (kMagic, sizeof (kMagic));
    const uint32_t version = kFormatVersion, reserved = 0;
    const uint64_t size = payload.size (), checksum = Fnv (payload.data (), payload.size ());
    out.append (reinterpret_cast<const char*> (&version), 4);
    out.append (reinterpret_cast<const char*> (&reserved), 4);
    out.append (reinterpret_cast<const char*> (&size), 8);
    out.append (reinterpret_cast<const char*> (&checksum), 8);
    out.append (payload);
    return out;
}

DecodeStatus Decode (const std::string& bytes, const std::string& expectedProject, State& out)
{
    if (bytes.size () < sizeof (kMagic) || std::memcmp (bytes.data (), kMagic, sizeof (kMagic)) != 0)
        return DecodeStatus::BadMagic;
    if (bytes.size () < kHeaderBytes)
        return DecodeStatus::Truncated;
    uint32_t version = 0;
    uint64_t size = 0, checksum = 0;
    std::memcpy (&version, bytes.data () + 8, 4);
    std::memcpy (&size, bytes.data () + 16, 8);
    std::memcpy (&checksum, bytes.data () + 24, 8);
    if (version != kFormatVersion)
        return DecodeStatus::WrongVersion;
    if (bytes.size () - kHeaderBytes < size)
        return DecodeStatus::Truncated;
    if (bytes.size () - kHeaderBytes != size)
        return DecodeStatus::Corrupt;
    const char* payload = bytes.data () + kHeaderBytes;
    if (Fnv (payload, size_t (size)) != checksum)
        return DecodeStatus::Corrupt;

    Reader r (payload, size_t (size));
    State state;
    if (!r.Str (state.project))
        return DecodeStatus::Corrupt;
    if (!expectedProject.empty () && state.project != expectedProject)
        return DecodeStatus::WrongProject;

    uint8_t haveEnvironment = 0;
    if (!r.Pod (haveEnvironment) || !ReadEnvironment (r, state.environment))
        return DecodeStatus::Corrupt;
    state.haveEnvironment = haveEnvironment != 0;

    auto materials = std::make_shared<MaterialTable> ();
    uint32_t count = 0;
    if (!r.Count (count, 4))
        return DecodeStatus::Corrupt;
    for (uint32_t i = 0; i < count; ++i) {
        SurfaceMaterial m;
        if (!ReadMaterial (r, m))
            return DecodeStatus::Corrupt;
        materials->Set (m);
    }
    if (!r.Count (count, 8))
        return DecodeStatus::Corrupt;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        uint8_t hasAlpha = 0;
        auto pixels = std::make_shared<TexturePixels> ();
        if (!r.Pod (key) || !r.Pod (pixels->width) || !r.Pod (pixels->height) || !r.Pod (hasAlpha) ||
            !r.Str (pixels->name) || !r.Vec (pixels->rgba))
            return DecodeStatus::Corrupt;
        pixels->hasAlpha = hasAlpha != 0;
        if (!pixels->Valid ())
            return DecodeStatus::Corrupt;
        materials->SetTexture (key, std::move (pixels));
    }
    state.materials = std::move (materials);

    auto geometry = std::make_shared<Snapshot> ();
    if (!r.Pod (geometry->captureStamp) || !r.Count (count, 4))
        return DecodeStatus::Corrupt;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t index = 0;
        CapturedMaterialIdentity identity;
        if (!r.Pod (index) || !r.Str (identity.name) || !r.Pod (identity.externalReference))
            return DecodeStatus::Corrupt;
        geometry->materialIdentity.emplace (index, std::move (identity));
    }
    if (!r.Count (count, 12))
        return DecodeStatus::Corrupt;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t index = 0;
        double transparency = 0.0;
        if (!r.Pod (index) || !r.Pod (transparency))
            return DecodeStatus::Corrupt;
        geometry->materialTransparency.emplace (index, transparency);
    }
    if (!r.Count (count, 4))
        return DecodeStatus::Corrupt;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t index = 0;
        CapturedSurface surface;
        if (!r.Pod (index) || !r.Str (surface.name) || !r.Str (surface.guid) || !r.Pod (surface.rgba))
            return DecodeStatus::Corrupt;
        geometry->surfaceAttributes.emplace (index, std::move (surface));
    }

    if (!r.Count (count, 8))
        return DecodeStatus::Corrupt;
    geometry->meshes.reserve (count);
    for (uint32_t i = 0; i < count; ++i) {
        Mesh mesh;
        ElementRecord record;
        uint32_t observations = 0;
        if (!ReadMesh (r, mesh) || !r.Pod (record.fingerprint.genId) || !r.Pod (record.fingerprint.shape) ||
            !r.Pod (record.fingerprint.deep) || !r.Count (observations, 16))
            return DecodeStatus::Corrupt;
        record.observations.resize (observations);
        for (SurfaceSubstanceObservation& o : record.observations) {
            int32_t substance = 0;
            if (!r.Pod (o.surface) || !r.Pod (substance) || !r.Pod (o.weight))
                return DecodeStatus::Corrupt;
            o.substance = Substance (substance);
        }
        // ⚠️ ONE ENTRY PER GUID: a duplicate would make the scene and the records disagree.
        if (mesh.guid.empty () || !state.records.emplace (mesh.guid, std::move (record)).second)
            return DecodeStatus::Corrupt;
        geometry->meshes.push_back (std::move (mesh));
    }
    if (!r.AtEnd ())
        return DecodeStatus::Corrupt;
    geometry->completeModel = true;
    state.geometry = std::move (geometry);
    out = std::move (state);
    return DecodeStatus::Ok;
}

PoolRemap BuildPoolRemap (const std::map<int32_t, CapturedMaterialIdentity>& cached,
                          const std::map<int32_t, CapturedMaterialIdentity>& live)
{
    // ⚠️ AN IDENTITY THAT TWO LIVE ENTRIES SHARE IS NO IDENTITY: mapping to either would be a guess.
    std::map<std::string, int32_t> byKey;
    std::set<std::string> ambiguous;
    for (const auto& [index, identity] : live) {
        const std::string key = IdentityKey (identity);
        if (!byKey.emplace (key, index).second)
            ambiguous.insert (key);
    }
    PoolRemap remap;
    for (const auto& [index, identity] : cached) {
        const std::string key = IdentityKey (identity);
        const auto found = byKey.find (key);
        if (found == byKey.end () || ambiguous.count (key) != 0) {
            remap.unmapped.insert (index);
            remap.identity = false;
            continue;
        }
        remap.to.emplace (index, found->second);
        if (found->second != index)
            remap.identity = false;
    }
    return remap;
}

bool CanRemap (const Mesh& mesh, const ElementRecord& record, const PoolRemap& remap)
{
    if (remap.unmapped.empty ())
        return true;
    for (int32_t index : mesh.triMaterial)
        if (remap.unmapped.count (index) != 0)
            return false;
    for (const SurfaceSubstanceObservation& o : record.observations)
        if (remap.unmapped.count (o.surface) != 0)
            return false;
    return true;
}

void Remap (Mesh& mesh, std::vector<SurfaceSubstanceObservation>& observations, const PoolRemap& remap)
{
    if (remap.identity)
        return;
    const auto map = [&remap] (int32_t index) {
        const auto found = remap.to.find (index);
        return found != remap.to.end () ? found->second : index;
    };
    for (int32_t& index : mesh.triMaterial)
        index = map (index);
    for (SurfaceSubstanceObservation& o : observations)
        o.surface = map (o.surface);
}

Reconciliation Reconcile (const std::set<std::string>& cached, const std::vector<std::string>& reused,
                          const std::vector<std::string>& captured)
{
    Reconciliation result;
    std::set<std::string> visited;
    for (const std::string& guid : reused)
        if (visited.insert (guid).second)
            result.reused.push_back (guid);
    for (const std::string& guid : captured) {
        if (!visited.insert (guid).second)
            continue;
        (cached.count (guid) != 0 ? result.changed : result.appeared).push_back (guid);
    }
    for (const std::string& guid : cached)
        if (visited.count (guid) == 0)
            result.stale.push_back (guid);
    return result;
}

std::filesystem::path FileFor (const std::filesystem::path& directory, const std::string& project)
{
    char name[32] = {};
    std::snprintf (name, sizeof (name), "%016llx.tsc",
                   static_cast<unsigned long long> (FnvString (project, kFnvOffset)));
    return directory / name;
}

bool WriteAtomic (const std::filesystem::path& file, const std::string& bytes, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories (file.parent_path (), ec);
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out (temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create " + temp.string ();
            return false;
        }
        out.write (bytes.data (), std::streamsize (bytes.size ()));
        if (!out) {
            error = "short write to " + temp.string ();
            out.close ();
            std::filesystem::remove (temp, ec);
            return false;
        }
    }
    std::filesystem::rename (temp, file, ec); // replaces an existing file (MoveFileEx on Windows)
    if (ec) {
        error = "cannot replace " + file.string () + ": " + ec.message ();
        std::filesystem::remove (temp, ec);
        return false;
    }
    return true;
}

bool ReadWhole (const std::filesystem::path& file, std::string& bytes)
{
    std::ifstream in (file, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamoff size = in.tellg ();
    if (size < 0)
        return false;
    bytes.resize (size_t (size));
    in.seekg (0);
    in.read (bytes.data (), size);
    return bool (in);
}

size_t Evict (const std::filesystem::path& directory, uint64_t maxTotal, const std::filesystem::path& keep)
{
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type written;
        uint64_t bytes = 0;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator (directory, ec)) {
        if (!item.is_regular_file (ec) || item.path ().extension () != ".tsc")
            continue;
        Entry e { item.path (), item.last_write_time (ec), item.file_size (ec) };
        total += e.bytes;
        entries.push_back (std::move (e));
    }
    std::sort (entries.begin (), entries.end (), [] (const Entry& a, const Entry& b) { return a.written < b.written; });
    size_t removed = 0;
    for (const Entry& e : entries) {
        if (total <= maxTotal)
            break;
        if (std::filesystem::equivalent (e.path, keep, ec))
            continue;
        if (std::filesystem::remove (e.path, ec)) {
            total -= e.bytes;
            ++removed;
        }
    }
    return removed;
}

} // namespace scenecache
} // namespace archviz
} // namespace geomsrv
