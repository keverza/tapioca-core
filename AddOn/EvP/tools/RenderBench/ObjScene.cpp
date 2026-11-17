#include "ObjScene.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace renderbench {

namespace av = geomsrv::archviz;

namespace {

struct MtlEntry {
    float kd[3] = { 0.8f, 0.8f, 0.8f };
    float ks[3] = { 0.0f, 0.0f, 0.0f };
    float ke[3] = { 0.0f, 0.0f, 0.0f };
    float ns = 0.0f;
    float d = 1.0f;
};

bool ReadFile (const std::string& path, std::string& bytes)
{
    std::ifstream file (path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream all;
    all << file.rdbuf ();
    bytes = all.str ();
    return true;
}

std::string DirectoryOf (const std::string& path)
{
    const size_t slash = path.find_last_of ("\\/");
    return slash == std::string::npos ? std::string () : path.substr (0, slash + 1);
}

const char* SkipSpace (const char* p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    return p;
}

const char* ReadFloat (const char* p, const char* end, float& value)
{
    p = SkipSpace (p, end);
    const auto result = std::from_chars (p, end, value);
    if (result.ec != std::errc ()) {
        // from_chars refuses a leading '+' and a bare ".5"-style token on some runtimes; strtof
        // takes both. The line is still bounded by `end` through the newline check below.
        char* stop = nullptr;
        value = std::strtof (p, &stop);
        return stop != nullptr && stop > p ? stop : p;
    }
    return result.ptr;
}

std::string RestOfLine (const char* p, const char* end)
{
    p = SkipSpace (p, end);
    const char* e = p;
    while (e < end && *e != '\n' && *e != '\r' && *e != '#')
        ++e;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        --e;
    return std::string (p, e);
}

void ParseMtl (const std::string& path, std::map<std::string, MtlEntry>& out)
{
    std::string bytes;
    if (!ReadFile (path, bytes))
        return;
    std::istringstream lines (bytes);
    std::string line;
    MtlEntry* current = nullptr;
    while (std::getline (lines, line)) {
        std::istringstream in (line);
        std::string key;
        in >> key;
        if (key == "newmtl") {
            std::string name;
            std::getline (in, name);
            const size_t a = name.find_first_not_of (" \t");
            const size_t b = name.find_last_not_of (" \t\r");
            name = a == std::string::npos ? std::string () : name.substr (a, b - a + 1);
            current = &out[name];
        }
        else if (current == nullptr)
            continue;
        else if (key == "Kd")
            in >> current->kd[0] >> current->kd[1] >> current->kd[2];
        else if (key == "Ks")
            in >> current->ks[0] >> current->ks[1] >> current->ks[2];
        else if (key == "Ke")
            in >> current->ke[0] >> current->ke[1] >> current->ke[2];
        else if (key == "Ns")
            in >> current->ns;
        else if (key == "d")
            in >> current->d;
        else if (key == "Tr") {
            float tr = 0.0f;
            in >> tr;
            current->d = 1.0f - tr;
        }
    }
}

// Archicad's channels from an MTL entry; see the header.
av::SurfaceMaterial ToSurface (int32_t index, const std::string& name, const MtlEntry& m)
{
    av::SurfaceMaterial s;
    s.index = index;
    s.name = name;
    s.r = m.kd[0];
    s.g = m.kd[1];
    s.b = m.kd[2];
    if (m.ke[0] + m.ke[1] + m.ke[2] > 0.0f) {
        // No emitters in the viewer: an emitter reads as a bright, plain surface.
        const float peak = std::max ({ m.ke[0], m.ke[1], m.ke[2] });
        s.r = m.ke[0] / peak;
        s.g = m.ke[1] / peak;
        s.b = m.ke[2] / peak;
    }
    s.alpha = std::clamp (m.d, 0.0f, 1.0f);
    s.shininess = std::clamp (m.ns / av::kMaxShininessFactor, 0.0f, 1.0f);
    const float ksPeak = std::max ({ m.ks[0], m.ks[1], m.ks[2] });
    s.specular = std::clamp (ksPeak, 0.0f, 1.0f);
    if (ksPeak > 0.0f) {
        s.specularR = m.ks[0] / ksPeak;
        s.specularG = m.ks[1] / ksPeak;
        s.specularB = m.ks[2] / ksPeak;
    }
    s.diffuse = std::clamp (std::max ({ m.kd[0], m.kd[1], m.kd[2] }), 0.0f, 1.0f);
    return s;
}

struct Builder {
    std::unique_ptr<av::ElementUpload> element;
    std::unordered_map<uint64_t, uint32_t> welded;
};

} // namespace

bool LoadObjScene (const std::string& path, bool yUp, float scale, ObjScene& out, std::string& error)
{
    std::string bytes;
    if (!ReadFile (path, bytes)) {
        error = "cannot read " + path;
        return false;
    }
    std::vector<float> positions;
    std::vector<float> normals;
    std::map<std::string, MtlEntry> library;
    std::map<std::string, int32_t> materialIndex;
    std::vector<Builder> builders;
    out.materials = std::make_unique<av::MaterialTable> ();
    out.materialNames.clear ();

    auto materialFor = [&] (const std::string& name) -> int32_t {
        auto found = materialIndex.find (name);
        if (found != materialIndex.end ())
            return found->second;
        const int32_t index = int32_t (materialIndex.size ());
        materialIndex.emplace (name, index);
        out.materialNames.push_back (name);
        builders.emplace_back ();
        return index;
    };
    int32_t current = materialFor ("(default)");

    auto convert = [&] (const float in[3], float o[3], float s) {
        if (yUp) {
            o[0] = in[0] * s;
            o[1] = -in[2] * s;
            o[2] = in[1] * s;
        }
        else {
            o[0] = in[0] * s;
            o[1] = in[1] * s;
            o[2] = in[2] * s;
        }
    };

    std::vector<int64_t> faceV;
    std::vector<int64_t> faceN;
    const char* p = bytes.data ();
    const char* end = p + bytes.size ();
    while (p < end) {
        const char* lineEnd = static_cast<const char*> (std::memchr (p, '\n', size_t (end - p)));
        if (lineEnd == nullptr)
            lineEnd = end;
        const char* q = SkipSpace (p, lineEnd);
        if (q + 1 < lineEnd && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t')) {
            float v[3] = {};
            const char* r = q + 1;
            for (float& c : v)
                r = ReadFloat (r, lineEnd, c);
            float o[3];
            convert (v, o, scale);
            positions.insert (positions.end (), o, o + 3);
        }
        else if (q + 2 < lineEnd && q[0] == 'v' && q[1] == 'n') {
            float v[3] = {};
            const char* r = q + 2;
            for (float& c : v)
                r = ReadFloat (r, lineEnd, c);
            float o[3];
            convert (v, o, 1.0f);
            normals.insert (normals.end (), o, o + 3);
        }
        else if (q + 1 < lineEnd && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t')) {
            faceV.clear ();
            faceN.clear ();
            const char* r = q + 1;
            while (true) {
                r = SkipSpace (r, lineEnd);
                if (r >= lineEnd || *r == '\r' || *r == '#')
                    break;
                int64_t vi = 0, ti = 0, ni = 0;
                auto res = std::from_chars (r, lineEnd, vi);
                if (res.ec != std::errc ())
                    break;
                r = res.ptr;
                if (r < lineEnd && *r == '/') {
                    ++r;
                    if (r < lineEnd && *r != '/') {
                        res = std::from_chars (r, lineEnd, ti);
                        r = res.ptr;
                    }
                    if (r < lineEnd && *r == '/') {
                        ++r;
                        res = std::from_chars (r, lineEnd, ni);
                        r = res.ptr;
                    }
                }
                while (r < lineEnd && *r != ' ' && *r != '\t' && *r != '\r')
                    ++r;
                const int64_t vCount = int64_t (positions.size () / 3);
                const int64_t nCount = int64_t (normals.size () / 3);
                faceV.push_back (vi < 0 ? vCount + vi : vi - 1);
                faceN.push_back (ni == 0 ? -1 : (ni < 0 ? nCount + ni : ni - 1));
            }
            Builder& b = builders[size_t (current)];
            if (b.element == nullptr) {
                b.element = std::make_unique<av::ElementUpload> ();
                b.element->guid = "obj-" + std::to_string (current);
            }
            av::ElementUpload& e = *b.element;
            for (size_t k = 2; k < faceV.size (); ++k) {
                const size_t corner[3] = { 0, k - 1, k };
                bool valid = true;
                for (size_t c : corner)
                    valid = valid && faceV[c] >= 0 && faceV[c] < int64_t (positions.size () / 3);
                if (!valid)
                    continue;
                const float* a = &positions[size_t (faceV[corner[0]]) * 3];
                const float* bb = &positions[size_t (faceV[corner[1]]) * 3];
                const float* c3 = &positions[size_t (faceV[corner[2]]) * 3];
                const float u[3] = { bb[0] - a[0], bb[1] - a[1], bb[2] - a[2] };
                const float w[3] = { c3[0] - a[0], c3[1] - a[1], c3[2] - a[2] };
                float flat[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
                const float length = std::sqrt (flat[0] * flat[0] + flat[1] * flat[1] + flat[2] * flat[2]);
                if (!(length > 0.0f))
                    continue;
                for (float& f : flat)
                    f /= length;
                for (size_t c : corner) {
                    const int64_t vi = faceV[c];
                    const int64_t ni = faceN[c] < int64_t (normals.size () / 3) ? faceN[c] : -1;
                    uint32_t index = 0;
                    if (ni >= 0) {
                        const uint64_t key = (uint64_t (vi) << 32) | uint64_t (ni);
                        auto found = b.welded.find (key);
                        if (found != b.welded.end ()) {
                            e.indices.push_back (found->second);
                            continue;
                        }
                        index = uint32_t (e.vertices.size () / 3);
                        b.welded.emplace (key, index);
                        e.normals.insert (e.normals.end (), &normals[size_t (ni) * 3], &normals[size_t (ni) * 3] + 3);
                    }
                    else {
                        index = uint32_t (e.vertices.size () / 3);
                        e.normals.insert (e.normals.end (), flat, flat + 3);
                    }
                    const float* v = &positions[size_t (vi) * 3];
                    e.vertices.insert (e.vertices.end (), v, v + 3);
                    e.indices.push_back (index);
                }
            }
        }
        else if (lineEnd - q > 6 && std::strncmp (q, "usemtl", 6) == 0) {
            current = materialFor (RestOfLine (q + 6, lineEnd));
        }
        else if (lineEnd - q > 6 && std::strncmp (q, "mtllib", 6) == 0) {
            ParseMtl (DirectoryOf (path) + RestOfLine (q + 6, lineEnd), library);
        }
        p = lineEnd + 1;
    }

    bool first = true;
    out.triangles = 0;
    for (size_t i = 0; i < builders.size (); ++i) {
        const std::string& name = out.materialNames[i];
        const auto found = library.find (name);
        const MtlEntry entry = found != library.end () ? found->second : MtlEntry {};
        if (entry.ke[0] + entry.ke[1] + entry.ke[2] > 0.0f)
            ++out.emissive;
        out.materials->Set (ToSurface (int32_t (i), name, entry));
        if (builders[i].element == nullptr || builders[i].element->indices.empty ())
            continue;
        av::ElementUpload& e = *builders[i].element;
        const uint32_t count = uint32_t (e.indices.size ());
        e.ranges.push_back (av::MaterialRange { int32_t (i), 0, count });
        e.wireEdges.assign (count / 3, 0x7u);
        for (int axis = 0; axis < 3; ++axis) {
            e.boundsMin[axis] = 1.0e30f;
            e.boundsMax[axis] = -1.0e30f;
        }
        for (size_t v = 0; v < e.vertices.size (); v += 3)
            for (int axis = 0; axis < 3; ++axis) {
                e.boundsMin[axis] = std::min (e.boundsMin[axis], e.vertices[v + axis]);
                e.boundsMax[axis] = std::max (e.boundsMax[axis], e.vertices[v + axis]);
            }
        for (int axis = 0; axis < 3; ++axis) {
            out.boundsMin[axis] = first ? e.boundsMin[axis] : std::min (out.boundsMin[axis], e.boundsMin[axis]);
            out.boundsMax[axis] = first ? e.boundsMax[axis] : std::max (out.boundsMax[axis], e.boundsMax[axis]);
        }
        first = false;
        out.triangles += count / 3;
        out.elements.push_back (std::move (builders[i].element));
    }
    if (out.elements.empty ()) {
        error = "no triangles in " + path;
        return false;
    }
    return true;
}

} // namespace renderbench
