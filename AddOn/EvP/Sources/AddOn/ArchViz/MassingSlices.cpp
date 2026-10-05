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

bool FloorHeights (const meta::EntityMetadata& entity, std::vector<double>& heights, std::string& error)
{
    const auto* value = meta::FindProperty (entity, "massing.floorHeight");
    if (!value)
        value = meta::FindProperty (entity, "massing.height");
    if (!value)
        heights = { 3 };
    else if (meta::IsNumber (value->value.type))
        heights = { value->value.AsNumber () };
    else if (value->value.type == meta::ValueType::List && value->value.elementType == meta::ValueType::Length)
        for (const auto& height : value->value.list) {
            if (height.type != meta::ValueType::Length)
                return Fail (error, "Floor heights must be lengths in metres.");
            heights.push_back (height.d);
        }
    if (heights.empty () || heights.size () > 512)
        return Fail (error, "massing.floorHeight needs 1..512 entries.");
    for (double height : heights)
        if (!std::isfinite (height) || height < 2.2 || height > 6)
            return Fail (error, "massing.floorHeight entries must be 2.2 - 6.0 m.");
    return true;
}

// For vertical prisms the boundary of the XY union in each Z band is exactly
// the exposed wall: coincident/internal faces disappear before perimeter is measured.
bool Facade (const std::vector<Input>& inputs, double& area)
{
    if (inputs.empty ()) {
        area = 0;
        return true;
    }
    std::vector<double> levels;
    std::vector<cp::PathsD> footprints;
    const double ox = inputs.front ().slab.outer.xy[0], oy = inputs.front ().slab.outer.xy[1];
    size_t points = 0;
    for (const auto& input : inputs) {
        levels.insert (levels.end (), { input.slab.bottom, input.slab.top });
        cp::PathsD rings;
        auto add = [&] (const slabslices::Ring& ring) {
            const auto contour = slabslices::Contour (ring, 0.05);
            cp::PathD path;
            for (size_t i = 0; i + 1 < contour.xy.size (); i += 2)
                path.emplace_back (contour.xy[i] - ox, contour.xy[i + 1] - oy);
            points += path.size ();
            rings.push_back (std::move (path));
        };
        add (input.slab.outer);
        for (const auto& hole : input.slab.holes)
            add (hole);
        if (points > 20000)
            return false;
        footprints.push_back (cp::Union (rings, cp::FillRule::EvenOdd, 6));
    }
    std::sort (levels.begin (), levels.end ());
    levels.erase (std::unique (levels.begin (), levels.end ()), levels.end ());
    double total = 0;
    size_t work = 0;
    for (size_t k = 1; k < levels.size (); ++k) {
        const double z = (levels[k - 1] + levels[k]) * 0.5;
        cp::PathsD active;
        for (size_t i = 0; i < inputs.size (); ++i)
            if (inputs[i].slab.bottom < z && inputs[i].slab.top > z)
                for (const auto& ring : footprints[i]) {
                    work += ring.size ();
                    if (work > 1000000)
                        return false;
                    active.push_back (ring);
                }
        double perimeter = 0;
        for (const auto& ring : cp::Union (active, cp::FillRule::NonZero, 6))
            for (size_t i = 0; i < ring.size (); ++i) {
                const auto& a = ring[i];
                const auto& b = ring[(i + 1) % ring.size ()];
                perimeter += std::hypot (b.x - a.x, b.y - a.y);
            }
        total += perimeter * (levels[k] - levels[k - 1]);
    }
    area = total;
    return std::isfinite (total);
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
        if (!std::isfinite (input.slab.bottom) || !std::isfinite (input.slab.top) ||
            std::abs (input.slab.bottom) > 1e9 || std::abs (input.slab.top) > 1e9 ||
            input.slab.top <= input.slab.bottom)
            return Fail (error, "Invalid massing slab height.");
        auto rings = input.slab.holes;
        rings.push_back (input.slab.outer);
        size_t coordinates = 0;
        for (const auto& ring : rings) {
            coordinates += ring.xy.size ();
            if (ring.xy.size () % 2 || coordinates > 200000)
                return Fail (error, "Massing slab contour exceeds its geometry budget.");
            for (double coordinate : ring.xy)
                if (!std::isfinite (coordinate) || std::abs (coordinate) > 1e9)
                    return Fail (error, "Invalid massing slab contour.");
        }
        std::vector<double> heights;
        const auto* mode = meta::FindProperty (input.metadata, "massing.heightMode");
        const bool archicad = mode && mode->value.s == "archicad";
        if (!archicad && !FloorHeights (input.metadata, heights, error))
            return false;
        if (archicad && storeys.levels.size () < 2)
            return Fail (error, "Match Archicad stories needs at least two project story levels.");
        if (input.slab.slopedEdges)
            return Fail (error, "A sloped slab edge needs a body cut, not a prism story preview.");
        slabslices::Rule rule;
        rule.cut = slabslices::Cut::Levels;
        rule.levels = { input.slab.bottom };
        if (archicad) {
            for (double z : storeys.levels)
                if (!std::isfinite (z))
                    return Fail (error, "Invalid project story elevation.");
                else if (z > input.slab.bottom + 1e-6 && z < input.slab.top - 1e-6)
                    rule.levels.push_back (z);
        }
        else {
            double z = input.slab.bottom;
            for (size_t i = 0;; ++i) {
                z += heights[(std::min) (i, heights.size () - 1)];
                if (z >= input.slab.top - 1e-6 || input.slab.top - z < 2.2 - 1e-6)
                    break;
                if (rule.levels.size () >= 512)
                    return Fail (error, "Automatic story slice budget is 512 levels.");
                rule.levels.push_back (z);
            }
        }
        std::vector<storysliceoverlay::Slice> slices;
        const auto summary = slabslices::SliceSlab (input.slab, rule, storeys, slices);
        if (!summary.problem.empty ())
            return Fail (error, summary.problem.c_str ());
        const auto* use = meta::FindProperty (input.metadata, "massing.function");
        const std::string function = use ? use->value.s : "residential";
        const auto* building = meta::FindProperty (input.metadata, "massing.buildingId");
        hudsection::Slab mass;
        mass.guid = input.slab.guid;
        mass.meta = input.metadata;
        hudmeta::Page heightPage;
        heightPage.known = true;
        heightPage.elements = heightPage.selected = 1;
        heightPage.element = input.slab.guid;
        hudmeta::Field source;
        source.id = "massing.heightMode";
        source.label = "Height source";
        source.group =
            (building ? building->value.s : input.slab.id) + " - " + std::to_string (slices.size ()) + " stories";
        source.kind = hudmeta::FieldKind::Choice;
        source.type = meta::ValueType::Enum;
        source.set = true;
        source.text = archicad ? "archicad" : "override";
        source.options = { { "override", "Override heights" }, { "archicad", "Match Archicad stories" } };
        heightPage.fields.push_back (source);
        if (!archicad) {
            hudmeta::Field field;
            field.id = "massing.floorHeight";
            field.label = "Floor heights";
            field.group = source.group;
            field.kind = hudmeta::FieldKind::Heights;
            field.type = meta::ValueType::List;
            field.numbers = heights;
            heightPage.fields.push_back (std::move (field));
        }
        out.heightControls.push_back (std::move (heightPage));
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
            // Floor identity follows elevation, independent of the derived story count.
            row.story = archicad ? slice.storey : slices.front ().storey + int (i);
            if (const auto* value =
                    meta::RangeValue (input.metadata, meta::kFloorDomain, row.story, "massing.function"))
                row.function = value->value.s;
            uint32_t colour = 0x9AA0A6FF;
            if (palette)
                for (const auto& option : palette->options)
                    if (option.value == row.function)
                        colour = option.rgba;
            row.z = slice.z;
            row.floorHeight = archicad ? summary.floors[i].height : heights[(std::min) (i, heights.size () - 1)];
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
            if (i == 0) {
                out.rawFirstFloorArea += row.rawArea;
                out.firstFloorArea += row.allowedArea;
            }
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
    for (auto& floor : out.section.floors)
        floor.label = "Floor " + std::to_string (floor.storey + 1);
    out.hasFacade = Facade (slabs, out.facadeArea);
    if (!slabs.empty ())
        out.note =
            out.clipped
                ? "Story slices intersected with the allowed envelope; areas sum per slab (overlaps count twice)."
                : "Showing slab story slices; allowed areas wait for the envelope.";
    if (!out.hasFacade)
        out.note += " Facade union exceeded its geometry budget; no partial wall area reported.";
    error = overlaylayers::Validate (out.layer);
    if (!error.empty ())
        return false;
    result = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingslices
