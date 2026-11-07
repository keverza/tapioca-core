#include "ArchViz/SunStudyObstructionGeometry.hpp"

#include "SunStudy/SunStudyRoles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace geomsrv::archviz {

namespace {

using PartKey = std::tuple<std::string, size_t, int>;
using EdgeKey = std::pair<uint32_t, uint32_t>;

uint32_t Mix (uint32_t a, uint32_t b, double t)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const double channel = ((a >> shift) & 255u) * (1.0 - t) + ((b >> shift) & 255u) * t;
        out |= uint32_t (std::clamp (channel, 0.0, 255.0) + 0.5) << shift;
    }
    return out;
}

bool AppendTriangle (const geomsrv::Mesh& mesh, size_t triangle, uint32_t rgba, GhPreviewBucket& bucket)
{
    if (triangle >= mesh.TriangleCount ())
        return false;
    const uint32_t* indices = &mesh.triangles[3 * triangle];
    double a[3], b[3], c[3];
    for (int axis = 0; axis < 3; ++axis) {
        const size_t ia = 3ull * indices[0] + axis, ib = 3ull * indices[1] + axis, ic = 3ull * indices[2] + axis;
        if (ia >= mesh.vertices.size () || ib >= mesh.vertices.size () || ic >= mesh.vertices.size ())
            return false;
        a[axis] = mesh.vertices[ia];
        b[axis] = mesh.vertices[ib];
        c[axis] = mesh.vertices[ic];
    }
    const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const double v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    double normal[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    const double length = std::sqrt (normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!(length > 0.0) || !std::isfinite (length))
        return false;
    for (double& component : normal)
        component /= length;
    const size_t first = bucket.meshVertices.size ();
    for (const double* point : { a, b, c }) {
        GhPreviewMeshVertex vertex;
        vertex.x = float (point[0]);
        vertex.y = float (point[1]);
        vertex.z = float (point[2]);
        vertex.nx = float (normal[0]);
        vertex.ny = float (normal[1]);
        vertex.nz = float (normal[2]);
        vertex.rgba = rgba;
        bucket.meshVertices.push_back (vertex);
    }
    bucket.meshIndices.insert (bucket.meshIndices.end (),
                               { uint32_t (first), uint32_t (first + 1), uint32_t (first + 2) });
    return true;
}

void AppendWireframe (const geomsrv::Mesh& mesh, GhPreviewBucket& bucket)
{
    std::set<EdgeKey> edges;
    const bool sourceEdges = mesh.triWireEdges.size () == mesh.TriangleCount ();
    for (size_t triangle = 0; triangle < mesh.TriangleCount (); ++triangle)
        for (size_t edge = 0; edge < 3; ++edge) {
            if (sourceEdges && (mesh.triWireEdges[triangle] & (1u << edge)) == 0)
                continue;
            const uint32_t a = mesh.triangles[3 * triangle + edge];
            const uint32_t b = mesh.triangles[3 * triangle + (edge + 1) % 3];
            if (!edges.insert (std::minmax (a, b)).second)
                continue;
            if (3ull * a + 2 >= mesh.vertices.size () || 3ull * b + 2 >= mesh.vertices.size ())
                continue;
            const float from[3] = { float (mesh.vertices[3ull * a]), float (mesh.vertices[3ull * a + 1]),
                                    float (mesh.vertices[3ull * a + 2]) };
            const float to[3] = { float (mesh.vertices[3ull * b]), float (mesh.vertices[3ull * b + 1]),
                                  float (mesh.vertices[3ull * b + 2]) };
            AppendPreviewSegment (from, to, kSunObstructionPink, bucket);
        }
}

} // namespace

uint32_t SunObstructionTimeColour (int minute)
{
    // Midnight blue -> dawn orange -> noon yellow -> dusk red -> midnight blue.
    constexpr std::array<std::pair<int, uint32_t>, 5> stops = { { { 0, 0x3B4CC0FFu },
                                                                  { 6 * 60, 0xF0A44AFFu },
                                                                  { 12 * 60, 0xF5E663FFu },
                                                                  { 18 * 60, 0xE26D5AFFu },
                                                                  { 24 * 60, 0x3B4CC0FFu } } };
    const int at = std::clamp (minute, 0, 24 * 60);
    for (size_t index = 1; index < stops.size (); ++index)
        if (at <= stops[index].first) {
            const double span = double (stops[index].first - stops[index - 1].first);
            return Mix (stops[index - 1].second, stops[index].second,
                        span > 0.0 ? (at - stops[index - 1].first) / span : 0.0);
        }
    return stops.back ().second;
}

bool BuildSunObstructionGeometry (const geomsrv::Snapshot& snapshot,
                                  const std::vector<evp::sunstudy::SunObstructionResult>& steps, bool wholeDay,
                                  SunObstructionGeometry& result)
{
    result = SunObstructionGeometry ();
    std::map<std::string, size_t> meshes;
    for (size_t index = 0; index < snapshot.meshes.size (); ++index) {
        const std::string guid = evp::sunstudy::CanonicalGuid (snapshot.meshes[index].guid);
        if (!meshes.emplace (guid, index).second) {
            result.error = "the study snapshot has an ambiguous blocking-object GUID";
            return false;
        }
    }
    std::set<PartKey> parts;
    std::set<std::string> blockers;
    for (const auto& step : steps)
        for (const auto& cell : step.cells) {
            if (cell.state != evp::sunstudy::SunObstructionState::Blocked)
                continue;
            const std::string guid = evp::sunstudy::CanonicalGuid (cell.blockerGuid);
            const auto mesh = meshes.find (guid);
            if (mesh == meshes.end ()) {
                result.error = "a blocking object is missing from the study snapshot";
                return false;
            }
            const int minute = wholeDay ? step.minute : 0;
            if (!parts.insert ({ guid, cell.sourceTriangle, minute }).second)
                continue;
            const uint32_t colour = wholeDay ? SunObstructionTimeColour (step.minute) : kSunObstructionPink;
            if (!AppendTriangle (snapshot.meshes[mesh->second], cell.sourceTriangle, colour,
                                 result.drawables.depthTested)) {
                result.error = "a blocking triangle is invalid in the study snapshot";
                return false;
            }
            blockers.insert (guid);
        }
    for (const std::string& guid : blockers)
        AppendWireframe (snapshot.meshes[meshes.at (guid)], result.drawables.depthTested);
    result.blockerObjects = blockers.size ();
    result.timedParts = parts.size ();
    return true;
}

} // namespace geomsrv::archviz
