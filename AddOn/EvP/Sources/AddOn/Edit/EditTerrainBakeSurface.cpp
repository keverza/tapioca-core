#include "Edit/EditTerrainBakeSurface.hpp"
#include "Geometry/GeometryRustTerrain.hpp"
#include <map>
#include <span>
#include <tuple>

namespace geomsrv::edit {
namespace tg = tapioca::geometry;
namespace {
// The TIN service attributes every input vertex and constraint edge. The plan is derived
// geometry with no authored source, so each one is one generated row of a single transient owner:
// a catalogue local to this call, never a document id (the TIN tests' own catalogue shape).
tg::ProvenanceRequest Generated (uint64_t vertices, uint64_t edges)
{
    tg::ProvenanceRequest value;
    value.expectedSession = { 1 };
    value.counts = { vertices, edges, 0, 0 };
    value.domains = 3; // vertex and edge
    auto& table = value.table;
    table.context = { 1, value.expectedSession, 0, 100 };
    table.operations = { { 12, {} } };
    table.operationIds = { 12 };
    table.objects = { { 13, 12 } };
    for (auto [domain, count] :
         { std::pair { tg::IdentityDomain::Vertex, vertices }, std::pair { tg::IdentityDomain::Edge, edges } })
        for (uint64_t index = 0; index < count; ++index) {
            const uint64_t offset = table.sources.size (), source = 0x20000000000001ULL + offset;
            table.sources.push_back ({ source, {} });
            table.sourceIds.push_back (source);
            tg::EntityMapping row;
            row.domain = domain;
            row.kind = tg::MappingKind::Generated;
            row.outputIndex = index;
            row.sources = { offset, 1 };
            row.operations = { 0, 1 };
            row.object = 13;
            table.mappings.push_back (row);
        }
    return value;
}
} // namespace

bool BakePlanSurface (const BakePlan& plan, BakeSurface& surface, std::string& error)
{
    if (!plan.ok || plan.outline.size () < 3) {
        error = "BakeSurface: the plan has no outline";
        return false;
    }
    // One vertex per distinct point: a line's end is an outline vertex.
    std::vector<double> points;
    std::map<std::tuple<double, double, double>, uint32_t> at;
    const auto index = [&] (const Vec3& p) {
        const auto [it, added] = at.try_emplace ({ p.x, p.y, p.z }, uint32_t (points.size () / 3));
        if (added)
            points.insert (points.end (), { p.x, p.y, p.z });
        return it->second;
    };
    std::vector<std::vector<uint32_t>> rings;
    std::vector<tg::TerrainConstraintRole> roles;
    uint64_t edges = 0;
    const auto add = [&] (const std::vector<Vec3>& line, bool closed, tg::TerrainConstraintRole role) {
        std::vector<uint32_t> ids;
        for (const auto& p : line)
            if (const auto id = index (p); ids.empty () || ids.back () != id)
                ids.push_back (id);
        if (role == tg::TerrainConstraintRole::Breakline && closed && ids.size () > 2 && ids.front () != ids.back ())
            ids.push_back (ids.front ()); // a ring as a breakline: closed explicitly
        if (ids.size () < 2)
            return;
        edges += role == tg::TerrainConstraintRole::Boundary ? ids.size () : ids.size () - 1;
        rings.push_back (std::move (ids));
        roles.push_back (role);
    };
    add (plan.outline, true, tg::TerrainConstraintRole::Boundary);
    for (const auto& line : plan.lines)
        add (line.points, line.closed, tg::TerrainConstraintRole::Breakline);
    for (const auto& p : plan.levelPoints)
        index (p);
    std::vector<tg::TerrainConstraint> constraints;
    for (size_t i = 0; i < rings.size (); ++i)
        constraints.push_back ({ roles[i], std::span<const uint32_t> (rings[i]) });
    const auto provenance = Generated (points.size () / 3, edges);
    const tg::TerrainRequest request { points, constraints, 1e-6, {}, provenance };
    tg::TerrainResult result;
    if (!tg::TriangulateTerrain (request, result, error)) {
        error = "BakeSurface: " + error;
        return false;
    }
    surface.points = std::move (result.points);
    surface.triangles = std::move (result.triangles);
    error.clear ();
    return true;
}

} // namespace geomsrv::edit
