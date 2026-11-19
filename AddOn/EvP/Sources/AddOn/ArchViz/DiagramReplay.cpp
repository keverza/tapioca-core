#include "ArchViz/DiagramReplay.hpp"
#include "NodeGraph/Json.hpp"
#include <algorithm>
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

V WriteSpaces (const std::vector<PlannedSpace>& spaces)
{
    js::JsonArray out;
    for (const auto& space : spaces) {
        js::JsonArray rings;
        for (const auto& ring : space.rings) {
            js::JsonArray xy;
            xy.reserve (ring.size () * 2);
            for (const auto& p : ring) {
                xy.push_back (Number (p[0]));
                xy.push_back (Number (p[1]));
            }
            rings.push_back (V::Array (std::move (xy)));
        }
        js::JsonObject o { { "id", Unsigned (space.id) },
                           { "rgba", V::Integer (static_cast<int64_t> (space.rgba)) },
                           { "rings", V::Array (std::move (rings)) } };
        if (!space.label.empty ())
            o["label"] = V::String (space.label);
        if (space.area > 0)
            o["area"] = Number (space.area);
        out.push_back (V::Object (std::move (o)));
    }
    return V::Array (std::move (out));
}
V WritePlans (const FloorPlans& plans)
{
    js::JsonArray buildings;
    for (const auto& building : plans.buildings) {
        js::JsonArray floors;
        for (const auto& floor : building.floors)
            floors.push_back (V::Object ({ { "story", V::Integer (floor.story) },
                                           { "z", Number (floor.z) },
                                           { "height", Number (floor.height) },
                                           { "units", WriteSpaces (floor.units) },
                                           { "rooms", WriteSpaces (floor.rooms) } }));
        buildings.push_back (
            V::Object ({ { "key", V::String (building.key) }, { "floors", V::Array (std::move (floors)) } }));
    }
    return V::Object ({ { "buildings", V::Array (std::move (buildings)) } });
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
bool ReadSpaces (const V* value, std::vector<PlannedSpace>& out)
{
    const auto* list = value ? value->AsArray () : nullptr;
    if (!list)
        return false;
    for (const auto& item : *list) {
        PlannedSpace space;
        int64_t id = 0, rgba = 0;
        const V* i = item.Find ("id");
        const V* c = item.Find ("rgba");
        const V* r = item.Find ("rings");
        const auto* rings = r ? r->AsArray () : nullptr;
        if (!i || !i->AsInteger (id) || id < 0 || !c || !c->AsInteger (rgba) || !rings)
            return false;
        space.id = static_cast<uint64_t> (id);
        space.rgba = static_cast<uint32_t> (rgba);
        ReadString (item, "label", space.label); // optional
        if (item.Find ("area") && !ReadDouble (item, "area", space.area))
            return false;
        for (const auto& ring : *rings) {
            const auto* xy = ring.AsArray ();
            if (!xy || xy->size () % 2 != 0)
                return false;
            PlanRing points;
            for (size_t k = 0; k + 1 < xy->size (); k += 2) {
                double x = 0, y = 0;
                if (!(*xy)[k].AsDouble (x) || !(*xy)[k + 1].AsDouble (y) || !std::isfinite (x) || !std::isfinite (y))
                    return false;
                points.push_back ({ x, y });
            }
            space.rings.push_back (std::move (points));
        }
        out.push_back (std::move (space));
    }
    return true;
}
bool ReadPlans (const V& value, FloorPlans& out, std::string& error)
{
    const V* b = value.Find ("buildings");
    const auto* buildings = b ? b->AsArray () : nullptr;
    if (!buildings) {
        error = "The capture's plans lack buildings.";
        return false;
    }
    for (const auto& item : *buildings) {
        PlannedBuilding building;
        const V* f = item.Find ("floors");
        const auto* floors = f ? f->AsArray () : nullptr;
        if (!ReadString (item, "key", building.key) || !floors) {
            error = "A capture plan building is malformed.";
            return false;
        }
        for (const auto& entry : *floors) {
            PlannedFloor floor;
            int64_t story = 0;
            const V* s = entry.Find ("story");
            if (!s || !s->AsInteger (story) || !ReadDouble (entry, "z", floor.z) ||
                !ReadDouble (entry, "height", floor.height) || !ReadSpaces (entry.Find ("units"), floor.units) ||
                !ReadSpaces (entry.Find ("rooms"), floor.rooms)) {
                error = "A capture plan floor of " + building.key + " is malformed.";
                return false;
            }
            floor.story = static_cast<int> (story);
            building.floors.push_back (std::move (floor));
        }
        out.buildings.push_back (std::move (building));
    }
    std::sort (out.buildings.begin (), out.buildings.end (),
               [] (const PlannedBuilding& a, const PlannedBuilding& b) { return a.key < b.key; });
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
    if (snapshot.plans)
        o["plans"] = WritePlans (*snapshot.plans);
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
    if (const V* plans = parsed.value.Find ("plans")) {
        auto read = std::make_shared<FloorPlans> ();
        if (!ReadPlans (*plans, *read, error))
            return false;
        read->revision = out.revisions.topology;
        out.plans = std::move (read);
    }
    return true;
}

bool ReplayCapture (const std::string& text, DiagramInputSnapshot& out, std::string& error)
{
    CaptureInputs inputs;
    if (!ReadCapture (text, inputs, error))
        return false;
    out = Capture (std::move (inputs.slices), std::move (inputs.storeys), inputs.revisions, std::move (inputs.plans));
    return true;
}
} // namespace geomsrv::archviz::diagram
