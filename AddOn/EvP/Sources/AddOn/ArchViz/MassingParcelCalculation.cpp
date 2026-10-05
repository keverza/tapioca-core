#include "ArchViz/MassingCalculation.hpp"

#include <cmath>
#include <iterator>
#include <set>

namespace geomsrv::archviz::massingcalculation {

std::vector<Request> Expand (const Request& request)
{
    if (request.parcels.empty ())
        return { request };
    std::vector<Request> out;
    for (const auto& parcel : request.parcels) {
        auto single = request;
        single.parcels.clear ();
        single.before = parcel.before;
        single.assignments = parcel.assignments;
        single.regulated = parcel.regulated;
        single.endpoints = parcel.endpoints;
        out.push_back (std::move (single));
    }
    return out;
}

bool Combine (const Request& inputs, std::vector<ParcelPreview> parcels, Preview& preview, std::string& error)
{
    error.clear ();
    const auto expected = Expand (inputs);
    if (parcels.empty () || parcels.size () > kMaxParcels || parcels.size () != expected.size ()) {
        error = "Massing preview needs a complete set of 1..32 parcels.";
        return false;
    }
    Preview out;
    out.inputs = inputs;
    auto& result = out.result;
    result.layer.name = "tapioca.massing.envelope";
    result.site.name = "tapioca.massing.lines";
    result.layer.occlusion = result.site.occlusion = overlaylayers::Behind::Dash;
    result.hasEnvelope = true;
    result.hasMeanZ = result.hasMeanASL = true;
    double weights = 0;
    std::set<std::string> guids;
    for (size_t i = 0; i < parcels.size (); ++i) {
        const auto& parcel = parcels[i];
        const auto& part = parcel.result;
        if (!SameRequest (parcel.inputs, expected[i]) || !guids.insert (parcel.inputs.before.guid).second ||
            !std::isfinite (part.parcelArea) || part.parcelArea <= 0 || !std::isfinite (part.allowedArea) ||
            part.allowedArea < 0 || part.allowedArea > part.parcelArea + 1e-5 ||
            (part.hasEnvelope && part.layer.meshes.size () != 1)) {
            error = "Invalid, duplicate or mismatched parcel preview.";
            return false;
        }
        result.parcelArea += part.parcelArea; // Sum parcels, not their planar union.
        result.allowedArea += part.allowedArea;
        result.faces += part.faces;
        result.hasEnvelope = result.hasEnvelope && part.hasEnvelope;
        result.hasMeanZ = result.hasMeanZ && part.hasMeanZ;
        result.hasMeanASL = result.hasMeanASL && part.hasMeanASL;
        // Each solver mean uses its reference endpoints. Weight the site mean by
        // that same count, not parcel area or the count of parcels.
        size_t count = 0;
        for (const auto& points : part.site.points)
            count += points.points.size () / 3;
        const double weight = double (count);
        if (part.hasMeanZ)
            result.meanZ += part.meanZ * weight;
        if (part.hasMeanASL)
            result.meanASL += part.meanASL * weight;
        weights += weight;
        if (!part.note.empty ()) {
            if (!result.note.empty ())
                result.note += " ";
            result.note += "Parcel " + std::to_string (i + 1) + ": " + part.note;
        }
        for (const auto& mesh : part.layer.meshes) {
            auto shell = mesh;
            shell.hoverTitle = "Parcel " + std::to_string (i + 1) + " envelope";
            result.layer.meshes.push_back (std::move (shell));
        }
        result.layer.polylines.insert (result.layer.polylines.end (), part.layer.polylines.begin (),
                                       part.layer.polylines.end ());
        result.site.polylines.insert (result.site.polylines.end (), part.site.polylines.begin (),
                                      part.site.polylines.end ());
        result.site.points.insert (result.site.points.end (), part.site.points.begin (), part.site.points.end ());
    }
    if (weights > 0) {
        result.meanZ /= weights;
        result.meanASL /= weights;
    }
    else
        result.hasMeanZ = result.hasMeanASL = false;
    if (!result.hasEnvelope)
        result.note += " Allowed story areas await valid envelopes for every parcel.";
    if (parcels.size () == 1)
        result.offsetXY = parcels.front ().result.offsetXY;
    error = overlaylayers::Validate (result.layer);
    if (error.empty ())
        error = overlaylayers::Validate (result.site);
    if (!error.empty ())
        return false;
    out.parcels = std::move (parcels);
    preview = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::massingcalculation
