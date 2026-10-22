#ifndef TAPIOCA_TEST_TERRAIN_FIXTURE_HPP
#define TAPIOCA_TEST_TERRAIN_FIXTURE_HPP

// Shared offline corpus/measurement helpers. Not linked into the add-on.
#include "Geometry/GeometryRustTerrain.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace tapioca::terrain_fixture {
using namespace tapioca::geometry;
struct Fixture {
    std::string name;
    double area = 0, insideArea = 0, maxArea = 0;
    size_t crossings = 0;
    std::vector<double> points, line;
    std::vector<std::pair<TerrainConstraintRole, std::vector<uint32_t>>> constraints;
    std::vector<TerrainConstraint> Constraints () const
    {
        std::vector<TerrainConstraint> result;
        for (const auto& [role, indices] : constraints)
            result.push_back ({ role, indices });
        return result;
    }
    size_t Segments () const
    {
        size_t count = 0;
        for (const auto& [role, indices] : constraints)
            count += indices.size () - (role == TerrainConstraintRole::Breakline);
        return count;
    }
};
inline void Require (bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error (message);
}
inline void Keyword (std::istream& input, const std::string& expected)
{
    std::string value;
    input >> value;
    Require (value == expected, "fixture expected " + expected);
}
inline std::vector<double> Points (std::istream& input)
{
    size_t count = 0;
    input >> count;
    Require (count <= 100000, "fixture point limit");
    std::vector<double> result;
    for (size_t i = 0; i < count; ++i) {
        double x = 0, y = 0, z = 0;
        input >> x >> y >> z;
        result.insert (result.end (), { 500000 + x, 6000000 + y, z });
    }
    return result;
}
inline std::vector<Fixture> Corpus ()
{
    std::ifstream file (TAPIOCA_TERRAIN_CORPUS);
    Require (file.good (), "terrain corpus missing");
    std::ostringstream text;
    for (std::string line; std::getline (file, line);) {
        if (line.find_first_not_of (" \t\r") != std::string::npos && line.at (line.find_first_not_of (" \t\r")) != '#')
            text << line << '\n';
    }
    std::istringstream input (text.str ());
    std::vector<Fixture> result;
    for (std::string start; input >> start;) {
        Require (start == "case", "fixture expected case");
        Fixture value;
        input >> value.name >> value.area >> value.insideArea >> value.crossings >> value.maxArea;
        Keyword (input, "points");
        value.points = Points (input);
        Keyword (input, "constraints");
        size_t count = 0;
        input >> count;
        for (size_t i = 0; i < count; ++i) {
            uint32_t role = 0;
            size_t size = 0;
            input >> role >> size;
            Require (role >= 1 && role <= 3 && size <= 100000, "fixture constraint shape");
            std::vector<uint32_t> indices (size);
            for (auto& index : indices)
                input >> index;
            value.constraints.emplace_back (static_cast<TerrainConstraintRole> (role), std::move (indices));
        }
        Keyword (input, "line");
        value.line = Points (input);
        Keyword (input, "end");
        Require (!input.fail (), "truncated terrain fixture");
        result.push_back (std::move (value));
    }
    Require (result.size () == 6, "terrain corpus count");
    return result;
}
inline Fixture Workload ()
{
    Fixture value;
    value.name = "grid_100x100_500";
    value.area = 9801;
    for (uint32_t y = 0; y < 100; ++y)
        for (uint32_t x = 0; x < 100; ++x)
            value.points.insert (value.points.end (), { 500000.0 + x, 6000000.0 + y, x * .1 + y * .05 });
    for (uint32_t n = 0; n < 500; ++n) {
        const auto a = (n / 99 * 17) * 100 + n % 99;
        value.constraints.push_back ({ TerrainConstraintRole::Breakline, { a, a + 1 } });
    }
    return value;
}
inline ProvenanceRequest Provenance (EntityCounts counts, uint32_t domains)
{
    ProvenanceRequest value;
    value.expectedSession = { 1 };
    value.counts = counts;
    value.domains = domains;
    auto& table = value.table;
    table.context = { 1, value.expectedSession, 0, 100 };
    table.operations = { { 12, {} } };
    table.operationIds = { 12 };
    table.objects = { { 13, 12 } };
    for (auto [domain, count] : { std::pair { IdentityDomain::Vertex, counts.vertices },
                                  { IdentityDomain::Edge, counts.edges },
                                  { IdentityDomain::Face, counts.faces } }) {
        for (uint64_t index = 0; index < count; ++index) {
            const uint64_t offset = table.sources.size (), source = 0x20000000000001ULL + offset;
            table.sources.push_back ({ source, {} });
            table.sourceIds.push_back (source);
            EntityMapping row;
            row.domain = domain;
            row.kind = MappingKind::Generated;
            row.outputIndex = index;
            row.sources = { offset, 1 };
            row.operations = { 0, 1 };
            row.object = 13;
            table.mappings.push_back (row);
        }
    }
    return value;
}
inline TerrainRequest Request (const Fixture& input, std::span<const TerrainConstraint> constraints,
                               const ProvenanceRequest& provenance)
{
    TerrainRequest request { input.points, constraints, 1e-8, {}, provenance };
    if (input.maxArea > 0)
        request.refinement = TerrainRefinement { 0, input.maxArea, 20000 };
    return request;
}
inline uint64_t Digest (const TerrainResult& result)
{
    uint64_t hash = 14695981039346656037ULL;
    const auto add = [&] (uint64_t bits, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) {
            hash ^= (bits >> (i * 8)) & 255U;
            hash *= 1099511628211ULL;
        }
    };
    for (double point : result.points)
        add (std::bit_cast<uint64_t> (point), 8);
    for (auto index : result.triangles)
        add (index, 4);
    return hash;
}
inline std::array<double, 3> Point (const TerrainResult& mesh, uint32_t index)
{
    const size_t offset = size_t (index) * 3;
    return { mesh.points.at (offset), mesh.points.at (offset + 1), mesh.points.at (offset + 2) };
}
inline double Area (const TerrainResult& mesh, size_t face)
{
    const auto a = Point (mesh, mesh.triangles.at (face * 3)), b = Point (mesh, mesh.triangles.at (face * 3 + 1)),
               c = Point (mesh, mesh.triangles.at (face * 3 + 2));
    return .5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
}
inline void CheckTin (const Fixture& input, const TerrainResult& mesh)
{
    double total = 0;
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (size_t face = 0; face < mesh.triangles.size () / 3; ++face) {
        const auto area = Area (mesh, face);
        Require (area > 0, "CCW/positive area");
        total += area;
        for (size_t i = 0; i < 3; ++i)
            edges.insert (std::minmax (mesh.triangles.at (face * 3 + i), mesh.triangles.at (face * 3 + (i + 1) % 3)));
        std::array<double, 3> centroid {};
        for (size_t i = 0; i < 3; ++i) {
            auto p = Point (mesh, mesh.triangles.at (face * 3 + i));
            for (size_t j = 0; j < 3; ++j)
                centroid[j] += p[j] / 3;
        }
        const double x = centroid[0] - 500000, y = centroid[1] - 6000000;
        const auto ramp = [x] (double start, double width, double height) {
            return std::clamp ((x - start) / width, 0.0, 1.0) * height;
        };
        double expected = .1 * x + .05 * y;
        if (input.name == "wall_sides")
            expected = ramp (6, .2, 3);
        if (input.name == "steep_curb")
            expected = ramp (12, .15, .18);
        if (input.name == "stair_treads")
            expected = ramp (5, .1, .2) + ramp (7, .1, .2) + ramp (9, .1, .2);
        Require (std::abs (centroid[2] - expected) < 1e-7, "analytic feature surface/sawtooth");
    }
    Require (std::abs (total - input.area) < 1e-7, "analytic domain area");
    size_t crossings = 0;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<TerrainMapping>> paths;
    for (const auto& row : mesh.mappings) {
        if (row.kind == TerrainMappingKind::Crossing)
            ++crossings;
        if (row.kind == TerrainMappingKind::ConstraintEdge)
            paths[{ row.index, row.values[0] }].push_back (row);
    }
    Require (crossings >= input.crossings, "crossing pre-split completeness");
    uint32_t flattened = 0;
    for (uint32_t c = 0; c < input.constraints.size (); ++c) {
        const auto& [role, indices] = input.constraints.at (c);
        for (uint32_t segment = 0; segment < indices.size () - (role == TerrainConstraintRole::Breakline);
             ++segment, ++flattened) {
            const auto& path = paths.at ({ c, segment });
            Require (!path.empty (), "missing constraint path");
            // These fixtures have unique input xy, so input remaps are identity.
            uint32_t current = indices.at (segment);
            for (const auto& row : path) {
                Require (row.values[1] == current && row.values[4] == flattened, "path order/input ancestry");
                Require (row.values[3] == 1 && edges.contains (std::minmax (row.values[1], row.values[2])),
                         "unenforced feature edge");
                current = row.values[2];
            }
            Require (current == indices.at ((segment + 1) % indices.size ()), "path endpoint");
        }
    }
}
inline void CheckSplit (const Fixture& input, const TerrainResult& baseline, const TerrainResult& split)
{
    Require (split.points.size () >= baseline.points.size () &&
                 std::equal (baseline.points.begin (), baseline.points.end (), split.points.begin ()),
             "split input vertex preservation");
    std::vector<double> areas (baseline.triangles.size () / 3);
    double inside = 0;
    std::vector<std::vector<size_t>> children (areas.size ());
    std::map<std::pair<uint32_t, uint32_t>, size_t> insideEdges;
    std::vector<uint32_t> lineVertices (input.line.size () / 3, std::numeric_limits<uint32_t>::max ());
    std::map<uint32_t, std::vector<TerrainMapping>> linePaths;
    for (const auto& row : split.mappings) {
        if (row.kind == TerrainMappingKind::LineVertex) {
            Require (lineVertices.at (row.index) == std::numeric_limits<uint32_t>::max (), "duplicate line remap");
            lineVertices.at (row.index) = row.values[0];
            const auto p = Point (split, row.values[0]);
            Require (p[0] == input.line.at (size_t (row.index) * 3) &&
                         p[1] == input.line.at (size_t (row.index) * 3 + 1),
                     "line remap coordinates");
        }
        if (row.kind == TerrainMappingKind::LineEdge)
            linePaths[row.index].push_back (row);
        if (row.kind != TerrainMappingKind::SplitFace)
            continue;
        const auto area = Area (split, row.index);
        Require (area > 0, "split positive area");
        areas.at (row.values[0]) += area;
        children.at (row.values[0]).push_back (row.index);
        if (row.values[1])
            inside += area;
        if (row.values[1])
            for (size_t i = 0; i < 3; ++i)
                ++insideEdges[std::minmax (split.triangles.at (size_t (row.index) * 3 + i),
                                           split.triangles.at (size_t (row.index) * 3 + (i + 1) % 3))];
        const size_t parent = row.values[0];
        const auto a = Point (baseline, baseline.triangles.at (parent * 3)),
                   b = Point (baseline, baseline.triangles.at (parent * 3 + 1)),
                   c = Point (baseline, baseline.triangles.at (parent * 3 + 2));
        for (size_t i = 0; i < 3; ++i) {
            const auto p = Point (split, split.triangles.at (size_t (row.index) * 3 + i));
            const auto d = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            const auto u = ((p[0] - a[0]) * (c[1] - a[1]) - (p[1] - a[1]) * (c[0] - a[0])) / d;
            const auto v = ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) / d;
            Require (std::abs (p[2] - (a[2] + u * (b[2] - a[2]) + v * (c[2] - a[2]))) < 1e-7,
                     "original parent surface");
        }
    }
    Require (std::abs (inside - input.insideArea) < 1e-7, "property-line area");
    for (uint32_t segment = 0; segment < lineVertices.size (); ++segment) {
        uint32_t current = lineVertices.at (segment);
        Require (current != std::numeric_limits<uint32_t>::max (), "missing line vertex");
        const auto& path = linePaths.at (segment);
        Require (!path.empty (), "missing line edge");
        for (const auto& edge : path) {
            Require (edge.values[0] == current, "line path order");
            Require (insideEdges.at (std::minmax (current, edge.values[1])) == 1, "line conformity/inside adjacency");
            current = edge.values[1];
        }
        Require (current == lineVertices.at ((segment + 1) % lineVertices.size ()), "line closure");
    }
    double right = -std::numeric_limits<double>::infinity ();
    for (size_t i = 0; i < input.line.size (); i += 3)
        right = std::max (right, input.line.at (i));
    size_t untouched = 0;
    for (size_t parent = 0; parent < areas.size (); ++parent) {
        Require (std::abs (areas[parent] - Area (baseline, parent)) < 1e-7, "split parent area");
        bool remote = true;
        for (size_t i = 0; i < 3; ++i)
            remote = remote && Point (baseline, baseline.triangles.at (parent * 3 + i))[0] > right;
        if (remote) {
            ++untouched;
            Require (children[parent].size () == 1, "remote triangle split");
            for (size_t i = 0; i < 3; ++i)
                Require (baseline.triangles.at (parent * 3 + i) == split.triangles.at (children[parent][0] * 3 + i),
                         "untouched triangle verbatim");
        }
    }
    Require (untouched > 0, "nonvacuous remote preservation");
}
} // namespace tapioca::terrain_fixture
#endif
