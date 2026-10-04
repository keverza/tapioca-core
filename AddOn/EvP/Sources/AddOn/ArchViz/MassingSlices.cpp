#include "ArchViz/MassingSlices.hpp"
#include "Geometry/SliceEngine.hpp"
#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace geomsrv::archviz::massingslices {
namespace {
namespace meta = metadata;
namespace cp = Clipper2Lib;
bool Fail (std::string& error, const char* text)
{
    error = text;
    return false;
}

double FloorHeight (const meta::EntityMetadata& entity)
{
    const auto* value = meta::FindProperty (entity, "massing.floorHeight");
    if (!value)
        value = meta::FindProperty (entity, "massing.height");
    return value && meta::IsNumber (value->value.type) ? value->value.AsNumber () : 3;
}

void Append (overlaylayers::Layer& into, overlaylayers::Layer from)
{
    into.polylines.insert (into.polylines.end (), std::make_move_iterator (from.polylines.begin ()),
                           std::make_move_iterator (from.polylines.end ()));
    into.meshes.insert (into.meshes.end (), std::make_move_iterator (from.meshes.begin ()),
                        std::make_move_iterator (from.meshes.end ()));
}

bool CutEnvelope (const overlaylayers::Mesh& envelope, double z, std::vector<SliceChain>& outlines, std::string& error)
{
    if (!std::isfinite (z) || envelope.points.empty () || envelope.points.size () % 3 ||
        envelope.points.size () > 60000 || envelope.indices.empty () || envelope.indices.size () % 3 ||
        envelope.indices.size () > 60000)
        return Fail (error, "Invalid envelope intersection buffers.");
    for (double value : envelope.points)
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return Fail (error, "Non-finite or out-of-range envelope coordinate.");
    for (uint32_t index : envelope.indices)
        if (index >= envelope.points.size () / 3)
            return Fail (error, "Invalid envelope intersection index.");
    // The envelope is flat-shaded, so duplicate face vertices are welded by SliceMesh.
    const auto loops = SliceMesh (envelope.points.data (), envelope.points.size () / 3, envelope.indices.data (),
                                  envelope.indices.size () / 3, z);
    for (const auto& loop : loops) {
        if (!loop.closed)
            return Fail (error, "Envelope cut has an open contour; no partial allowed slice published.");
        SliceChain ring;
        ring.closed = true;
        for (size_t i = 0; i + 1 < loop.PointCount (); ++i)
            ring.xy.insert (ring.xy.end (), { loop.pts[i * 3], loop.pts[i * 3 + 1] });
        if (ring.Count () >= 3)
            outlines.push_back (std::move (ring));
    }
    return true;
}

bool ClipContours (const std::vector<SliceChain>& slab, const std::vector<SliceChain>& envelope,
                   std::vector<SliceChain>& outlines, double& area, std::string& error)
{
    if (slab.empty ())
        return Fail (error, "Empty slab intersection contour.");
    for (const auto& ring : slab)
        if (!ring.closed || ring.Count () < 3 || ring.xy.size () % 2 || ring.xy.size () > 200000)
            return Fail (error, "Story slice has an open or invalid contour.");
    const double ox = slab[0].xy[0], oy = slab[0].xy[1];
    cp::PathsD subject, clip;
    for (const auto* rings : { &slab, &envelope })
        for (const auto& ring : *rings) {
            cp::PathD path;
            for (size_t i = 0; i < ring.xy.size (); i += 2) {
                if (!std::isfinite (ring.xy[i]) || !std::isfinite (ring.xy[i + 1]) || std::abs (ring.xy[i]) > 1e9 ||
                    std::abs (ring.xy[i + 1]) > 1e9)
                    return Fail (error, "Non-finite or out-of-range story slice contour.");
                path.emplace_back (ring.xy[i] - ox, ring.xy[i + 1] - oy);
            }
            (rings == &slab ? subject : clip).push_back (std::move (path));
        }
    const cp::PathsD paths = cp::BooleanOp (cp::ClipType::Intersection, cp::FillRule::EvenOdd, subject, clip, 6);
    std::vector<SliceChain> final;
    for (const auto& path : paths) {
        SliceChain ring;
        ring.closed = true;
        for (const auto& point : path) {
            ring.xy.push_back (point.x + ox);
            ring.xy.push_back (point.y + oy);
        }
        final.push_back (std::move (ring));
    }
    const double net = std::abs (cp::Area (paths));
    if (!std::isfinite (net))
        return Fail (error, "Non-finite intersection area.");
    outlines = std::move (final);
    area = net;
    return true;
}
} // namespace

bool Intersect (const std::vector<SliceChain>& slab, const overlaylayers::Mesh& envelope, double z,
                std::vector<SliceChain>& outlines, double& area, std::string& error)
{
    error.clear ();
    std::vector<SliceChain> cut;
    return CutEnvelope (envelope, z, cut, error) && ClipContours (slab, cut, outlines, area, error);
}

bool Build (const std::vector<Input>& slabs, const ProjectStoreys& storeys, const massingcalculation::Result* envelope,
            Result& result, std::string& error)
{
    error.clear ();
    if (slabs.size () > 128)
        return Fail (error, "Automatic story slice budget is 128 slabs.");
    Result out;
    out.layer.name = kLayer;
    out.layer.occlusion = overlaylayers::Behind::Dash;
    out.clipped = envelope && envelope->hasEnvelope && envelope->layer.meshes.size () == 1;
    const auto schema = meta::DefaultSchema ();
    const auto* palette = schema.FindEnumeration ("building-usage");
    std::map<double, std::vector<SliceChain>> cuts;
    std::vector<hudsection::Slab> masses;
    for (const auto& input : slabs) {
        const double height = FloorHeight (input.metadata);
        if (!std::isfinite (height) || height < 0.01 || height > 1000)
            return Fail (error, "massing.floorHeight must be a positive floor-to-floor interval.");
        if (input.slab.slopedEdges)
            return Fail (error, "A sloped slab edge needs a body cut, not a prism story preview.");
        slabslices::Rule rule;
        rule.cut = slabslices::Cut::Step;
        rule.stepMetres = height;
        if (input.slab.top - input.slab.bottom <= height) {
            rule.cut = slabslices::Cut::Levels; // a normal thin slab is one floor, not zero floors
            rule.levels = { input.slab.bottom };
        }
        std::vector<storysliceoverlay::Slice> slices;
        const auto summary = slabslices::SliceSlab (input.slab, rule, storeys, slices);
        if (!summary.problem.empty ())
            return Fail (error, summary.problem.c_str ());
        const auto* use = meta::FindProperty (input.metadata, "massing.function");
        const std::string function = use ? use->value.s : "residential";
        const auto* story = meta::FindProperty (input.metadata, "massing.story");
        if (story && (!meta::IsNumber (story->value.type) || !std::isfinite (story->value.AsNumber ()) ||
                      story->value.AsNumber () != std::floor (story->value.AsNumber ()) ||
                      std::abs (story->value.AsNumber ()) > 10000))
            return Fail (error, "massing.story must be an integer between -10000 and 10000.");
        const auto* building = meta::FindProperty (input.metadata, "massing.buildingId");
        hudsection::Slab mass;
        mass.guid = input.slab.guid;
        mass.meta = input.metadata;
        if (!use) {
            meta::Property fallback;
            fallback.key = "massing.function";
            fallback.value = meta::Value::Text (function);
            meta::SetProperty (mass.meta, std::move (fallback));
        }
        for (size_t i = 0; i < slices.size (); ++i) {
            if (out.rows.size () >= 512)
                return Fail (error, "Automatic story slice budget is 512 levels.");
            auto slice = slices[i];
            Row row;
            row.guid = input.slab.guid;
            row.function = function;
            row.story = (story ? int (story->value.AsNumber ()) : slices.front ().storey) + int (i);
            if (const auto* value =
                    meta::RangeValue (input.metadata, meta::kFloorDomain, row.story, "massing.function"))
                row.function = value->value.s;
            uint32_t colour = 0x9AA0A6FF;
            if (palette)
                for (const auto& option : palette->options)
                    if (option.value == row.function)
                        colour = option.rgba;
            row.z = slice.z;
            row.floorHeight = height;
            row.rawArea = slice.areaM2;
            row.clipped = out.clipped;
            if (out.clipped) {
                const auto cut = cuts.try_emplace (slice.z);
                if (cut.second && !CutEnvelope (envelope->layer.meshes[0], slice.z, cut.first->second, error))
                    return false;
                if (!ClipContours (slice.chains, cut.first->second, slice.chains, slice.areaM2, error))
                    return false;
                row.allowedArea = slice.areaM2;
            }
            out.rawArea += row.rawArea;
            out.allowedArea += row.allowedArea;
            out.rows.push_back (row);
            mass.floors.push_back ({ row.z, summary.floors[i].height, out.clipped ? row.allowedArea : row.rawArea });
            mass.storeys.push_back (row.story);
            slice.name = (building ? building->value.s : input.slab.id) + " S" + std::to_string (row.story);
            if (slice.chains.empty ())
                continue; // a level outside the shell counts zero and draws nothing
            storysliceoverlay::Controls controls;
            controls.views = overlaylayers::Views::Both;
            controls.fillRgba = (colour & 0xFFFFFF00) | 0x59;
            controls.outlineRgba = colour;
            controls.label = false;
            controls.liftMetres = 0.015;
            Append (out.layer, storysliceoverlay::BuildLayer ({ slice }, controls).layer);
        }
        masses.push_back (std::move (mass));
    }
    out.section = hudsection::Build (masses, storeys, schema, "massing.function");
    if (!slabs.empty ())
        out.note =
            out.clipped
                ? "Story slices intersected with the allowed envelope; areas sum per slab (overlaps count twice)."
                : "Showing slab story slices; allowed areas wait for the envelope.";
    error = overlaylayers::Validate (out.layer);
    if (!error.empty ())
        return false;
    result = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingslices
