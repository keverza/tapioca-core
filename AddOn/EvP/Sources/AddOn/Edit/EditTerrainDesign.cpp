#include "Edit/EditTerrainDesign.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditLandscapeInternal.hpp"
#include "Edit/EditTerrainAdapterInternal.hpp"
#include "Edit/EditTerrainControls.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <exception>
#include <set>
#include <stdexcept>
#include <tuple>

namespace tapioca::geometry {
namespace {
using namespace terrain_detail;
namespace edit = geomsrv::edit;
namespace cp = Clipper2Lib;

bool SameBits (double a, double b)
{
    return std::bit_cast<uint64_t> (a) == std::bit_cast<uint64_t> (b);
}

// The path's own stations and cross-sections, converted at the boundary.
bool Stations (const edit::Path& path, const TerrainDesignOptions& options, std::vector<double>& stations,
               std::vector<TerrainStation>& result, std::string& error)
{
    edit::AlignmentGeometry geometry;
    if (!edit::ValidatePath (path, geometry, error))
        return false;
    // Each range change happens over `taper` metres ending at its start: the
    // station `taper` before it still evaluates the range in force there.
    auto samples = edit::SampleStations (path, geometry, options.maxStep);
    for (size_t i = 1; i < path.sections.size (); ++i) {
        const double before = path.sections[i].station - options.taper;
        if (before > 0)
            samples.push_back (before);
    }
    std::sort (samples.begin (), samples.end ());
    samples.erase (std::unique (samples.begin (), samples.end (), [] (double a, double b) { return b - a < 1.0e-6; }),
                   samples.end ());
    for (double station : samples) {
        edit::StationFrame frame;
        if (!edit::Evaluate (path, geometry, station, frame)) {
            error = "TerrainDesignStation: the path cannot be evaluated at a sampled station";
            return false;
        }
        const auto& section = frame.section;
        TerrainStation value;
        value.position = { frame.centre.x, frame.centre.y, frame.centre.z };
        value.halfLeft = section.widthLeft;
        value.halfRight = section.widthRight;
        if (section.crossfall != 0.0 && section.fall == edit::Fall::Crown)
            value.crown = TerrainSlope { TerrainSlope::Unit::Percent, section.crossfall, 0 };
        else if (section.crossfall != 0.0) {
            value.crossfall = TerrainCrossfall {
                { TerrainSlope::Unit::Percent, section.crossfall, 0 },
                section.fall == edit::Fall::Left ? TerrainHand::Left : TerrainHand::Right,
            };
        }
        value.shoulderLeft = value.shoulderRight = section.shoulder;
        value.cut = { TerrainSlope::Unit::Percent, section.cutSlope, 0 };
        value.fill = { TerrainSlope::Unit::Percent, section.fillSlope, 0 };
        value.cutLimit = options.cutLimit;
        value.fillLimit = options.fillLimit;
        value.rounding = section.rounding;
        stations.push_back (station);
        result.push_back (value);
    }
    return true;
}

// The longest run of stations whose band edges lie on `options.cover` (trimToCover only).
bool TrimToCover (const TerrainDesignOptions& options, std::vector<double>& sampled,
                  std::vector<TerrainStation>& stations, std::string& error)
{
    if (!options.trimToCover || options.cover.size () < 9)
        return true;
    cp::PathD cover;
    for (size_t i = 0; i + 2 < options.cover.size (); i += 3)
        cover.push_back ({ options.cover[i], options.cover[i + 1] });
    const size_t n = stations.size ();
    std::vector<bool> on (n, false);
    for (size_t i = 0; i < n; ++i) {
        const auto& a = stations[i > 0 ? i - 1 : i].position;
        const auto& b = stations[i + 1 < n ? i + 1 : i].position;
        const double dx = b[0] - a[0], dy = b[1] - a[1], length = std::hypot (dx, dy);
        if (!(length > 0))
            continue;
        const double nx = -dy / length, ny = dx / length;
        const auto& c = stations[i].position;
        const auto inside = [&] (double d) {
            return cp::PointInPolygon (cp::PointD { c[0] + nx * d, c[1] + ny * d }, cover) !=
                   cp::PointInPolygonResult::IsOutside;
        };
        on[i] = inside (stations[i].halfLeft) && inside (-stations[i].halfRight);
    }
    size_t best = 0, bestLength = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && on[j])
            ++j;
        if (j - i > bestLength)
            best = i, bestLength = j - i;
        i = j + 1;
    }
    if (bestLength < 2) {
        error = "DesignCoverage: the path does not run over the captured terrain";
        return false;
    }
    sampled = std::vector<double> (sampled.begin () + best, sampled.begin () + best + bestLength);
    stations = std::vector<TerrainStation> (stations.begin () + best, stations.begin () + best + bestLength);
    return true;
}

// Framing, the operation, the owner and the path centreline source.
bool Prepare (const geomsrv::edit::Document& doc, const EditTerrainInput& input, const TerrainDesignOptions& options,
              const edit::Path*& path, const IdentityObject*& owner, std::string& error)
{
    if (!ValidateInput (doc, input, error) ||
        !ValidateOperations (doc, input.provenance, std::array<OperationId, 1> { options.operation }, error))
        return false;
    if (!NewTables (input.provenance, options.designTable, options.meshTable)) {
        error = "ProvenanceConflict: design and mesh table tokens must be new and distinct";
        return false;
    }
    owner = UniformOwner (input.provenance, options.object);
    if (!owner) {
        error = "ProvenanceConflict: terrain design requires one existing, uniformly attributed terrain owner";
        return false;
    }
    if (!CurrentParents (doc, input.provenance, options.operation)) {
        error = "ProvenanceStale: design operation parents differ from the current document";
        return false;
    }
    const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                      [&] (const auto& value) { return value.id == options.source; });
    const auto operation = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                         [&] (const auto& op) { return op.id == options.operation; });
    if (source == doc.landscape->sources.end () || source->deleted || source->role != SourceRole::PathCenterline ||
        !(path = edit::FindPath (doc, source->pathId)) ||
        std::find (operation->inputs.begin (), operation->inputs.end (), options.source) == operation->inputs.end ()) {
        error = "ProvenanceUnattributable: the design operation must consume a live path centreline source";
        return false;
    }
    return CatalogSource (doc, input.provenance, options.source, "path centreline", error);
}

// The band's reach: its stations offset by the widest half width, shoulder and
// face limit plus `margin` (round joins and ends), clipped to the property line
// when the landscape has one. Exactly one hole-free ring must remain.
bool Reach (const geomsrv::edit::Document& doc, const std::vector<TerrainStation>& stations,
            const TerrainDesignOptions& options, std::vector<double>& ring, std::string& error)
{
    double reach = 0;
    cp::PathD centre;
    for (const auto& station : stations) {
        reach = std::max (reach, std::max (station.halfLeft, station.halfRight) +
                                     std::max (station.shoulderLeft, station.shoulderRight) +
                                     std::max (station.cutLimit, station.fillLimit));
        centre.push_back ({ station.position[0], station.position[1] });
    }
    auto shape = cp::InflatePaths ({ centre }, reach + options.margin, cp::JoinType::Round, cp::EndType::Round, 2.0,
                                   landscape_detail::kPrecision, options.arcTolerance);
    LandscapeResult landscape;
    if (!EvaluateLandscape (doc, landscape)) {
        error = landscape.error;
        return false;
    }
    if (!landscape.propertyLine.empty ()) {
        cp::PathD property;
        for (const auto& point : landscape.propertyLine)
            property.push_back ({ point.x, point.y });
        shape = cp::Intersect (shape, { property }, cp::FillRule::NonZero, landscape_detail::kPrecision);
    }
    if (options.cover.size () >= 9) {
        cp::PathD cover;
        for (size_t i = 0; i + 2 < options.cover.size (); i += 3)
            cover.push_back ({ options.cover[i], options.cover[i + 1] });
        shape = cp::Intersect (shape, { cover }, cp::FillRule::NonZero, landscape_detail::kPrecision);
        // Clipped to the terrain, a band may leave crumbs at the mesh's edge: the largest piece.
        if (shape.size () > 1) {
            std::sort (shape.begin (), shape.end (), [] (const cp::PathD& a, const cp::PathD& b) {
                return std::abs (cp::Area (a)) > std::abs (cp::Area (b));
            });
            shape.resize (1);
        }
    }
    if (shape.size () != 1 || shape.front ().size () < 3 || cp::Area (shape.front ()) <= 0) {
        error = "TerrainDesignRegion: the band's reach does not leave one region inside the property line";
        return false;
    }
    ring.clear ();
    for (const auto& point : shape.front ())
        ring.insert (ring.end (), { point.x, point.y, 0.0 });
    return true;
}

// The caller's ring, or the derived reach.
bool Region (const geomsrv::edit::Document& doc, const std::vector<TerrainStation>& stations,
             const TerrainDesignOptions& options, std::vector<double>& ring, std::string& error)
{
    if (options.region.empty ())
        return Reach (doc, stations, options, ring, error);
    if (options.region.size () % 3 != 0 || options.region.size () < 9) {
        error = "TerrainDesignRegion: the region must be a closed xyz ring of at least three vertices";
        return false;
    }
    ring.assign (options.region.begin (), options.region.end ());
    return true;
}

// Design input (domains=7): the baseline rows supported exactly, then the region
// ring's vertices and segments generated by the path source and the operation.
bool Combine (const EditTerrainInput& input, const TerrainDesignOptions& options, const IdentityObject& owner,
              std::span<const double> region, ProvenanceRequest& combined, std::string& error)
{
    const size_t ring = region.size () / 3;
    combined = input.provenance;
    combined.table.context.tableId = options.designTable;
    combined.domains = 7;
    combined.counts.vertices += ring;
    combined.counts.edges = ring;
    combined.table.supportTables.push_back ({ input.provenance.table.context, input.provenance.counts, false });
    combined.table.mappings.clear ();
    for (const auto& row : input.provenance.table.mappings)
        combined.table.mappings.push_back (
            Supported (row, row.domain, row.outputIndex, input.provenance.table.context.tableId, combined.table));
    EntityMapping guide;
    guide.kind = MappingKind::Generated;
    guide.object = options.object;
    guide.sources = Append (combined.table.sourceIds, { options.source });
    guide.operations = Append (combined.table.operationIds, { owner.operation, options.operation });
    for (size_t i = 0; i < ring; ++i) {
        guide.domain = IdentityDomain::Vertex;
        guide.outputIndex = input.provenance.counts.vertices + i;
        combined.table.mappings.push_back (guide);
        guide.domain = IdentityDomain::Edge;
        guide.outputIndex = i;
        combined.table.mappings.push_back (guide);
    }
    return Validate (combined, error);
}

// The base-mesh ancestry of one output vertex, resolved through the kernel's
// explicit rows only. Empty parents: a vertex the path generated on its own line.
struct PredecessorLess {
    bool operator() (const EntityPredecessor& a, const EntityPredecessor& b) const
    {
        return std::tuple (a.tableId, a.domain, a.entityIndex) < std::tuple (b.tableId, b.domain, b.entityIndex);
    }
};
struct Ancestry {
    std::set<const EntityMapping*> parents; // Rows of one table vector: address order is row order.
    std::set<EntityPredecessor, PredecessorLess> support;
};

class Remapper {
  public:
    Remapper (const EditTerrainInput& input, const TerrainDesignOptions& options, const IdentityObject& owner,
              TerrainDesignResult& candidate, const TerrainControls& controls)
        : input_ (input), options_ (options), owner_ (owner), candidate_ (candidate),
          rows_ (candidate.build.inputProvenance), inputs_ (input.mesh.points.size () / 3), controls_ (controls)
    {
    }

    bool Run (std::string& error)
    {
        const auto& build = candidate_.build;
        const size_t vertices = build.points.size () / 3, faces = build.triangles.size () / 3;
        if (build.points.size () % 3 != 0 || build.triangles.size () % 3 != 0 || vertices < inputs_ ||
            build.base.size () != vertices || build.owners.size () != vertices) {
            error = "TerrainOutputShape: design build has an inconsistent shape";
            return false;
        }
        for (size_t i = 0; i < inputs_ * 3; ++i)
            if (!SameBits (i % 3 == 2 ? build.base[i / 3] : build.points[i], input_.mesh.points[i])) {
                error = "TerrainOutputShape: design build did not retain its original vertex prefix";
                return false;
            }
        split_.assign (vertices, nullptr);
        region_.assign (vertices, {});
        fixed_.assign (vertices, nullptr);
        for (const auto& fixed : build.locked) {
            if (fixed_.at (fixed.vertex) || fixed.supports.empty ())
                throw std::runtime_error ("duplicate/empty fixed vertex support");
            fixed_.at (fixed.vertex) = &fixed;
        }
        faceRows_.assign (faces, nullptr);
        std::vector<uint32_t> children (input_.provenance.counts.faces);
        std::set<std::pair<uint32_t, uint32_t>> edges;
        for (size_t i = 0; i < build.triangles.size (); i += 3)
            for (size_t j = 0; j < 3; ++j) {
                const auto a = build.triangles.at (i + j), b = build.triangles.at (i + (j + 1) % 3);
                if (a >= vertices || b >= vertices)
                    throw std::runtime_error ("invalid output triangle index");
                edges.emplace (std::min (a, b), std::max (a, b));
            }
        for (const auto& row : build.mappings) {
            switch (row.kind) {
                case TerrainMappingKind::RegionFace:
                    if (faceRows_.at (row.index) || row.values[1] > 1 || row.values[2] > 1)
                        throw std::runtime_error ("duplicate/invalid region face support");
                    faceRows_.at (row.index) = &row;
                    ++children.at (row.values[0]);
                    break;
                case TerrainMappingKind::SplitVertex:
                    if (row.index < inputs_ || split_.at (row.index))
                        throw std::runtime_error ("duplicate/invalid split vertex support");
                    split_.at (row.index) = &row;
                    break;
                case TerrainMappingKind::RegionLineVertex:
                case TerrainMappingKind::RegionCrossing:
                case TerrainMappingKind::RegionPoint:
                    if (row.index < inputs_ || split_.at (row.index) || region_.at (row.index).size () > 1)
                        throw std::runtime_error ("duplicate/invalid region vertex support");
                    region_.at (row.index).push_back (&row);
                    break;
                case TerrainMappingKind::DesignLineEdge:
                    if (row.index >= build.lines.size () || !edges.count ({ std::min (row.values[1], row.values[2]),
                                                                            std::max (row.values[1], row.values[2]) }))
                        throw std::runtime_error ("design line edge is not an output mesh edge");
                    break;
                case TerrainMappingKind::DesignLineVertex:
                    if (row.index >= build.lines.size () || row.values[1] >= vertices)
                        throw std::runtime_error ("invalid design line vertex");
                    break;
                case TerrainMappingKind::RingVertex:
                    if (row.values[0] >= vertices)
                        throw std::runtime_error ("invalid ring vertex");
                    break;
                case TerrainMappingKind::ControlPointVertex:
                    if (row.index >= controls_.pointSupport.size () || row.values[0] >= vertices)
                        throw std::runtime_error ("invalid control point remap");
                    break;
                case TerrainMappingKind::LockedVertex:
                    if (!fixed_.at (row.index))
                        throw std::runtime_error ("missing fixed vertex support");
                    break;
                default:
                    throw std::runtime_error ("unexpected design support kind");
            }
        }
        if (std::any_of (children.begin (), children.end (), [] (uint32_t count) { return count == 0; })) {
            error = "ProvenanceUnattributable: design rebuild lost a baseline face";
            return false;
        }
        for (size_t v = 0; v < vertices; ++v) {
            if (v >= inputs_ && !split_[v]) {
                const auto& rows = region_[v];
                const bool line = rows.size () == 1 && (rows[0]->kind == TerrainMappingKind::RegionLineVertex ||
                                                        rows[0]->kind == TerrainMappingKind::RegionPoint);
                const bool crossing = rows.size () == 2 && rows[0]->kind == TerrainMappingKind::RegionCrossing &&
                                      rows[1]->kind == TerrainMappingKind::RegionCrossing;
                if (!line && !crossing) {
                    error = "ProvenanceUnattributable: a design vertex has no explicit support";
                    return false;
                }
            }
        }
        return Publish (vertices, faces, error);
    }

  private:
    Ancestry Resolve (uint32_t vertex, int depth) const
    {
        if (depth > 2)
            throw std::runtime_error ("design vertex ancestry does not terminate");
        Ancestry result;
        const auto table = options_.designTable;
        const auto add = [&] (const EntityMapping* row) {
            result.parents.insert (row);
            result.support.insert ({ table, row->domain, row->outputIndex });
        };
        if (vertex < inputs_) {
            result.parents.insert (rows_.vertices.at (vertex));
            result.support.insert ({ table, IdentityDomain::Vertex, vertex });
        }
        else if (const auto* row = split_.at (vertex)) {
            const auto& v = row->values;
            if (v[2] == 1) {
                if (v[3] >= inputs_ || v[4] >= inputs_ || v[3] == v[4])
                    throw std::runtime_error ("invalid original mesh edge host");
                for (uint32_t end : { v[3], v[4] }) {
                    result.parents.insert (rows_.vertices.at (end));
                    result.support.insert ({ table, IdentityDomain::Vertex, end });
                }
            }
            else if (v[2] == 2) {
                result.parents.insert (rows_.faces.at (v[3]));
                result.support.insert ({ table, IdentityDomain::Face, v[3] });
            }
            else
                throw std::runtime_error ("unknown split vertex host");
            if (v[0] != 1 && v[0] != 2)
                throw std::runtime_error ("unknown split vertex origin");
            const auto* guide = v[0] == 1 ? rows_.vertices.at (inputs_ + v[1]) : rows_.edges.at (v[1]);
            result.parents.insert (guide);
            result.support.insert ({ table, guide->domain, guide->outputIndex });
        }
        else
            for (const auto* row : region_.at (vertex)) {
                // A crossing on a base edge inherits both ends' ancestry; a line
                // vertex or a line-line crossing has none in the base.
                if (row->kind == TerrainMappingKind::RegionCrossing && row->values[0] == 1)
                    for (uint32_t end : { row->values[1], row->values[2] }) {
                        auto inner = Resolve (end, depth + 1);
                        result.parents.insert (inner.parents.begin (), inner.parents.end ());
                        result.support.insert (inner.support.begin (), inner.support.end ());
                    }
                else if (row->kind == TerrainMappingKind::RegionCrossing && row->values[0] != 2)
                    throw std::runtime_error ("unknown crossing support");
                if (row->kind == TerrainMappingKind::RegionLineVertex) {
                    const auto& line = candidate_.build.lines.at (row->values[0]);
                    if (line.kind == HybridLineKind::Locked)
                        add (rows_.vertices.at (candidate_.build.inputControlLineStarts.at (line.root) +
                                                row->values[1]));
                }
                if (row->kind == TerrainMappingKind::RegionPoint)
                    add (rows_.vertices.at (candidate_.build.inputControlPointStart + row->values[0]));
            }
        // Fixed-height ancestry is additive even at an existing base vertex;
        // no coincident control or shared segment contributor may disappear.
        if (const auto* fixed = fixed_.at (vertex))
            for (const auto& ref : fixed->supports) {
                if (ref.kind == HybridControlRef::Kind::Point)
                    add (rows_.vertices.at (candidate_.build.inputControlPointStart + ref.index));
                else {
                    size_t edge = candidate_.build.inputRegionCount;
                    for (size_t line = 0; line < ref.index; ++line)
                        edge += controls_.lineSupport.at (line).size ();
                    add (rows_.edges.at (edge + ref.segment));
                }
            }
        return result;
    }

    bool Publish (size_t vertices, size_t faces, std::string& error)
    {
        const auto& build = candidate_.build;
        const auto& combined = build.inputProvenance;
        auto& output = candidate_.meshProvenance;
        output = combined;
        output.table.context.tableId = options_.meshTable;
        output.domains = kMeshProvenanceDomains;
        output.counts = { vertices, 0, faces, 0 };
        output.table.supportTables.push_back ({ combined.table.context, combined.counts, false });
        output.table.mappings.clear ();
        for (size_t v = 0; v < vertices; ++v) {
            if (v < inputs_ && !fixed_[v]) {
                output.table.mappings.push_back (
                    Supported (*rows_.vertices.at (v), IdentityDomain::Vertex, v, options_.designTable, output.table));
                continue;
            }
            const auto ancestry = Resolve (static_cast<uint32_t> (v), 0);
            if (ancestry.parents.empty ()) {
                EntityMapping row;
                row.domain = IdentityDomain::Vertex;
                row.outputIndex = v;
                row.kind = MappingKind::Generated;
                row.object = options_.object;
                row.sources = Append (output.table.sourceIds, { options_.source });
                row.operations = Append (output.table.operationIds, { owner_.operation, options_.operation });
                output.table.mappings.push_back (row);
                continue;
            }
            const std::vector<const EntityMapping*> parents (ancestry.parents.begin (), ancestry.parents.end ());
            const std::vector<EntityPredecessor> support (ancestry.support.begin (), ancestry.support.end ());
            output.table.mappings.push_back (Fragment (combined, parents, IdentityDomain::Vertex, v, options_.operation,
                                                       options_.source, support, output));
        }
        for (size_t f = 0; f < faces; ++f) {
            const auto* row = faceRows_.at (f);
            if (!row)
                throw std::runtime_error ("missing region face support");
            const uint32_t parent = row->values[0];
            const auto* input = rows_.faces.at (parent);
            if (row->values[2] == 1) {
                if (!std::equal (build.triangles.begin () + f * 3, build.triangles.begin () + f * 3 + 3,
                                 input_.mesh.triangles.begin () + parent * 3))
                    throw std::runtime_error ("kept face differs from its parent");
                output.table.mappings.push_back (
                    Supported (*input, IdentityDomain::Face, f, options_.designTable, output.table));
            }
            else {
                const std::array<const EntityMapping*, 1> parents { input };
                const std::array<EntityPredecessor, 1> support { EntityPredecessor { options_.designTable,
                                                                                     IdentityDomain::Face, parent } };
                output.table.mappings.push_back (Fragment (combined, parents, IdentityDomain::Face, f,
                                                           options_.operation, options_.source, support, output));
            }
        }
        return Validate (output, error);
    }

    const EditTerrainInput& input_;
    const TerrainDesignOptions& options_;
    const IdentityObject& owner_;
    TerrainDesignResult& candidate_;
    InputRows rows_;
    size_t inputs_ = 0;
    const TerrainControls& controls_;
    std::vector<const HybridTerrainResult::FixedVertex*> fixed_;
    std::vector<const TerrainMapping*> split_, faceRows_;
    std::vector<std::vector<const TerrainMapping*>> region_;
};

// Every vertex of an outside triangle keeps its base height bits and no owner.
bool VerifyOutside (const HybridTerrainResult& build, std::string& error)
{
    std::vector<bool> fixed (build.base.size ());
    for (const auto& row : build.mappings)
        if (row.kind == TerrainMappingKind::RegionFace && row.values[1] == 0)
            for (size_t j = 0; j < 3; ++j)
                fixed.at (build.triangles.at (row.index * 3 + j)) = true;
    for (size_t v = 0; v < fixed.size (); ++v)
        if (fixed[v] && (!SameBits (build.points.at (v * 3 + 2), build.base[v]) || build.owners[v])) {
            error = "TerrainDesignOutside: the band moved a vertex outside its region";
            return false;
        }
    if (build.context.cut != 0.0 || build.context.fill != 0.0) {
        error = "TerrainDesignOutside: the band changed volume outside its region";
        return false;
    }
    return true;
}

bool Parameters (const TerrainDesignOptions& options, std::string& error)
{
    if (!(options.maxStep > 0) || !std::isfinite (options.maxStep) || !(options.taper > 0) ||
        !std::isfinite (options.taper) || !std::isfinite (options.cutLimit) || !std::isfinite (options.fillLimit) ||
        options.cutLimit < 0 || options.fillLimit < 0 || !(options.margin > 0) || !std::isfinite (options.margin) ||
        !(options.arcTolerance > 0) || !std::isfinite (options.arcTolerance)) {
        error = "TerrainDesignParameters: step, taper, limits, margin and arc tolerance must be finite, all but "
                "the limits positive";
        return false;
    }
    return true;
}

// Kernel station indices become path stations.
bool Locate (const std::vector<HybridConflict>& located, const std::vector<double>& stations,
             const TerrainControls& controls, std::vector<TerrainDesignConflict>& result, std::string& error)
{
    for (const auto& conflict : located) {
        if (conflict.code == HybridConflictCode::LockedConflict) {
            if (!conflict.control)
                throw std::runtime_error ("fixed conflict has no authored control");
            TerrainDesignConflict value;
            value.code = conflict.code;
            value.position = conflict.position;
            value.magnitude = conflict.magnitude;
            value.control = ResolveTerrainControl (controls, *conflict.control);
            if (conflict.other)
                value.other = ResolveTerrainControl (controls, *conflict.other);
            result.push_back (value);
            continue;
        }
        if (conflict.station >= stations.size ()) {
            error = "TerrainOutputShape: a conflict names an unknown station";
            return false;
        }
        result.push_back ({ conflict.code, stations[conflict.station], conflict.side, conflict.position,
                            conflict.magnitude, conflict.hard });
    }
    return true;
}

// "<kernel message> at station S m, side, x y: fix; N located".
void Describe (const std::vector<TerrainDesignConflict>& conflicts, std::string& error)
{
    const auto first = std::find_if (conflicts.begin (), conflicts.end (), [] (const auto& c) { return c.hard; });
    if (first == conflicts.end ())
        return;
    char text[192];
    if (first->control) {
        std::snprintf (text, sizeof (text), " at fixed source %llu, vertex %llu, segment %llu, %.3f %.3f: %s",
                       static_cast<unsigned long long> (first->control->source),
                       static_cast<unsigned long long> (first->control->vertices[0]),
                       static_cast<unsigned long long> (first->control->segment), first->position[0],
                       first->position[1], HybridConflictSuggestion (first->code));
        error += text;
        return;
    }
    std::snprintf (text, sizeof (text), " at station %.2f m, %s, %.3f %.3f: %s; %zu located", first->station,
                   first->side == TerrainHand::Left ? "left" : "right", first->position[0], first->position[1],
                   HybridConflictSuggestion (first->code),
                   static_cast<size_t> (
                       std::count_if (conflicts.begin (), conflicts.end (), [] (const auto& c) { return c.hard; })));
    error += text;
}

} // namespace

std::vector<double> TerrainFootprint (const TerrainMeshView& mesh, double inset)
{
    cp::PathsD triangles;
    for (size_t t = 0; t + 2 < mesh.triangles.size (); t += 3) {
        cp::PathD triangle;
        for (size_t k = 0; k < 3; ++k) {
            const size_t v = size_t (mesh.triangles[t + k]) * 3;
            if (v + 2 >= mesh.points.size ())
                return {};
            triangle.push_back ({ mesh.points[v], mesh.points[v + 1] });
        }
        if (cp::Area (triangle) < 0)
            std::reverse (triangle.begin (), triangle.end ());
        if (cp::Area (triangle) > 1.0e-12)
            triangles.push_back (std::move (triangle));
    }
    auto shape = cp::Union (triangles, cp::FillRule::NonZero, landscape_detail::kPrecision);
    shape =
        cp::InflatePaths (shape, -inset, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, landscape_detail::kPrecision);
    const auto largest = std::max_element (shape.begin (), shape.end (), [] (const cp::PathD& a, const cp::PathD& b) {
        return std::abs (cp::Area (a)) < std::abs (cp::Area (b));
    });
    std::vector<double> ring;
    if (largest == shape.end () || largest->size () < 3)
        return ring;
    for (const auto& point : *largest)
        ring.insert (ring.end (), { point.x, point.y, 0.0 });
    return ring;
}

bool AssessTerrainDesign (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                          const TerrainDesignOptions& options, std::vector<TerrainDesignConflict>& conflicts,
                          std::string& error)
{
    try {
        if (!Parameters (options, error))
            return false;
        const edit::Path* path = nullptr;
        const IdentityObject* owner = nullptr;
        ProvenanceRequest combined;
        TerrainControls controls;
        std::vector<double> sampled, region;
        std::vector<TerrainStation> stations;
        std::vector<HybridConflict> located;
        std::vector<TerrainDesignConflict> candidate;
        if (!Prepare (doc, input, options, path, owner, error) ||
            !Stations (*path, options, sampled, stations, error) || !TrimToCover (options, sampled, stations, error) ||
            !Region (doc, stations, options, region, error) ||
            !Combine (input, options, *owner, region, combined, error) ||
            !CompileTerrainControls (doc, input.provenance, controls, error) ||
            !AppendTerrainControls (controls, options, *owner, combined, error) ||
            !AssessHybridTerrain ({ input.mesh, region, stations, options.effect, options.tolerance, combined,
                                    controls.lines, controls.points },
                                  located, error) ||
            !Locate (located, sampled, controls, candidate, error))
            return false;
        conflicts = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainDesignRefused: ") + exception.what ();
        return false;
    }
}

bool EvaluateTerrainDesign (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                            const TerrainDesignOptions& options, TerrainDesignResult& result, std::string& error)
{
    try {
        if (!Parameters (options, error))
            return false;
        const edit::Path* path = nullptr;
        const IdentityObject* owner = nullptr;
        ProvenanceRequest combined;
        TerrainDesignResult candidate;
        TerrainControls controls;
        std::vector<TerrainStation> stations;
        if (!Prepare (doc, input, options, path, owner, error) ||
            !Stations (*path, options, candidate.stations, stations, error) ||
            !TrimToCover (options, candidate.stations, stations, error) ||
            !Region (doc, stations, options, candidate.region, error) ||
            !Combine (input, options, *owner, candidate.region, combined, error) ||
            !CompileTerrainControls (doc, input.provenance, controls, error) ||
            !AppendTerrainControls (controls, options, *owner, combined, error))
            return false;
        const HybridTerrainRequest band { input.mesh,        candidate.region, stations,       options.effect,
                                          options.tolerance, combined,         controls.lines, controls.points };
        if (!BuildHybridTerrain (band, candidate.build, error)) {
            // Name where, when the refusal is a located conflict.
            std::vector<HybridConflict> located;
            std::vector<TerrainDesignConflict> placed;
            std::string ignored;
            if (AssessHybridTerrain (band, located, ignored) &&
                Locate (located, candidate.stations, controls, placed, ignored))
                Describe (placed, error);
            return false;
        }
        if (!VerifyOutside (candidate.build, error) ||
            !Locate (candidate.build.conflicts, candidate.stations, controls, candidate.conflicts, error))
            return false;
        Remapper remapper (input, options, *owner, candidate, controls);
        if (!remapper.Run (error))
            return false;
        for (const auto& fixed : candidate.build.locked) {
            TerrainDesignResult::FixedVertex value { fixed.vertex, {} };
            for (const auto& support : fixed.supports)
                value.supports.push_back (ResolveTerrainControl (controls, support));
            candidate.locked.push_back (std::move (value));
        }
        candidate.vertexOperations.reserve (candidate.build.owners.size ());
        for (const auto& owned : candidate.build.owners)
            candidate.vertexOperations.push_back (owned ? std::optional<OperationId> (options.operation)
                                                        : std::nullopt);
        candidate.revision = input.revision;
        candidate.sourceTerrainGuid = input.sourceTerrainGuid;
        result = std::move (candidate);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("TerrainDesignRefused: ") + exception.what ();
        return false;
    }
}

} // namespace tapioca::geometry
