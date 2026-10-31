#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainOutline.hpp"
#include "Edit/EditLandscapeInternal.hpp"
#include "Geometry/ClipperPlanRing.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace tapioca::geometry {
namespace {
namespace cp = Clipper2Lib;
namespace ld = landscape_detail;
cp::PathD Plan (std::span<const double> points)
{
    cp::PathD ring;
    for (size_t i = 0; i + 2 < points.size (); i += 3)
        ring.emplace_back (points[i], points[i + 1]);
    return ring;
}
struct Preparation {
    const geomsrv::edit::Document& doc;
    ObjectId owner;
    RegionTerrainResult& out;
    std::shared_ptr<const TerrainCapture> current;
    uint64_t token = 100;
    size_t vertices = 0;
    bool Split (const cp::PathD& ring, std::span<const SourceId> sources, OperationId operation, std::string& error)
    {
        if (ring.size () < 3)
            return true;
        if (out.preparation.size () >= 128 || vertices > 1000000) {
            error = "RegionTerrainWorkLimit: preparation exceeds 128 rings or one million retained vertices";
            return false;
        }
        // Tokens are request-local, not document identities, and cannot collide
        // with the native/surface capture's retained support set.
        const auto used = [&] (uint64_t value) {
            if (value == current->provenance.table.context.tableId)
                return true;
            return std::any_of (current->provenance.table.supportTables.begin (),
                                current->provenance.table.supportTables.end (),
                                [=] (const auto& table) { return table.context.tableId == value; });
        };
        while (used (token) || used (token + 1))
            token += 2;
        std::vector<Vec3> points;
        for (const auto& p : ring)
            points.push_back ({ p.x, p.y, 0 });
        EditTerrainSplitResult split;
        if (!SplitEditTerrainRing (doc, current->Input (), { operation, owner, token, token + 1 }, points, sources,
                                   split, error))
            return false;
        token += 2;
        auto next = std::make_shared<TerrainCapture> ();
        next->revision = doc.revision;
        next->sourceTerrainGuid = current->sourceTerrainGuid;
        next->points = split.split.points;
        next->triangles = split.split.triangles;
        next->provenance = split.meshProvenance;
        vertices += next->points.size () / 3;
        if (next->points.size () / 3 > 200000 || vertices > 1000000) {
            error = "RegionTerrainWorkLimit: prepared terrain exceeds 200000 vertices";
            return false;
        }
        out.preparation.push_back (std::move (split));
        current = std::move (next);
        return true;
    }
};
} // namespace

std::vector<Id> TerrainRegionObjects (const geomsrv::edit::Document& doc)
{
    std::vector<Id> ids;
    if (doc.landscape)
        for (const auto& object : doc.landscape->objects)
            if (object.terrain.effect != RegionTerrainEffect::None && object.boundarySource &&
                std::any_of (doc.landscape->sources.begin (), doc.landscape->sources.end (), [&] (const auto& source) {
                    return source.id == object.boundarySource && !source.deleted &&
                           source.role == SourceRole::RegionBoundary;
                }))
                ids.push_back (object.id);
    return ids;
}

bool EvaluateRegionTerrain (const geomsrv::edit::Document& doc, std::shared_ptr<const TerrainCapture> input,
                            RegionTerrainResult& result, std::string& error)
{
    error.clear ();
    try {
        const auto ids = TerrainRegionObjects (doc);
        if (!input || ids.empty () || !doc.landscape || !ValidateLandscape (doc, error)) {
            if (error.empty ())
                error = "RegionTerrain: a captured terrain and enabled region target are required";
            return false;
        }
        RegionTerrainResult out;
        out.input = input;
        LandscapeResult landscape;
        std::map<Id, ld::Shape> footprints;
        if (!ld::EvaluateLandscapeOperations (doc, landscape, &footprints)) {
            error = landscape.error;
            return false;
        }
        const auto cover = TerrainDesignArea (doc, input->Input ().mesh);
        if (cover.size () < 9) {
            error = "RegionTerrainCoverage: terrain/property intersection is empty";
            return false;
        }
        if (input->provenance.table.objects.size () != 1) {
            error = "RegionTerrainOwner: one retained terrain owner required";
            return false;
        }
        Preparation preparation { doc, input->provenance.table.objects[0].id, out, input };
        const auto objectFor = [&] (Id id) -> const HardscapeObject& {
            return *std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                  [=] (const auto& o) { return o.id == id; });
        };
        const auto& first = objectFor (ids[0]);
        if (!landscape.propertyLine.empty ()) {
            cp::PathD property;
            for (const auto& point : landscape.propertyLine)
                property.emplace_back (point.x, point.y);
            geomsrv::edit::Document terrainOnly;
            const auto nativeCover = TerrainDesignArea (terrainOnly, input->Input ().mesh);
            const std::array<SourceId, 1> support { doc.landscape->propertyLineSourceId };
            for (const auto& ring :
                 cp::Intersect ({ property }, { Plan (nativeCover) }, cp::FillRule::NonZero, ld::kPrecision))
                if (!preparation.Split (ring, support, first.footprintOperation, error))
                    return false;
        }
        std::vector<SourceId> coverSources { first.boundarySource };
        for (const auto& source : doc.landscape->sources)
            if (!source.deleted && source.role == SourceRole::Terrain)
                coverSources = { source.id };
        if (doc.landscape->propertyLineSourceId)
            coverSources.push_back (doc.landscape->propertyLineSourceId);
        if (!preparation.Split (Plan (cover), coverSources, first.footprintOperation, error))
            return false;
        std::vector<EditTerrainOperation> edits;
        std::vector<std::vector<double>> targetPoints;
        std::vector<std::vector<uint32_t>> targetTriangles;
        std::vector<Id> order;
        if (!ValidateLandscapeDag (*doc.landscape, order, error))
            return false;
        auto sorted = ids;
        std::sort (sorted.begin (), sorted.end (), [&] (Id a, Id b) {
            return std::find (order.begin (), order.end (), objectFor (a).footprintOperation) <
                   std::find (order.begin (), order.end (), objectFor (b).footprintOperation);
        });
        for (Id id : sorted) {
            const auto& object = objectFor (id);
            const auto& shape = footprints.at (object.footprintOperation);
            if (shape.size () != 1 || shape[0].rings.size () != 1) {
                error = "TerrainFootprintUnsupported: region grading currently requires one hole-free footprint";
                return false;
            }
            const auto paths =
                cp::Intersect (ld::Paths (shape), { Plan (cover) }, cp::FillRule::NonZero, ld::kPrecision);
            if (paths.empty ()) {
                error = "ClipEmpty: region does not intersect the captured terrain/property";
                return false;
            }
            for (const auto& ring : paths)
                if (!preparation.Split (ring, shape[0].sources, object.footprintOperation, error))
                    return false;
            const auto& settings = object.terrain;
            const double reach =
                settings.transition == RegionTerrainTransition::Falloff ? settings.falloff : settings.reach;
            for (size_t step = 1; step <= 9; ++step) {
                // Offset arcs are chords (2 cm error). A 5 cm outer guard lies
                // beyond the edit reach even between those chords, preventing a
                // tiny moved transition vertex from tilting remote base faces.
                const double distance = step == 9 ? reach + 0.05 : reach * double (step) / 8;
                auto rings = cp::InflatePaths (paths, distance, cp::JoinType::Round, cp::EndType::Polygon, 2,
                                               ld::kPrecision, 0.02);
                rings = cp::Intersect (rings, { Plan (cover) }, cp::FillRule::NonZero, ld::kPrecision);
                for (const auto& ring : rings)
                    if (!preparation.Split (ring, shape[0].sources, object.footprintOperation, error))
                        return false;
            }
            TerrainEdit parameters;
            parameters.target = TerrainEditTarget::Area;
            parameters.level = object.surface->first.z;
            parameters.effect = settings.effect == RegionTerrainEffect::Add        ? TerrainEditEffect::Fill
                                : settings.effect == RegionTerrainEffect::Subtract ? TerrainEditEffect::Cut
                                                                                   : TerrainEditEffect::Both;
            parameters.transition = settings.transition == RegionTerrainTransition::Falloff
                                        ? TerrainEditTransition::Falloff
                                        : TerrainEditTransition::Daylight;
            parameters.blur = settings.falloff;
            parameters.maxDistance = reach;
            parameters.cut = { settings.cut, reach };
            parameters.fill = { { TerrainSlope::Unit::Percent, settings.fillPercent, 0 }, reach };
            if (object.surface->elevation == RegionElevationMode::Boundary ||
                object.surface->slope != RegionSlopeMode::None) {
                // The shipped Rust Area target is level-only. Use its existing
                // Surface target for an arbitrary authored plane, never a fake path
                // or an ignored Area crossfall parameter.
                LandscapePreviewMesh target;
                if (!BuildRegionSurfaceMesh (doc, object, shape[0], target, error))
                    return false;
                std::vector<double> points;
                for (const auto& p : target.vertices)
                    points.insert (points.end (), { p.x, p.y, p.z });
                parameters.target = TerrainEditTarget::Surface;
                parameters.surface = static_cast<uint32_t> (targetPoints.size ());
                targetPoints.push_back (std::move (points));
                targetTriangles.push_back (std::move (target.triangles));
            }
            edits.push_back ({ object.footprintOperation, parameters });
        }
        // Constrain every positional mask edge as well as property/footprint
        // edges, so a protected face cannot tilt across an unsplit boundary.
        for (const auto& constraint : landscape.terrainConstraints)
            for (const auto& mask : constraint.masks) {
                for (const auto& ring :
                     cp::Intersect (ld::Paths (mask), { Plan (cover) }, cp::FillRule::NonZero, ld::kPrecision))
                    if (!preparation.Split (ring, mask.sources, constraint.operation, error))
                        return false;
                // Local relief next to pinned mask vertices: without this ring,
                // a coarse face can ramp from a protected edge across the site.
                const auto relief = cp::InflatePaths (ld::Paths (mask), 0.25, cp::JoinType::Round, cp::EndType::Polygon,
                                                      2, ld::kPrecision, 0.02);
                for (const auto& ring : cp::Intersect (relief, { Plan (cover) }, cp::FillRule::NonZero, ld::kPrecision))
                    if (!preparation.Split (ring, mask.sources, constraint.operation, error))
                        return false;
            }
        std::vector<TerrainMeshView> targets;
        for (size_t i = 0; i < targetPoints.size (); ++i)
            targets.push_back ({ targetPoints[i], targetTriangles[i] });
        const auto preparedInput = preparation.current->Input ();
        LandscapeTerrainRequest request { preparedInput };
        request.edits = edits;
        request.surfaces = targets;
        request.fallbackArea = cover;
        request.volumes.emplace ();
        if (!EvaluateLandscapeTerrain (doc, request, out.evaluation, error))
            return false;
        result = std::move (out);
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = std::string ("RegionTerrain: ") + exception.what ();
        return false;
    }
}
} // namespace tapioca::geometry
