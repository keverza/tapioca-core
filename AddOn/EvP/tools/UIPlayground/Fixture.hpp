#pragma once
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/FloorProgramme.hpp"
#include "NodeGraph/Json.hpp"

namespace tapioca::playground {
namespace json = evp::nodegraph::json;
using Value = json::JsonValue;
namespace av = geomsrv::archviz;

const Value& Member (const Value& value, const char* key);
std::string Text (const Value& value, const char* key, const std::string& fallback = {});
double Number (const Value& value, const char* key, double fallback = 0);
bool Flag (const Value& value, const char* key, bool fallback = false);
void Put (Value& value, const std::string& key, Value member);

struct FixtureModel {
    std::string panel, variant, status, message;
    av::massingslices::Result stats;
    av::floorprogramme::Programme programme, saved;
    av::massingareas::Coefficients coefficients;
    // "hudeditor": a virtual editor debug dump, kept as written (HudVirtualEditorDebug.cpp). Its
    // `playground` member, when present, names the variant and what the session starts with.
    Value editor;
};
// Malformed transport is rejected; deliberately invalid domain values remain visible
// for the real production validator. Stats fixtures never contain geometry.
bool DecodeFixture (const Value& value, FixtureModel& model, std::string& error);
Value EncodeFixture (const FixtureModel& model);
Value EncodeProgramme (const av::floorprogramme::Programme& programme);
bool DecodeProgramme (const Value& value, av::floorprogramme::Programme& programme);
void Event (Value& document, const std::string& text);
// A virtual editor debug dump (Export debug scene, or a fixture written in its format) as a
// "hudeditor" fixture: its variant from `playground.variant`, else `fallbackVariant`; its status an
// error when the dump recorded a terrain refusal.
bool IsEditorDump (const Value& value);
Value FixtureFromDump (const Value& dump, const std::string& fallbackVariant);
} // namespace tapioca::playground
