#include "ArchViz/DiagramReplay.hpp"
#include "NodeGraph/Json.hpp"
#include <cmath>

namespace geomsrv::archviz::diagram {
namespace {
namespace js = evp::nodegraph::json;
using V = js::JsonValue;

V Number (double value)
{
    return V::Double (std::isfinite (value) ? value : 0.0);
}
V Unsigned (uint64_t value)
{
    // JSON integers are int64 here; stamps above 2^63 never occur, but must not wrap silently.
    return V::Integer (static_cast<int64_t> (value & 0x7FFFFFFFFFFFFFFFull));
}
V Chains (const std::vector<SliceChain>& chains)
{
    js::JsonArray out;
    for (const auto& chain : chains) {
        js::JsonArray xy;
        xy.reserve (chain.xy.size ());
        for (double v : chain.xy)
            xy.push_back (Number (v));
        out.push_back (V::Object ({ { "closed", V::Bool (chain.closed) }, { "xy", V::Array (std::move (xy)) } }));
    }
    return V::Array (std::move (out));
}
V WriteResult (const massingslices::Result& result, bool nested)
{
    js::JsonArray rows, records;
    for (const auto& row : result.rows) {
        js::JsonObject o;
        o["guid"] = V::String (row.guid);
        o["function"] = V::String (row.function);
        o["story"] = V::Integer (row.story);
        o["z"] = Number (row.z);
        o["floorHeight"] = Number (row.floorHeight);
        o["rgba"] = V::Integer (static_cast<int64_t> (row.rgba));
        o["rawArea"] = Number (row.rawArea);
        o["allowedArea"] = Number (row.allowedArea);
        o["chains"] = Chains (row.chains);
        if (!row.rawChains.empty ())
            o["rawChains"] = Chains (row.rawChains);
        rows.push_back (V::Object (std::move (o)));
    }
    for (const auto& surface : result.buildingSurfaces)
        records.push_back (
            V::Object ({ { "guid", V::String (surface.record.guid) }, { "id", V::String (surface.record.id) } }));
    js::JsonObject o;
    o["complete"] = V::Bool (result.complete);
    o["followsStories"] = V::Bool (result.followsStories);
    o["rows"] = V::Array (std::move (rows));
    o["records"] = V::Array (std::move (records));
    if (!result.note.empty ())
        o["note"] = V::String (result.note);
    // One level only: a story cut never carries a cut of its own.
    if (!nested && result.stories)
        o["stories"] = WriteResult (*result.stories, true);
    return V::Object (std::move (o));
}

bool ReadDouble (const V& object, const char* key, double& out)
{
    const V* v = object.Find (key);
    return v && v->AsDouble (out) && std::isfinite (out);
}
bool ReadString (const V& object, const char* key, std::string& out)
{
    const V* v = object.Find (key);
    return v && v->AsString (out);
}
bool ReadChains (const V* value, std::vector<SliceChain>& out)
{
    if (!value)
        return true; // optional
    const auto* list = value->AsArray ();
    if (!list)
        return false;
    for (const auto& item : *list) {
        SliceChain chain;
        const V* closed = item.Find ("closed");
        const V* xy = item.Find ("xy");
        const auto* coords = xy ? xy->AsArray () : nullptr;
        if (!closed || !closed->AsBool (chain.closed) || !coords || coords->size () % 2 != 0)
            return false;
        chain.xy.reserve (coords->size ());
        for (const auto& c : *coords) {
            double v = 0;
            if (!c.AsDouble (v) || !std::isfinite (v))
                return false;
            chain.xy.push_back (v);
        }
        out.push_back (std::move (chain));
    }
    return true;
}
bool ReadResult (const V& value, massingslices::Result& out, bool nested, std::string& error)
{
    const V* complete = value.Find ("complete");
    const V* follows = value.Find ("followsStories");
    const auto* rows = value.Find ("rows") ? value.Find ("rows")->AsArray () : nullptr;
    const auto* records = value.Find ("records") ? value.Find ("records")->AsArray () : nullptr;
    if (!complete || !complete->AsBool (out.complete) || !follows || !follows->AsBool (out.followsStories) || !rows ||
        !records) {
        error = "A capture result lacks complete, followsStories, rows or records.";
        return false;
    }
    for (size_t i = 0; i < rows->size (); ++i) {
        const V& item = (*rows)[i];
        massingslices::Row row;
        int64_t story = 0, rgba = 0;
        const V* s = item.Find ("story");
        const V* c = item.Find ("rgba");
        if (!ReadString (item, "guid", row.guid) || !ReadString (item, "function", row.function) || !s ||
            !s->AsInteger (story) || !ReadDouble (item, "z", row.z) ||
            !ReadDouble (item, "floorHeight", row.floorHeight) || !c || !c->AsInteger (rgba) ||
            !ReadChains (item.Find ("chains"), row.chains) || !ReadChains (item.Find ("rawChains"), row.rawChains)) {
            error = "Capture row " + std::to_string (i) + " is malformed.";
            return false;
        }
        // Optional: the inspector's areas (absent in hand-written fixtures, which read 0).
        if (item.Find ("rawArea") && !ReadDouble (item, "rawArea", row.rawArea)) {
            error = "Capture row " + std::to_string (i) + " has a malformed rawArea.";
            return false;
        }
        if (item.Find ("allowedArea") && !ReadDouble (item, "allowedArea", row.allowedArea)) {
            error = "Capture row " + std::to_string (i) + " has a malformed allowedArea.";
            return false;
        }
        row.story = static_cast<int> (story);
        row.rgba = static_cast<uint32_t> (rgba);
        out.rows.push_back (std::move (row));
    }
    for (const auto& item : *records) {
        massingbuildings::Surface surface;
        if (!ReadString (item, "guid", surface.record.guid) || !ReadString (item, "id", surface.record.id)) {
            error = "A capture building record is malformed.";
            return false;
        }
        out.buildingSurfaces.push_back (std::move (surface));
    }
    ReadString (value, "note", out.note);
    if (const V* stories = value.Find ("stories")) {
        if (nested) {
            error = "A capture story cut carries a story cut of its own.";
            return false;
        }
        auto cut = std::make_shared<massingslices::Result> ();
        if (!ReadResult (*stories, *cut, true, error))
            return false;
        out.stories = std::move (cut);
    }
    return true;
}
template <typename T, typename F> bool ReadList (const V& object, const char* key, std::vector<T>& out, F read)
{
    const V* v = object.Find (key);
    const auto* list = v ? v->AsArray () : nullptr;
    if (!list)
        return false;
    for (const auto& item : *list) {
        T value {};
        if (!read (item, value))
            return false;
        out.push_back (std::move (value));
    }
    return true;
}
} // namespace

std::string WriteCapture (const DiagramInputSnapshot& snapshot, const std::string& note)
{
    js::JsonArray levels, names, indices, floorIds;
    for (double level : snapshot.storeys.levels)
        levels.push_back (Number (level));
    for (const auto& name : snapshot.storeys.names)
        names.push_back (V::String (name));
    for (int index : snapshot.storeys.indices)
        indices.push_back (V::Integer (index));
    for (int id : snapshot.storeys.floorIds)
        floorIds.push_back (V::Integer (id));
    const RevisionSet& r = snapshot.revisions;
    js::JsonObject o;
    o["format"] = V::String (kCaptureFormat);
    o["version"] = V::Integer (kCaptureVersion);
    if (!note.empty ())
        o["note"] = V::String (note);
    o["storeys"] = V::Object ({ { "levels", V::Array (std::move (levels)) },
                                { "names", V::Array (std::move (names)) },
                                { "indices", V::Array (std::move (indices)) },
                                { "floorIds", V::Array (std::move (floorIds)) } });
    o["revisions"] = V::Object ({ { "projectEpoch", Unsigned (r.projectEpoch) },
                                  { "modelStamp", Unsigned (r.modelStamp) },
                                  { "slices", Unsigned (r.slices) },
                                  { "topology", Unsigned (r.topology) },
                                  { "metadata", Unsigned (r.metadata) },
                                  { "metrics", Unsigned (r.metrics) } });
    if (snapshot.slices)
        o["slices"] = WriteResult (*snapshot.slices, false);
    return js::Write (V::Object (std::move (o)), 1);
}

std::string ContentSignature (const DiagramInputSnapshot& snapshot)
{
    DiagramInputSnapshot content = snapshot; // shared rows; only the stamps are reset
    content.revisions = {};
    return WriteCapture (content);
}

bool ReadCapture (const std::string& text, CaptureInputs& out, std::string& error)
{
    out = {};
    if (text.size () > kMaxCaptureText) {
        error = "The diagram capture exceeds its size budget.";
        return false;
    }
    const auto parsed = js::Parse (text);
    if (!parsed.ok) {
        error = "The diagram capture is not JSON: " + parsed.error;
        return false;
    }
    std::string format;
    int64_t version = 0;
    const V* v = parsed.value.Find ("version");
    if (!ReadString (parsed.value, "format", format) || format != kCaptureFormat || !v || !v->AsInteger (version) ||
        version != kCaptureVersion) {
        error = "Not a diagram capture this build reads (format " + std::string (kCaptureFormat) + " version " +
                std::to_string (kCaptureVersion) + ").";
        return false;
    }
    ReadString (parsed.value, "note", out.note);
    const V* storeys = parsed.value.Find ("storeys");
    if (!storeys ||
        !ReadList (*storeys, "levels", out.storeys.levels,
                   [] (const V& item, double& d) { return item.AsDouble (d) && std::isfinite (d); }) ||
        !ReadList (*storeys, "names", out.storeys.names,
                   [] (const V& item, std::string& s) { return item.AsString (s); }) ||
        !ReadList (*storeys, "indices", out.storeys.indices,
                   [] (const V& item, int& i) {
                       int64_t n = 0;
                       return item.AsInteger (n) && (i = static_cast<int> (n), true);
                   }) ||
        !ReadList (*storeys, "floorIds", out.storeys.floorIds, [] (const V& item, int& i) {
            int64_t n = 0;
            return item.AsInteger (n) && (i = static_cast<int> (n), true);
        })) {
        error = "The capture's storeys are malformed.";
        return false;
    }
    if (out.storeys.levels.size () != out.storeys.indices.size ()) {
        error = "The capture's storey levels and indices differ in length.";
        return false;
    }
    if (const V* revisions = parsed.value.Find ("revisions")) {
        auto stamp = [&] (const char* key, uint64_t& into) {
            int64_t n = 0;
            if (const V* s = revisions->Find (key); s && s->AsInteger (n) && n >= 0)
                into = static_cast<uint64_t> (n);
        };
        stamp ("projectEpoch", out.revisions.projectEpoch);
        stamp ("modelStamp", out.revisions.modelStamp);
        stamp ("slices", out.revisions.slices);
        stamp ("topology", out.revisions.topology);
        stamp ("metadata", out.revisions.metadata);
        stamp ("metrics", out.revisions.metrics);
    }
    if (const V* slices = parsed.value.Find ("slices")) {
        auto result = std::make_shared<massingslices::Result> ();
        if (!ReadResult (*slices, *result, false, error))
            return false;
        out.slices = std::move (result);
    }
    return true;
}

bool ReplayCapture (const std::string& text, DiagramInputSnapshot& out, std::string& error)
{
    CaptureInputs inputs;
    if (!ReadCapture (text, inputs, error))
        return false;
    out = Capture (std::move (inputs.slices), std::move (inputs.storeys), inputs.revisions);
    return true;
}
} // namespace geomsrv::archviz::diagram
