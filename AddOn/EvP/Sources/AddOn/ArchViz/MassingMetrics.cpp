#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/FloorProgramme.hpp"
#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace geomsrv::archviz::massingslices {
namespace {
namespace cp = Clipper2Lib;
cp::PathsD Paths (const std::vector<SliceChain>& chains, double ox, double oy)
{
    cp::PathsD paths;
    for (const auto& chain : chains) {
        cp::PathD path;
        for (size_t i = 0; i < chain.Count (); ++i)
            path.emplace_back (chain.xy[i * 2] - ox, chain.xy[i * 2 + 1] - oy);
        if (path.size () >= 3 && chain.closed)
            paths.push_back (std::move (path));
    }
    return cp::Union (paths, cp::FillRule::EvenOdd, 6);
}
// A floor inside its walls (user, 2026-10-10): its contour 0.5 m in for the facade wall, from its
// level to the underside of the next floor's 0.3 m slab. A floor too narrow to keep anything inside
// its facade leaves the mesh empty.
bool Extrude (const Row& floor, overlaylayers::Mesh& mesh, std::string& error)
{
    const auto inside = Inset (floor.chains, floorprogramme::kFacade);
    if (inside.empty ())
        return true;
    const double top = floor.z + (std::max) (0.0, floor.floorHeight - floorprogramme::kSlab);
    if (!ExtrudeChains (inside, floor.z, top, mesh, error))
        return false;
    mesh.rgba = floor.fillRgba;
    mesh.styled = true;
    mesh.style.opacity = floor.fillOpacity;
    mesh.style.edgeRgba = floor.rgba | 0xFFu;
    mesh.style.edgeWidthPixels = floor.wireWidthPixels;
    mesh.style.behind = overlaylayers::Behind::Show;
    mesh.style.cullBack = false;
    mesh.hoverTitle = floor.function + " floor " + std::to_string (floor.story);
    return true;
}
using FloorTargets = std::map<std::pair<std::string, int>, std::string>;
bool HighlightRows (const Result& result, const FloorTargets& targets, overlaylayers::Layer& layer, std::string& error)
{
    size_t points = 0;
    for (const auto& row : result.rows) {
        const auto target = targets.find ({ row.guid, row.story });
        if (target == targets.end ())
            continue;
        auto floor = row;
        floor.chains = row.footprintChains.empty () ? row.rawChains : row.highlightChains;
        floor.rgba = 0xD9822BFF;
        floor.fillRgba = 0xD9822B90;
        floor.fillOpacity = 1;
        floor.wireWidthPixels = 2.5f;
        overlaylayers::Mesh mesh;
        if (!Extrude (floor, mesh, error))
            return false;
        if (mesh.indices.empty ())
            continue;
        mesh.style.behind = overlaylayers::Behind::Fade;
        mesh.hoverTitle = target->second;
        points += mesh.points.size () / 3;
        if (points > 600000)
            return error = "Floor highlight exceeds its geometry budget.", false;
        layer.meshes.push_back (std::move (mesh));
    }
    error = overlaylayers::Validate (layer);
    return error.empty ();
}
} // namespace

std::vector<SliceChain> Inset (const std::vector<SliceChain>& chains, double distance)
{
    std::vector<SliceChain> out;
    if (chains.empty () || chains.front ().xy.size () < 2)
        return out;
    const double ox = chains.front ().xy[0], oy = chains.front ().xy[1];
    const auto inside =
        cp::InflatePaths (Paths (chains, ox, oy), -distance, cp::JoinType::Miter, cp::EndType::Polygon, 2.0, 6);
    for (const auto& path : inside) {
        if (std::abs (cp::Area (path)) < 1e-6)
            continue;
        SliceChain chain;
        chain.closed = true;
        for (const auto& p : path)
            chain.xy.insert (chain.xy.end (), { p.x + ox, p.y + oy });
        out.push_back (std::move (chain));
    }
    return out;
}
bool ExtrudeChains (const std::vector<SliceChain>& chains, double bottom, double top, overlaylayers::Mesh& mesh,
                    std::string& error, bool topCap)
{
    if (chains.empty ())
        return true;
    const double ox = chains.front ().xy[0], oy = chains.front ().xy[1];
    const auto paths = Paths (chains, ox, oy);
    std::vector<SliceChain> local;
    for (const auto& path : paths) {
        SliceChain chain;
        chain.closed = true;
        for (const auto& p : path)
            chain.xy.insert (chain.xy.end (), { p.x, p.y });
        local.push_back (std::move (chain));
    }
    std::vector<StorySliceFillVertex> triangles;
    BuildSliceFill (local, 0, triangles);
    if (triangles.empty ()) {
        error = "Could not triangulate floor volume; no partial highlight published.";
        return false;
    }
    const auto vertex = [&] (double x, double y, double z) {
        mesh.indices.push_back (uint32_t (mesh.points.size () / 3));
        mesh.points.insert (mesh.points.end (), { x + ox, y + oy, z });
    };
    for (size_t i = 0; i + 2 < triangles.size (); i += 3) {
        auto a = triangles[i], b = triangles[i + 1], c = triangles[i + 2];
        if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) < 0)
            std::swap (b, c);
        if (topCap) {
            vertex (a.x, a.y, top);
            vertex (b.x, b.y, top);
            vertex (c.x, c.y, top);
        }
        vertex (c.x, c.y, bottom);
        vertex (b.x, b.y, bottom);
        vertex (a.x, a.y, bottom);
    }
    for (const auto& path : paths)
        for (size_t i = 0; i < path.size (); ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size ()];
            vertex (a.x, a.y, bottom);
            vertex (b.x, b.y, bottom);
            vertex (b.x, b.y, top);
            vertex (a.x, a.y, bottom);
            vertex (b.x, b.y, top);
            vertex (a.x, a.y, top);
        }
    return true;
}
std::vector<Usage> UsageMix (const Result& result)
{
    std::map<std::string, Usage> uses;
    double total = 0;
    const auto schema = metadata::DefaultSchema ();
    const auto* palette = schema.FindEnumeration ("building-usage");
    for (const auto& row : result.rows) {
        const double area = result.clipped ? row.allowedArea : row.rawArea;
        const double volume = result.clipped ? row.allowedVolume : row.rawVolume;
        if (area <= 0 && volume <= 0)
            continue;
        auto& use = uses[row.function];
        use.function = use.label = row.function;
        use.rgba = row.rgba;
        if (palette)
            for (const auto& option : palette->options)
                if (option.value == row.function)
                    use.label = option.label;
        use.area += area;
        use.volume += volume > 0 ? volume : area * row.floorHeight;
        total += area;
    }
    std::vector<Usage> out;
    for (auto& item : uses) {
        item.second.percent = total > 0 ? 100 * item.second.area / total : 0;
        out.push_back (std::move (item.second));
    }
    return out;
}

bool FloorHighlight (const Result& result, const std::string& building, const hudsection::Run& run,
                     overlaylayers::Layer& layer, std::string& error)
{
    error.clear ();
    overlaylayers::Layer out;
    out.name = massingbuildings::kFloorsLayer;
    out.views = overlaylayers::Views::Both;
    out.occlusion = overlaylayers::Behind::Fade;
    std::vector<massingbuildings::Record> records;
    for (const auto& surface : result.buildingSurfaces)
        records.push_back (surface.record);
    FloorTargets targets;
    for (const auto& group : massingbuildings::Groups (records)) {
        if (group.key != building)
            continue;
        const auto previews = massingbuildings::Previews (result.section, records, {}, group.guids);
        for (const auto& preview : previews)
            for (const auto& floor : preview.section.floors)
                if (run.Has (floor.storey))
                    for (const auto& part : floor.parts)
                        targets[{ part.guid, part.sourceStory }] =
                            "Building " + group.id + " floor " + std::to_string (floor.storey);
    }
    if (!HighlightRows (result, targets, out, error))
        return false;
    layer = std::move (out);
    return true;
}

bool LargeFloorHighlight (const Result& result, const std::vector<massingbuildings::Record>& records,
                          const massingareas::Coefficients& coefficients, overlaylayers::Layer& layer,
                          std::string& error)
{
    error.clear ();
    if (!massingareas::Valid (coefficients))
        return error = "Large-floor inspection needs valid area coefficients.", false;
    std::vector<std::string> seeds;
    for (const auto& surface : result.buildingSurfaces)
        seeds.push_back (surface.record.guid);
    for (const auto& guid : seeds)
        if (std::none_of (records.begin (), records.end (), [&] (const auto& record) { return record.guid == guid; }))
            return error = "Large-floor inspection awaits the building identity index.", false;
    const auto previews = massingbuildings::Previews (result.section, records, {}, seeds);
    FloorTargets targets;
    for (const auto& preview : previews) {
        if (!preview.section.known)
            return error = "Large-floor inspection awaits complete building story slices; no partial marks shown.",
                   false;
        for (const auto& floor : preview.section.floors) {
            const double gross = massingareas::Calculate (floor.areaM2, coefficients).gross;
            if (gross <= 500)
                continue;
            const std::string title =
                (preview.building.id.empty () ? "Unassigned slab" : "Building " + preview.building.id) + " floor " +
                std::to_string (floor.storey) + " (gross " + hudmeta::NumberText (gross) + " m2 > 500 m2)";
            for (const auto& part : floor.parts)
                targets[{ part.guid, part.sourceStory }] = title;
        }
    }
    overlaylayers::Layer out;
    out.name = kLargeFloorsLayer;
    out.views = overlaylayers::Views::Both;
    out.occlusion = overlaylayers::Behind::Fade;
    if (!HighlightRows (result, targets, out, error))
        return false;
    layer = std::move (out);
    return true;
}

bool Coverage (Result& result, const massingcalculation::Preview& preview, std::string& error)
{
    error.clear ();
    result.hasCoverage = false;
    result.coverage.clear ();
    const auto parcels = massingcalculation::Expand (preview.inputs);
    if (parcels.empty () || parcels.size () > massingcalculation::kMaxParcels)
        return false;
    double built = 0, total = 0;
    std::vector<CoveragePatch> patches;
    // Calculate independently in each parcel's local frame. This matches the
    // requested parcel SUM even for overlapping parcels, without counting stacked floors.
    size_t work = 0;
    for (const auto& parcel : parcels) {
        if (!parcel.before.known || parcel.before.edges.empty ())
            return false;
        const double ox = parcel.before.edges.front ().ax, oy = parcel.before.edges.front ().ay;
        cp::PathD boundary;
        for (const auto& edge : parcel.before.edges) {
            if (std::abs (edge.arcAngle) > 1e-8)
                return false; // no chord substitute for unsupported curved parcels
            boundary.emplace_back (edge.ax - ox, edge.ay - oy);
        }
        cp::PathsD all;
        for (const auto& row : result.rows) {
            const auto& physical = row.footprintChains.empty () ? row.rawChains : row.footprintChains;
            for (const auto& chain : physical)
                work += chain.Count ();
            if (work > 2000000) {
                error = "Parcel footprint union exceeds its work budget.";
                return false;
            }
            const auto paths = Paths (physical, ox, oy);
            all.insert (all.end (), paths.begin (), paths.end ());
        }
        const auto footprint = cp::Union (all, cp::FillRule::NonZero, 6);
        const auto inside = cp::Intersect (footprint, { boundary }, cp::FillRule::NonZero, 6);
        const auto outside = cp::Difference ({ boundary }, footprint, cp::FillRule::NonZero, 6);
        CoveragePatch patch;
        const auto retain = [&] (const cp::PathsD& paths, std::vector<SliceChain>& chains) {
            for (const auto& path : paths) {
                SliceChain chain;
                chain.closed = true;
                for (const auto& p : path)
                    chain.xy.insert (chain.xy.end (), { p.x + ox, p.y + oy });
                chains.push_back (std::move (chain));
            }
        };
        retain (inside, patch.built);
        retain (outside, patch.unbuilt);
        for (const auto& row : result.rows)
            if ((!row.footprintChains.empty () || !row.rawChains.empty ()) && std::isfinite (row.z) &&
                (!patch.hasElevation || row.z < patch.z)) {
                patch.hasElevation = true;
                patch.z = row.z;
            }
        const auto adopted = std::find_if (preview.parcels.begin (), preview.parcels.end (),
                                           [&] (const auto& p) { return p.inputs.before.guid == parcel.before.guid; });
        const auto& ground = adopted == preview.parcels.end () ? preview.result : adopted->result;
        if (ground.hasMeanZ && std::isfinite (ground.meanZ)) {
            patch.z = ground.meanZ;
            patch.hasElevation = true;
        }
        patches.push_back (std::move (patch));
        const double parcelArea = std::abs (cp::Area (boundary));
        const double occupied = std::abs (cp::Area (inside));
        if (!std::isfinite (parcelArea) || parcelArea <= 0 || !std::isfinite (occupied) || occupied > parcelArea + 1e-5)
            return false;
        total += parcelArea;
        built += (std::min) (occupied, parcelArea);
    }
    result.hasCoverage = true;
    result.parcelArea = total;
    result.builtArea = built;
    result.unbuiltArea = total - built;
    result.coverage = std::move (patches);
    return true;
}

bool Highlight (const Result& result, const std::string& function, overlaylayers::Layer& layer, std::string& error)
{
    error.clear ();
    if (function == kUnbuiltHover) {
        overlaylayers::Layer projected;
        return UnbuiltHighlight (result, nullptr, layer, projected, error);
    }
    overlaylayers::Layer out;
    out.name = kHighlightLayer;
    out.graphicsCategory =
        function == kBuiltHover || function == kUnbuiltHover ? "coverageHighlight" : "functionHighlight";
    out.views = overlaylayers::Views::Both;
    out.occlusion = overlaylayers::Behind::Show;
    size_t points = 0;
    if (function == kBuiltHover || function == kUnbuiltHover) {
        if (!result.hasCoverage) {
            error = "Parcel coverage highlight awaits complete current footprint geometry.";
            return false;
        }
        if (std::any_of (result.coverage.begin (), result.coverage.end (),
                         [] (const auto& p) { return !p.hasElevation; }))
            out.views = overlaylayers::Views::TwoD; // No invented 3D ground elevation for an empty/no-terrain site.
        size_t work = 0;
        for (const auto& patch : result.coverage) {
            const auto& chains = function == kBuiltHover ? patch.built : patch.unbuilt;
            if (chains.empty ())
                continue;
            const double ox = chains[0].xy[0], oy = chains[0].xy[1];
            std::vector<SliceChain> local = chains;
            for (auto& chain : local) {
                work += chain.Count ();
                if (work > 200000)
                    return error = "Coverage highlight contour budget exceeded.", false;
                for (size_t i = 0; i < chain.Count (); ++i) {
                    chain.xy[i * 2] -= ox;
                    chain.xy[i * 2 + 1] -= oy;
                }
            }
            std::vector<StorySliceFillVertex> triangles;
            BuildSliceFill (local, 0, triangles);
            if (triangles.empty ())
                return error = "Could not triangulate the complete coverage highlight.", false;
            overlaylayers::Mesh mesh;
            mesh.rgba = function == kBuiltHover ? 0x9AA0A6FF : 0x66BB6AFF;
            mesh.styled = true;
            mesh.style.opacity = 0.65f;
            mesh.style.behind = overlaylayers::Behind::Show;
            mesh.style.cullBack = false;
            for (const auto& p : triangles) {
                mesh.indices.push_back (uint32_t (mesh.points.size () / 3));
                mesh.points.insert (mesh.points.end (), { p.x + ox, p.y + oy, patch.z + 0.012 });
            }
            points += mesh.points.size () / 3;
            if (points > 600000)
                return error = "Coverage highlight fill budget exceeded.", false;
            out.meshes.push_back (std::move (mesh));
        }
    }
    else if (!function.empty ())
        for (const auto& floor : result.rows) {
            if (floor.function != function || floor.chains.empty ())
                continue;
            overlaylayers::Mesh mesh;
            if (!Extrude (floor, mesh, error))
                return false;
            if (mesh.indices.empty ())
                continue;
            mesh.graphicsFunction = floor.function;
            points += mesh.points.size () / 3;
            if (points > 600000) {
                error = "Floor-volume highlight exceeds its geometry budget.";
                return false;
            }
            out.meshes.push_back (std::move (mesh));
        }
    error = overlaylayers::Validate (out);
    if (!error.empty ())
        return false;
    layer = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingslices
