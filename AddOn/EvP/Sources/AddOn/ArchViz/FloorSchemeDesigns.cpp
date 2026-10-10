#include "ArchViz/FloorSchemeDesigns.hpp"
#include "NodeGraph/Json.hpp"
#include <cmath>

namespace geomsrv::archviz::floorscheme::edit {
namespace {
namespace js = evp::nodegraph::json;
using V = js::JsonValue;
constexpr int64_t kVersion = 1;

V Number (double value)
{
    return V::Double (std::isfinite (value) ? std::round (value * 1000) / 1000 : 0.0); // to the millimetre
}
V Point (Vec p)
{
    return V::Array ({ Number (p.x), Number (p.y) });
}
V Rings (const std::vector<Ring>& rings)
{
    js::JsonArray out;
    for (const auto& ring : rings) {
        js::JsonArray points;
        for (const auto& p : ring)
            points.push_back (Point (p));
        out.push_back (V::Array (std::move (points)));
    }
    return V::Array (std::move (out));
}
template <typename T, typename F> V List (const std::vector<T>& items, F each)
{
    js::JsonArray out;
    for (const auto& item : items)
        out.push_back (each (item));
    return V::Array (std::move (out));
}
V Write (const Design& d)
{
    js::JsonObject o;
    if (!d.added.empty ())
        o["added"] = Rings (d.added);
    if (!d.cut.empty ())
        o["cut"] = Rings (d.cut);
    if (d.shallow != Access::Auto)
        o["shallow"] = V::Integer (static_cast<int64_t> (d.shallow));
    if (!d.locked.empty ())
        o["locked"] = List (d.locked, Point);
    const auto& p = d.pins;
    if (!p.cores.empty ())
        o["cores"] = List (p.cores, [] (const Pins::Core& c) {
            return V::Array ({ Number (c.centre.x), Number (c.centre.y), Number (c.width), Number (c.depth) });
        });
    if (!p.access.empty ())
        o["access"] = List (p.access, [] (const Pins::AccessAt& a) {
            return V::Array ({ Number (a.at.x), Number (a.at.y), V::Integer (static_cast<int64_t> (a.access)) });
        });
    if (!p.walls.empty ())
        o["walls"] = List (p.walls, [] (const Pins::Wall& w) { return Point (w.at); });
    if (!p.rooms.empty ())
        o["rooms"] = List (p.rooms, [] (const Pins::Rooms& r) {
            return V::Array ({ Number (r.at.x), Number (r.at.y), Number (r.rooms) });
        });
    if (!p.ends.empty ())
        o["ends"] = List (p.ends, [] (const Pins::End& e) { return Point (e.at); });
    if (!p.counts.empty ())
        o["counts"] = List (p.counts, [] (const Pins::Count& c) {
            return V::Array ({ Number (c.at.x), Number (c.at.y), V::Integer (c.flats) });
        });
    return V::Object (std::move (o));
}

// Readers: false on anything that is not what it should be.
bool Numbers (const V& value, size_t count, double* out)
{
    const auto* array = value.AsArray ();
    if (!array || array->size () != count)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (!(*array)[i].AsDouble (out[i]) || !std::isfinite (out[i]) || std::abs (out[i]) > 1e9)
            return false;
    return true;
}
bool ReadPoint (const V& value, Vec& p)
{
    double xy[2];
    if (!Numbers (value, 2, xy))
        return false;
    p = { xy[0], xy[1] };
    return true;
}
bool ReadRings (const V* value, std::vector<Ring>& rings)
{
    if (!value)
        return true;
    const auto* array = value->AsArray ();
    if (!array)
        return false;
    for (const auto& item : *array) {
        const auto* points = item.AsArray ();
        if (!points || points->size () < 3)
            return false;
        Ring ring;
        for (const auto& point : *points) {
            Vec p;
            if (!ReadPoint (point, p))
                return false;
            ring.push_back (p);
        }
        rings.push_back (std::move (ring));
    }
    return true;
}
template <typename T, typename F> bool ReadList (const V* value, std::vector<T>& out, F each)
{
    if (!value)
        return true;
    const auto* array = value->AsArray ();
    if (!array)
        return false;
    for (const auto& item : *array) {
        T t {};
        if (!each (item, t))
            return false;
        out.push_back (std::move (t));
    }
    return true;
}
bool ReadAccess (int64_t value, Access& access)
{
    if (value < 0 || value > static_cast<int64_t> (Access::Rows))
        return false;
    access = static_cast<Access> (value);
    return true;
}
bool Read (const V& value, Design& d)
{
    if (!value.AsObject ())
        return false;
    if (!ReadRings (value.Find ("added"), d.added) || !ReadRings (value.Find ("cut"), d.cut))
        return false;
    if (const auto* shallow = value.Find ("shallow")) {
        int64_t n = 0;
        if (!shallow->AsInteger (n) || !ReadAccess (n, d.shallow))
            return false;
    }
    auto& p = d.pins;
    return ReadList (value.Find ("locked"), d.locked, ReadPoint) &&
           ReadList (value.Find ("cores"), p.cores,
                     [] (const V& item, Pins::Core& c) {
                         double n[4];
                         if (!Numbers (item, 4, n) || n[2] < 0 || n[3] < 0 || n[2] > 12 || n[3] > 12)
                             return false;
                         c = { { n[0], n[1] }, n[2], n[3] };
                         return true;
                     }) &&
           ReadList (value.Find ("access"), p.access,
                     [] (const V& item, Pins::AccessAt& a) {
                         const auto* array = item.AsArray ();
                         int64_t kind = 0;
                         return array && array->size () == 3 && (*array)[0].AsDouble (a.at.x) &&
                                (*array)[1].AsDouble (a.at.y) && (*array)[2].AsInteger (kind) &&
                                ReadAccess (kind, a.access);
                     }) &&
           ReadList (value.Find ("walls"), p.walls,
                     [] (const V& item, Pins::Wall& w) { return ReadPoint (item, w.at); }) &&
           ReadList (value.Find ("rooms"), p.rooms,
                     [] (const V& item, Pins::Rooms& r) {
                         double n[3];
                         if (!Numbers (item, 3, n) || n[2] < 0.5 || n[2] > 8)
                             return false;
                         r = { { n[0], n[1] }, n[2] };
                         return true;
                     }) &&
           ReadList (value.Find ("ends"), p.ends,
                     [] (const V& item, Pins::End& e) { return ReadPoint (item, e.at); }) &&
           ReadList (value.Find ("counts"), p.counts, [] (const V& item, Pins::Count& c) {
               const auto* array = item.AsArray ();
               int64_t flats = 0;
               if (!array || array->size () != 3 || !(*array)[0].AsDouble (c.at.x) || !(*array)[1].AsDouble (c.at.y) ||
                   !(*array)[2].AsInteger (flats) || flats < 1 || flats > 64)
                   return false;
               c.flats = static_cast<int> (flats);
               return true;
           });
}
} // namespace

std::string ToJson (const Designs& designs)
{
    if (designs.Empty ())
        return {};
    js::JsonArray floors, unique;
    for (const auto& [story, design] : designs.floors)
        floors.push_back (V::Object ({ { "story", V::Integer (story) }, { "design", Write (design) } }));
    for (int story : designs.unique)
        unique.push_back (V::Integer (story));
    return js::Write (V::Object ({ { "version", V::Integer (kVersion) },
                                   { "floors", V::Array (std::move (floors)) },
                                   { "unique", V::Array (std::move (unique)) } }),
                      0);
}

bool FromJson (const std::string& text, Designs& designs, std::string& error)
{
    if (text.empty ()) {
        designs = {};
        return true;
    }
    if (text.size () > kMaxDesignsText) {
        error = "Floor designs exceed their size budget.";
        return false;
    }
    const auto parsed = js::Parse (text);
    int64_t version = 0;
    const V* v = parsed.ok ? parsed.value.Find ("version") : nullptr;
    if (!parsed.ok || !v || !v->AsInteger (version) || version != kVersion) {
        error = parsed.ok ? "Floor designs are not a version this add-on reads."
                          : "Floor designs are not JSON: " + parsed.error;
        return false;
    }
    Designs out;
    const auto* floors = parsed.value.Find ("floors");
    const auto* list = floors ? floors->AsArray () : nullptr;
    if (!list) {
        error = "Floor designs have no floors.";
        return false;
    }
    for (const auto& item : *list) {
        int64_t story = 0;
        const auto* s = item.Find ("story");
        const auto* d = item.Find ("design");
        Design design;
        if (!s || !s->AsInteger (story) || !d || !Read (*d, design)) {
            error = "A floor design is malformed.";
            return false;
        }
        out.floors[static_cast<int> (story)] = std::move (design);
    }
    if (const auto* unique = parsed.value.Find ("unique")) {
        const auto* stories = unique->AsArray ();
        if (!stories) {
            error = "Unique floors are malformed.";
            return false;
        }
        for (const auto& item : *stories) {
            int64_t story = 0;
            if (!item.AsInteger (story)) {
                error = "Unique floors are malformed.";
                return false;
            }
            out.unique.insert (static_cast<int> (story));
        }
    }
    designs = std::move (out);
    return true;
}
} // namespace geomsrv::archviz::floorscheme::edit
