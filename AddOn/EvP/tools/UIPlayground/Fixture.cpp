#include "Fixture.hpp"
#include <algorithm>
#include <cmath>

namespace tapioca::playground {
const Value& Member (const Value& value, const char* key)
{
    static const Value empty;
    const auto* member = value.Find (key);
    return member ? *member : empty;
}
std::string Text (const Value& value, const char* key, const std::string& fallback)
{
    std::string result = fallback;
    Member (value, key).AsString (result);
    return result;
}
double Number (const Value& value, const char* key, double fallback)
{
    double result = fallback;
    Member (value, key).AsDouble (result);
    return std::isfinite (result) ? result : fallback;
}
bool Flag (const Value& value, const char* key, bool fallback)
{
    Member (value, key).AsBool (fallback);
    return fallback;
}
void Put (Value& value, const std::string& key, Value member)
{
    auto object = value.AsObject () ? *value.AsObject () : json::JsonObject {};
    object[key] = std::move (member);
    value = Value::Object (std::move (object));
}
Value EncodeProgramme (const av::floorprogramme::Programme& programme)
{
    json::JsonArray rows;
    for (const auto& row : programme.types)
        rows.push_back (Value::Object ({ { "rooms", Value::Double (row.rooms) },
                                         { "minM2", Value::Double (row.minM2) },
                                         { "maxM2", Value::Double (row.maxM2) },
                                         { "share", Value::Double (row.share) },
                                         { "rgba", Value::Integer (row.rgba) } }));
    return Value::Array (std::move (rows));
}
bool DecodeProgramme (const Value& value, av::floorprogramme::Programme& programme)
{
    const auto* rows = value.AsArray ();
    if (!rows || rows->size () > av::floorprogramme::kMaxTypes)
        return false;
    av::floorprogramme::Programme result;
    for (const auto& row : *rows) {
        double rooms, low, high, share;
        int64_t rgba = 0;
        if (!Member (row, "rooms").AsDouble (rooms) || !Member (row, "minM2").AsDouble (low) ||
            !Member (row, "maxM2").AsDouble (high) || !Member (row, "share").AsDouble (share) ||
            !std::isfinite (rooms) || !std::isfinite (low) || !std::isfinite (high) || !std::isfinite (share))
            return false;
        if (row.Find ("rgba") && (!Member (row, "rgba").AsInteger (rgba) || rgba < 0 || rgba > UINT32_MAX))
            return false;
        result.types.push_back ({ rooms, low, high, share, uint32_t (rgba) });
    }
    programme = std::move (result);
    return true;
}
bool DecodeFixture (const Value& value, FixtureModel& model, std::string& error)
{
    error = "Expected a v1 tapioca.uiplayground.fixture with a supported panel/status and typed data.";
    if (Text (value, "schema") != "tapioca.uiplayground.fixture" || Number (value, "version") != 1)
        return false;
    FixtureModel result;
    result.panel = Text (value, "panel");
    result.variant = Text (value, "variant");
    result.status = Text (Member (value, "status"), "kind");
    result.message = Text (Member (value, "status"), "message");
    if (result.variant.empty () || (result.status != "ready" && result.status != "computing" &&
                                    result.status != "error" && result.status != "completed"))
        return false;
    const auto& data = Member (value, "data");
    if (result.panel == "hudprogramme") {
        if (!DecodeProgramme (Member (data, "types"), result.programme))
            return false;
        result.saved = result.programme;
        if (data.Find ("saved") && !DecodeProgramme (Member (data, "saved"), result.saved))
            return false;
    }
    else if (result.panel == "hudmassingstats") {
        const auto* rows = Member (data, "rows").AsArray ();
        if (!rows || rows->size () > 1000)
            return false;
        for (const auto& row : *rows) {
            av::massingslices::Row item;
            item.guid = Text (row, "guid");
            item.function = Text (row, "function");
            if (item.guid.empty () || item.function.empty ())
                return false;
            for (const auto* key : { "rawArea", "allowedArea", "rawVolume", "allowedVolume", "floorHeight" }) {
                double number = 0;
                if (!Member (row, key).AsDouble (number) || !std::isfinite (number) || number < 0)
                    return false;
            }
            item.rawArea = Number (row, "rawArea");
            item.allowedArea = Number (row, "allowedArea");
            item.rawVolume = Number (row, "rawVolume");
            item.allowedVolume = Number (row, "allowedVolume");
            item.floorHeight = Number (row, "floorHeight");
            const double rgba = Number (row, "rgba", item.rgba);
            if (rgba < 0 || rgba > UINT32_MAX || std::floor (rgba) != rgba)
                return false;
            item.rgba = uint32_t (rgba);
            result.stats.rows.push_back (std::move (item));
        }
        result.stats.clipped = Flag (data, "clipped");
        result.stats.hasCoverage = Flag (data, "hasCoverage");
        result.stats.builtArea = Number (data, "builtArea");
        result.stats.unbuiltArea = Number (data, "unbuiltArea");
        result.stats.parcelArea = Number (data, "parcelArea");
        const auto& c = Member (data, "coefficients");
        result.coefficients.grossFactor = Number (c, "grossFactor", result.coefficients.grossFactor);
        result.coefficients.sellableFactor = Number (c, "sellableFactor", result.coefficients.sellableFactor);
        result.coefficients.unitGrossArea = Number (c, "unitGrossArea", result.coefficients.unitGrossArea);
        result.coefficients.parkingAreaPerUnit =
            Number (c, "parkingAreaPerUnit", result.coefficients.parkingAreaPerUnit);
        if (!av::massingareas::Valid (result.coefficients))
            return false;
    }
    else
        return false;
    model = std::move (result);
    error.clear ();
    return true;
}
Value EncodeFixture (const FixtureModel& model)
{
    Value data;
    if (model.panel == "hudprogramme")
        data = Value::Object (
            { { "types", EncodeProgramme (model.programme) }, { "saved", EncodeProgramme (model.saved) } });
    else {
        json::JsonArray rows;
        for (const auto& row : model.stats.rows)
            rows.push_back (Value::Object ({ { "guid", Value::String (row.guid) },
                                             { "function", Value::String (row.function) },
                                             { "rawArea", Value::Double (row.rawArea) },
                                             { "allowedArea", Value::Double (row.allowedArea) },
                                             { "rawVolume", Value::Double (row.rawVolume) },
                                             { "allowedVolume", Value::Double (row.allowedVolume) },
                                             { "floorHeight", Value::Double (row.floorHeight) },
                                             { "rgba", Value::Integer (row.rgba) } }));
        const auto& c = model.coefficients;
        data = Value::Object (
            { { "rows", Value::Array (std::move (rows)) },
              { "clipped", Value::Bool (model.stats.clipped) },
              { "hasCoverage", Value::Bool (model.stats.hasCoverage) },
              { "builtArea", Value::Double (model.stats.builtArea) },
              { "unbuiltArea", Value::Double (model.stats.unbuiltArea) },
              { "parcelArea", Value::Double (model.stats.parcelArea) },
              { "coefficients", Value::Object ({ { "grossFactor", Value::Double (c.grossFactor) },
                                                 { "sellableFactor", Value::Double (c.sellableFactor) },
                                                 { "unitGrossArea", Value::Double (c.unitGrossArea) },
                                                 { "parkingAreaPerUnit", Value::Double (c.parkingAreaPerUnit) } }) } });
    }
    return Value::Object ({ { "schema", Value::String ("tapioca.uiplayground.fixture") },
                            { "version", Value::Integer (1) },
                            { "panel", Value::String (model.panel) },
                            { "variant", Value::String (model.variant) },
                            { "status", Value::Object ({ { "kind", Value::String (model.status) },
                                                         { "message", Value::String (model.message) } }) },
                            { "data", std::move (data) } });
}
void Event (Value& document, const std::string& text)
{
    auto events = Member (document, "events").AsArray () ? *Member (document, "events").AsArray () : json::JsonArray {};
    events.push_back (Value::String (text));
    if (events.size () > 200)
        events.erase (events.begin (), events.begin () + (events.size () - 200));
    Put (document, "events", Value::Array (std::move (events)));
}
} // namespace tapioca::playground
