#include "Edit/EditLandscape.hpp"
#include "Edit/EditDocument.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>

namespace tapioca::geometry {
namespace edit = geomsrv::edit;
namespace {

// The same copy/rebuild/validate/publish transaction as the other landscape
// mutations. No SDF-specific document, undo stack or terrain write is created.
bool Mutate (edit::Document& doc, const std::function<bool (edit::Document&, std::string&)>& action, std::string& error)
{
    error.clear ();
    if (!doc.landscape) {
        error = "landscape session is not started";
        return false;
    }
    edit::Document candidate = doc;
    if (!action (candidate, error))
        return false;
    LandscapeResult result;
    if (!RebuildLandscape (candidate, result, error, &doc) || !edit::Validate (candidate, error))
        return false;
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    return true;
}

bool Allocate (edit::Document& doc, Id& id, std::string& error)
{
    if (doc.nextId == 0 || doc.nextId == std::numeric_limits<Id>::max ()) {
        error = "landscape authored ID allocator exhausted";
        return false;
    }
    id = edit::NewId (doc);
    return true;
}

} // namespace

bool AddVolumetricDetail (edit::Document& doc, VolumetricDetail detail, Id& id, std::string& error)
{
    Id allocated = 0;
    const bool ok = Mutate (
        doc,
        [&] (edit::Document& d, std::string& e) {
            if (detail.id != kNoId) {
                e = "new volumetric detail must not supply an authored ID";
                return false;
            }
            for (const auto& entry : detail.entries) {
                const Id supplied = std::get_if<VolumetricOperation> (&entry)
                                        ? std::get<VolumetricOperation> (entry).id
                                        : std::get<VolumetricGroup> (entry).id;
                if (supplied != kNoId) {
                    e = "new volumetric detail must not supply stack-entry IDs";
                    return false;
                }
            }
            if (!Allocate (d, allocated, e))
                return false;
            detail.id = allocated;
            detail.revision = 1;
            for (auto& entry : detail.entries) {
                Id entryId = 0;
                if (!Allocate (d, entryId, e))
                    return false;
                if (auto* operation = std::get_if<VolumetricOperation> (&entry))
                    operation->id = entryId;
                else
                    std::get<VolumetricGroup> (entry).id = entryId;
            }
            d.landscape->volumetricDetails.push_back (std::move (detail));
            return true;
        },
        error);
    if (ok)
        id = allocated;
    return ok;
}

bool SetVolumetricDetail (edit::Document& doc, VolumetricDetail detail, std::string& error)
{
    if (doc.landscape) {
        const auto found = std::find_if (doc.landscape->volumetricDetails.begin (), doc.landscape->volumetricDetails.end (),
                                         [&] (const VolumetricDetail& current) { return current.id == detail.id; });
        if (found != doc.landscape->volumetricDetails.end () && detail == *found) {
            error.clear ();
            return true;
        }
    }
    return Mutate (
        doc,
        [&] (edit::Document& d, std::string& e) {
            auto found = std::find_if (d.landscape->volumetricDetails.begin (), d.landscape->volumetricDetails.end (),
                                       [&] (const VolumetricDetail& current) { return current.id == detail.id; });
            if (found == d.landscape->volumetricDetails.end ()) {
                e = "volumetric detail does not exist";
                return false;
            }
            if (detail.revision != found->revision) {
                e = "volumetric detail revision is stale";
                return false;
            }
            if (detail == *found)
                return true;
            std::set<Id> retainedIds;
            for (const auto& entry : found->entries)
                retainedIds.insert (std::get_if<VolumetricOperation> (&entry)
                                        ? std::get<VolumetricOperation> (entry).id
                                        : std::get<VolumetricGroup> (entry).id);
            for (auto& entry : detail.entries) {
                auto* operation = std::get_if<VolumetricOperation> (&entry);
                Id& entryId = operation ? operation->id : std::get<VolumetricGroup> (entry).id;
                if (entryId == kNoId) {
                    if (!Allocate (d, entryId, e))
                        return false;
                }
                else if (!retainedIds.count (entryId)) {
                    e = "edited volumetric stack entries must retain their IDs or use zero for a new entry";
                    return false;
                }
            }
            if (found->revision == std::numeric_limits<uint64_t>::max ()) {
                e = "volumetric detail revision is exhausted";
                return false;
            }
            detail.revision = found->revision + 1;
            *found = std::move (detail);
            return true;
        },
        error);
}

bool RemoveVolumetricDetail (edit::Document& doc, Id id, std::string& error)
{
    return Mutate (
        doc,
        [=] (edit::Document& d, std::string& e) {
            auto& details = d.landscape->volumetricDetails;
            const auto found = std::find_if (details.begin (), details.end (),
                                             [=] (const VolumetricDetail& detail) { return detail.id == id; });
            if (found == details.end ()) {
                e = "volumetric detail does not exist";
                return false;
            }
            details.erase (found);
            return true;
        },
        error);
}

} // namespace tapioca::geometry
