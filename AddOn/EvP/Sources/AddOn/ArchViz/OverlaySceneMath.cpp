// ArchViz/OverlaySceneMath -- the guest's pure geometry: vertex and corner normals, a
// mesh's feature edges, a length as text, a ramp's colour. Exposed through
// OverlayScene.hpp for their tests; no ImGui, no text engine.

#include "ArchViz/OverlaySceneBuilder.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>

namespace geomsrv {
namespace archviz {
namespace overlayscene {

using namespace build;

std::vector<double> VertexNormals (const std::vector<double>& points, const std::vector<uint32_t>& indices)
{
    const size_t vertices = points.size () / 3;
    std::vector<double> normals (vertices * 3, 0.0);
    for (size_t t = 0; t + 2 < indices.size (); t += 3) {
        const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= vertices || i1 >= vertices || i2 >= vertices)
            continue;
        // Unnormalised: its length is twice the triangle's area, which is the weight.
        const Vec3 n = Cross (Sub (At (points, i1), At (points, i0)), Sub (At (points, i2), At (points, i0)));
        for (const uint32_t i : { i0, i1, i2 }) {
            normals[size_t (i) * 3] += n.x;
            normals[size_t (i) * 3 + 1] += n.y;
            normals[size_t (i) * 3 + 2] += n.z;
        }
    }
    for (size_t v = 0; v < vertices; ++v) {
        const Vec3 n = { normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2] };
        const double length = Length (n);
        if (length > 1e-30) {
            normals[v * 3] = n.x / length;
            normals[v * 3 + 1] = n.y / length;
            normals[v * 3 + 2] = n.z / length;
        }
        else {
            normals[v * 3] = 0.0;
            normals[v * 3 + 1] = 0.0;
            normals[v * 3 + 2] = 1.0;
        }
    }
    return normals;
}

std::vector<std::pair<uint32_t, uint32_t>> FeatureEdges (const std::vector<double>& points,
                                                         const std::vector<uint32_t>& indices, float angleDegrees)
{
    const size_t vertices = points.size () / 3;
    // Weld: every vertex to the first one at its position, within a micrometre.
    std::vector<uint32_t> canonical (vertices);
    {
        struct KeyHash {
            size_t operator() (const std::tuple<int64_t, int64_t, int64_t>& key) const
            {
                const uint64_t a = uint64_t (std::get<0> (key)), b = uint64_t (std::get<1> (key)),
                               c = uint64_t (std::get<2> (key));
                return size_t (a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0x94D049BB133111EBull ^ c);
            }
        };
        std::unordered_map<std::tuple<int64_t, int64_t, int64_t>, uint32_t, KeyHash> seen;
        seen.reserve (vertices);
        for (size_t v = 0; v < vertices; ++v) {
            const auto key = std::make_tuple (int64_t (std::llround (points[v * 3] * 1e6)),
                                              int64_t (std::llround (points[v * 3 + 1] * 1e6)),
                                              int64_t (std::llround (points[v * 3 + 2] * 1e6)));
            canonical[v] = seen.emplace (key, uint32_t (v)).first->second;
        }
    }
    struct EdgeFaces {
        uint32_t a = 0, b = 0; // original indices, for the coordinates
        Vec3 normals[2];
        uint32_t faces = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, EdgeFaces> edges;
    for (size_t t = 0; t + 2 < indices.size (); t += 3) {
        const uint32_t original[3] = { indices[t], indices[t + 1], indices[t + 2] };
        if (original[0] >= vertices || original[1] >= vertices || original[2] >= vertices)
            continue;
        const Vec3 n = Cross (Sub (At (points, original[1]), At (points, original[0])),
                              Sub (At (points, original[2]), At (points, original[0])));
        const double area = Length (n);
        if (area < 1e-18)
            continue; // a degenerate triangle has no face to crease against
        const Vec3 unit = { n.x / area, n.y / area, n.z / area };
        for (int e = 0; e < 3; ++e) {
            const uint32_t i = original[e], j = original[(e + 1) % 3];
            const uint32_t ci = canonical[i], cj = canonical[j];
            if (ci == cj)
                continue;
            EdgeFaces& edge = edges[{ (std::min) (ci, cj), (std::max) (ci, cj) }];
            if (edge.faces == 0) {
                edge.a = i;
                edge.b = j;
            }
            if (edge.faces < 2)
                edge.normals[edge.faces] = unit;
            ++edge.faces;
        }
    }
    const double creaseCosine = std::cos (double (angleDegrees) * kPi / 180.0);
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& entry : edges) {
        const EdgeFaces& edge = entry.second;
        // One face: a boundary. More than two: not a surface, so drawn. Two: a crease
        // when they turn by more than the angle.
        const bool feature = edge.faces != 2 || Dot (edge.normals[0], edge.normals[1]) < creaseCosine - 1e-12;
        if (feature)
            out.push_back ({ edge.a, edge.b });
    }
    return out;
}

std::string FormatLength (double metres, uint32_t decimals, layers::LengthUnit unit, bool showUnit)
{
    double value = metres;
    const char* suffix = "m";
    if (unit == layers::LengthUnit::Centimetres) {
        value = metres * 100.0;
        suffix = "cm";
    }
    else if (unit == layers::LengthUnit::Millimetres) {
        value = metres * 1000.0;
        suffix = "mm";
    }
    char buffer[64] = {};
    std::snprintf (buffer, sizeof (buffer), "%.*f", int ((std::min) (decimals, 6u)), value);
    std::string text = buffer;
    if (showUnit)
        text += std::string (" ") + suffix;
    return text;
}

uint32_t RampAt (const std::vector<layers::ColourStop>& stops, float t)
{
    if (stops.empty ())
        return 0;
    if (t <= stops.front ().at)
        return stops.front ().rgba;
    if (t >= stops.back ().at)
        return stops.back ().rgba;
    for (size_t i = 1; i < stops.size (); ++i) {
        if (t > stops[i].at)
            continue;
        const float span = stops[i].at - stops[i - 1].at;
        const float f = span > 1e-6f ? (t - stops[i - 1].at) / span : 1.0f;
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float a = float ((stops[i - 1].rgba >> shift) & 0xFFu);
            const float b = float ((stops[i].rgba >> shift) & 0xFFu);
            out |= uint32_t (std::lround (a + (b - a) * f)) << shift;
        }
        return out;
    }
    return stops.back ().rgba;
}

} // namespace overlayscene
} // namespace archviz
} // namespace geomsrv
