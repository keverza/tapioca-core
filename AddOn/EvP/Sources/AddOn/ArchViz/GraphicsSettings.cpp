#include "ArchViz/GraphicsSettings.hpp"
#include "Metadata/TapiocaMetadata.hpp"
#include "NodeGraph/Json.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>

namespace geomsrv::archviz::graphicssettings {
namespace {
bool SceneKey (const std::string& key)
{
    return key.find ("ui.") != 0 && key.find ("diagram.") != 0;
}
Snapshot Defaults ()
{
    Snapshot result;
    const auto add = [&] (std::string key, std::string group, Kind kind, double value, double min, double max,
                          std::string units = {}) {
        result.definitions[key] = { key, std::move (group), std::move (units), kind, value, min, max };
        result.values[key] = { value, false };
    };
    for (const auto* category : { "envelope", "collapseZone", "parcel", "offset", "slices", "selectionHighlight",
                                  "functionHighlight", "coverageHighlight", "annotation", "otherOverlay" }) {
        const std::string c = category;
        add (c + ".surfaceOpacity", c, Kind::Number, c == "envelope" ? 0.25 : 0.35, 0, 1);
        add (c + ".surfaceOcclusionIntensity", c, Kind::Number, 0.3, 0, 1, "retained opacity behind buildings");
        add (c + ".surfaceColor", c, Kind::Colour, 0xDCF3FAFFu, 0, 4294967295.0);
        add (c + ".surfaceIsHatched", c, Kind::Boolean, 0, 0, 1);
        add (c + ".surfaceHatchDirection", c, Kind::Number, 45, 0, 180, "degrees in model XY");
        add (c + ".surfaceHatchDensity", c, Kind::Number, 1, 0.01, 100, "lines / model metre");
        add (c + ".lineThickness", c, Kind::Number, c == "collapseZone" ? 0.7 : 2, 0.1, 20, "logical px");
        add (c + ".lineColor", c, Kind::Colour, c == "offset" ? 0xA66226FFu : 0xAA4465FFu, 0, 4294967295.0);
        add (c + ".lineOpacity", c, Kind::Number, 1, 0, 1);
        add (c + ".lineDashLength", c, Kind::Number, 0.5, 0, 100, "model metres; 0 = solid");
        add (c + ".lineGapLength", c, Kind::Number, 0.3, 0.01, 100, "model metres");
        add (c + ".textSize", c, Kind::Number, 13, 4, 96, "logical px; scales planar text proportionally");
        add (c + ".textColor", c, Kind::Colour, 0x263029FFu, 0, 4294967295.0);
        add (c + ".textHalo", c, Kind::Number, 1, 0, 8, "logical px; 0 = off");
        add (c + ".textHaloColor", c, Kind::Colour, 0xFBFCFAFFu, 0, 4294967295.0);
        add (c + ".textHideDistance", c, Kind::Number, 8, 0, 96, "minimum projected px; planar text only");
        add (c + ".pointSize", c, Kind::Number, 6, 1, 40, "logical px; 3D scales proportionally");
        add (c + ".pointColor", c, Kind::Colour, 0x5A7F62FFu, 0, 4294967295.0);
    }
    for (const auto& swatch : Palette ())
        if (std::string (swatch.name).find ("diagram") == 0)
            add (std::string ("diagram.") + (std::string (swatch.name) == "diagram boundary" ? "propertyColor"
                                             : std::string (swatch.name) == "diagram offset" ? "offsetColor"
                                                                                             : "zeroOffsetColor"),
                 "Parcel diagram", Kind::Colour, swatch.rgba, 0, 4294967295.0);
    add ("diagram.propertyThickness", "Parcel diagram", Kind::Number, 2.5, 0.1, 12, "logical px");
    add ("diagram.offsetThickness", "Parcel diagram", Kind::Number, 2, 0.1, 12, "logical px");
    add ("diagram.propertyDashLength", "Parcel diagram", Kind::Number, 12, 1, 100, "px at 13 px font");
    add ("diagram.offsetDashLength", "Parcel diagram", Kind::Number, 5, 1, 100, "px at 13 px font");
    add ("diagram.zeroOffsetDepth", "Parcel diagram", Kind::Number, 18, 0, 60, "px at 13 px font");
    add ("diagram.height", "Parcel diagram", Kind::Number, 200, 80, 800, "logical px");
    add ("diagram.pointSize", "Parcel diagram", Kind::Number, 4, 1, 12, "radius px");
    add ("diagram.pointColor", "Parcel diagram", Kind::Colour, 0x2F6FEBFFu, 0, 4294967295.0);
    add ("diagram.pointDisabledColor", "Parcel diagram", Kind::Colour, 0xAAB4B9FFu, 0, 4294967295.0);
    add ("ui.panelWidth", "HUD layout", Kind::Number, 220, 180, 900, "logical px; viewer default 280");
    add ("ui.save.unsaved", "Save interaction", Kind::Colour, 0xE8A33DFFu, 0, 4294967295.0);
    add ("ui.save.saved", "Save interaction", Kind::Colour, 0x9AA0A6FFu, 0, 4294967295.0);
    add ("ui.save.hovered", "Save interaction", Kind::Colour, 0xF7BA4DFFu, 0, 4294967295.0);
    add ("ui.save.active", "Save interaction", Kind::Colour, 0xD68A21FFu, 0, 4294967295.0);
    add ("ui.save.text", "Save interaction", Kind::Colour, 0x1C1C1CFFu, 0, 4294967295.0);
    add ("ui.fontScale", "HUD layout", Kind::Number, 1, 0.8, 2, "multiplier over selected text-size step");
    for (const auto& enumeration : metadata::DefaultSchema ().enumerations)
        if (enumeration.id == "building-usage")
            for (const auto& option : enumeration.options)
                add ("function." + option.value, "Story slice functions", Kind::Colour, option.rgba, 0, 4294967295.0);
    add ("ui.tip.background", "Tooltips", Kind::Colour, 0xF9F9F9FAu, 0, 4294967295.0);
    add ("ui.tip.text", "Tooltips", Kind::Colour, 0x1B1B1BFFu, 0, 4294967295.0);
    add ("ui.tip.border", "Tooltips", Kind::Colour, 0x00000030u, 0, 4294967295.0);
    add ("ui.tip.rounding", "Tooltips", Kind::Number, 0.3, 0, 1, "font em");
    add ("ui.tip.shadow", "Tooltips", Kind::Boolean, 1, 0, 1);
    add ("ui.dock.background", "Dock interaction", Kind::Colour, 0xFAFAFAEEu, 0, 4294967295.0);
    add ("ui.dock.activeBackground", "Dock interaction", Kind::Colour, 0x5A7F62FFu, 0, 4294967295.0);
    add ("ui.dock.text", "Dock interaction", Kind::Colour, 0x1C211DFFu, 0, 4294967295.0);
    add ("ui.dock.hover", "Dock interaction", Kind::Colour, 0x5A7F624Du, 0, 4294967295.0);
    add ("ui.dock.pressed", "Dock interaction", Kind::Colour, 0x5A7F627Au, 0, 4294967295.0);
    add ("ui.dock.busy", "Dock interaction", Kind::Colour, 0xE8A33DFFu, 0, 4294967295.0);
    add ("ui.dock.error", "Dock interaction", Kind::Colour, 0xD64545FFu, 0, 4294967295.0);
    add ("ui.dock.circleRadius", "Dock interaction", Kind::Number, 0.22, 0.1, 0.4, "fraction of dock width");
    add ("ui.dock.rounding", "Dock interaction", Kind::Number, 6, 0, 12, "logical px");
    add ("ui.section.hover", "Building section", Kind::Colour, 0x5A7F6238u, 0, 4294967295.0);
    add ("ui.section.selection", "Building section", Kind::Colour, 0x5A7F62FFu, 0, 4294967295.0);
    add ("ui.section.selectionWidth", "Building section", Kind::Number, 2, 0.1, 8, "logical px");
    add ("ui.section.rounding", "Building section", Kind::Number, 2, 0, 8, "logical px");
    add ("ui.section.rowHeight", "Building section", Kind::Number, 1.2, 1, 3, "font em");
    add ("ui.section.rowGap", "Building section", Kind::Number, 1, 0.1, 12, "logical px");
    add ("ui.coverage.built", "Coverage diagram", Kind::Colour, 0x9AA0A6FFu, 0, 4294967295.0);
    add ("ui.coverage.unbuilt", "Coverage diagram", Kind::Colour, 0x66BB6AFFu, 0, 4294967295.0);
    return result;
}
std::mutex s_mutex;
std::atomic<std::shared_ptr<const Snapshot>> s_current { std::make_shared<const Snapshot> (Defaults ()) };

uint32_t Tint (const Snapshot& settings, const std::string& key, uint32_t colour)
{
    return uint32_t (Number (settings, key, colour));
}
uint32_t Alpha (uint32_t colour, double opacity)
{
    return (colour & 0xFFFFFF00u) | uint32_t (std::lround ((colour & 255u) * opacity));
}
std::string Category (const overlaylayers::Layer& layer)
{
    if (layer.graphicsCategory == "coverageHighlight" || layer.graphicsCategory == "functionHighlight")
        return layer.graphicsCategory;
    auto name = layer.name;
    for (auto& letter : name)
        if (letter >= 'A' && letter <= 'Z')
            letter = char (letter - 'A' + 'a');
    if (name == "tapioca.massing.envelope")
        return "envelope";
    if (name.find ("collapse") != std::string::npos)
        return "collapseZone";
    if (name.find ("coverage") != std::string::npos)
        return "coverageHighlight";
    if (name.find ("offsetdimensions") != std::string::npos || name.find ("watch") != std::string::npos ||
        name.find ("annotation") != std::string::npos)
        return "annotation";
    if (name.find ("offset") != std::string::npos)
        return "offset";
    if (name.find ("parcel") != std::string::npos || name.find ("property") != std::string::npos)
        return "parcel";
    if (name.find ("highlight") != std::string::npos || name.find ("selection") != std::string::npos)
        return "selectionHighlight";
    if (name == "tapioca.floorpicks")
        return "selectionHighlight";
    if (name == "tapioca.massing.functionvolumes")
        return "functionHighlight";
    if (name.find ("slice") != std::string::npos)
        return "slices";
    return "otherOverlay";
}
} // namespace

const std::vector<Swatch>& Palette ()
{
    // sRGB conversion of taste-variables.css OKLCH tokens; diagram colours are explicitly additional.
    static const std::vector<Swatch> palette = [] () {
        std::vector<Swatch> result = { { "background", 0xF9FCF9FFu },
                                       { "foreground", 0x1C211DFFu },
                                       { "surface", 0xF0F5F1FFu },
                                       { "border", 0xDAE0DBFFu },
                                       { "accent", 0x5A7F62FFu },
                                       { "accent-secondary", 0x839987FFu },
                                       { "accent-tertiary", 0x37523DFFu },
                                       { "muted", 0x626C64FFu },
                                       { "muted-foreground", 0x8A928BFFu },
                                       { "success", 0x4B8358FFu },
                                       { "warning", 0xB28324FFu },
                                       { "danger", 0xB84E45FFu },
                                       { "primary-foreground", 0xFCFCFCFFu },
                                       { "diagram boundary", 0xAA4465FFu },
                                       { "diagram offset", 0xA66226FFu },
                                       { "diagram zero offset", 0xFFC146FFu },
                                       { "envelope", 0xDCF3FAFFu } };
        for (const auto& enumeration : metadata::DefaultSchema ().enumerations)
            if (enumeration.id == "building-usage")
                for (const auto& option : enumeration.options)
                    result.push_back ({ "function: " + option.label, option.rgba });
        return result;
    }();
    return palette;
}
std::shared_ptr<const Snapshot> Current ()
{
    return s_current.load ();
}
void Register (const Definition& definition)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    auto old = Current ();
    if (old->definitions.count (definition.key))
        return;
    auto next = std::make_shared<Snapshot> (*old);
    next->definitions[definition.key] = definition;
    next->values[definition.key] = { definition.initial, false };
    ++next->revision;
    s_current.store (next);
}
bool Set (const std::string& key, double value)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    const auto old = Current ();
    const auto found = old->definitions.find (key);
    if (found == old->definitions.end () || !std::isfinite (value))
        return false;
    const auto& d = found->second;
    if (value < d.min || value > d.max || (d.kind != Kind::Number && std::floor (value) != value))
        return false;
    const auto& before = old->values.at (key);
    if (before.enabled && before.number == value)
        return false;
    auto next = std::make_shared<Snapshot> (*old);
    next->values[key] = { value, true };
    ++next->revision;
    if (SceneKey (key))
        ++next->sceneRevision;
    s_current.store (next);
    return true;
}
bool Reset (const std::string& key)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    auto next = std::make_shared<Snapshot> (*Current ());
    bool changed = false;
    bool sceneChanged = false;
    for (auto& [name, value] : next->values)
        if ((key.empty () || name == key) && value.enabled) {
            value = { next->definitions.at (name).initial, false };
            changed = true;
            sceneChanged = sceneChanged || SceneKey (name);
        }
    if (!changed)
        return false;
    ++next->revision;
    if (sceneChanged)
        ++next->sceneRevision;
    s_current.store (next);
    return true;
}
double Number (const Snapshot& settings, const std::string& key, double fallback)
{
    const auto value = settings.values.find (key);
    return value != settings.values.end () && value->second.enabled ? value->second.number : fallback;
}
double Number (const std::string& key, double fallback)
{
    return Number (*Current (), key, fallback);
}
uint32_t Colour (const std::string& key, uint32_t fallback)
{
    return uint32_t (Number (key, fallback));
}
uint32_t FunctionColour (const std::string& function, uint32_t fallback)
{
    const auto key = "function." + function;
    Register ({ key, "Story slice functions", "RGBA", Kind::Colour, double (fallback), 0, 4294967295.0 });
    return Colour (key, fallback);
}
bool Affects (const overlaylayers::Layer& layer, const Snapshot& settings)
{
    const auto prefix = Category (layer) + ".";
    for (const auto& [key, value] : settings.values)
        if (value.enabled &&
            (key.find (prefix) == 0 ||
             (layer.name == "tapioca.massing.lines" && (key.find ("parcel.") == 0 || key.find ("offset.") == 0)) ||
             ((prefix == "slices." || prefix == "functionHighlight.") && key.find ("function.") == 0)))
            return true;
    return false;
}

overlaylayers::Layer Apply (const overlaylayers::Layer& source, const Snapshot& settings)
{
    auto layer = source;
    const auto c = Category (source) + ".";
    bool surfaceEdited = false;
    for (const auto& [key, value] : settings.values)
        if (value.enabled && key.find (c + "surface") == 0)
            surfaceEdited = true;
    const auto number = [&] (const char* key, double fallback) { return Number (settings, c + key, fallback); };
    const auto colour = [&] (const char* key, uint32_t fallback) { return Tint (settings, c + key, fallback); };
    const auto functionColour = [&] (uint32_t authored) {
        if (c != "slices." && c != "functionHighlight.")
            return authored;
        for (const auto& [key, d] : settings.definitions)
            if (d.group == "Story slice functions" && (uint32_t (d.initial) & 0xFFFFFF00u) == (authored & 0xFFFFFF00u))
                return (Tint (settings, key, authored) & 0xFFFFFF00u) | (authored & 255u);
        return authored;
    };
    const auto functionRoleColour = [&] (const std::string& function, uint32_t authored) {
        if (function.empty ())
            return functionColour (authored);
        return (Tint (settings, "function." + function, authored) & 0xFFFFFF00u) | (authored & 255u);
    };
    const bool replaceCollapseHatch =
        c == "collapseZone." && (settings.values.at (c + "surfaceIsHatched").enabled ||
                                 settings.values.at (c + "surfaceHatchDirection").enabled ||
                                 settings.values.at (c + "surfaceHatchDensity").enabled);
    if (replaceCollapseHatch)
        layer.polylines.clear (); // Replace authored hatch geometry with model-anchored shader hatching.
    for (auto& mesh : layer.meshes) {
        mesh.rgba = functionRoleColour (mesh.graphicsFunction, mesh.rgba);
        mesh.rgba = colour ("surfaceColor", mesh.rgba);
        if (settings.values.at (c + "surfaceColor").enabled)
            mesh.vertexRgba.clear ();
        mesh.style.opacity = float (number ("surfaceOpacity", mesh.style.opacity));
        mesh.style.occludedOpacity = float (number ("surfaceOcclusionIntensity", mesh.style.occludedOpacity));
        mesh.style.hatched = number ("surfaceIsHatched", replaceCollapseHatch || mesh.style.hatched) != 0;
        mesh.style.hatchDirection = float (number ("surfaceHatchDirection", mesh.style.hatchDirection));
        mesh.style.hatchDensity = float (number ("surfaceHatchDensity", mesh.style.hatchDensity));
        if (settings.values.at (c + "surfaceOcclusionIntensity").enabled)
            mesh.style.behind = overlaylayers::Behind::Fade;
        mesh.styled = mesh.styled || surfaceEdited;
        if ((mesh.style.edgeRgba & 255u) != 0) {
            mesh.style.edgeRgba =
                Alpha (colour ("lineColor", functionRoleColour (mesh.graphicsFunction, mesh.style.edgeRgba)),
                       number ("lineOpacity", 1));
            mesh.style.edgeWidthPixels = float (number ("lineThickness", mesh.style.edgeWidthPixels));
            if (settings.values.at (c + "lineDashLength").enabled || settings.values.at (c + "lineGapLength").enabled) {
                const float dash = float (number (
                    "lineDashLength", mesh.style.edgeDashMetres.empty () ? 0 : mesh.style.edgeDashMetres.front ()));
                mesh.style.edgeDashMetres = dash > 0
                                                ? std::vector<float> { dash, float (number ("lineGapLength", 0.3)) }
                                                : std::vector<float> {};
            }
        }
    }
    for (auto& line : layer.polylines) {
        const auto prefix = source.name == "tapioca.massing.lines"
                                ? std::string ((line.rgba & 0xFFFFFF00u) == 0xAA446500u ? "parcel." : "offset.")
                                : c;
        line.rgba = Alpha (Tint (settings, prefix + "lineColor", functionRoleColour (line.graphicsFunction, line.rgba)),
                           Number (settings, prefix + "lineOpacity", 1));
        line.widthPixels = float (Number (settings, prefix + "lineThickness", line.widthPixels));
        if (settings.values.at (prefix + "lineThickness").enabled)
            line.behind =
                overlaylayers::Resolve (line.behind, layer); // Pixel-wide solid strokes also need the guest in 3D.
        if (settings.values.at (prefix + "lineDashLength").enabled ||
            settings.values.at (prefix + "lineGapLength").enabled) {
            const auto dash = float (
                Number (settings, prefix + "lineDashLength", line.dashMetres.empty () ? 0 : line.dashMetres.front ()));
            line.dashMetres =
                dash > 0 ? std::vector<float> { dash, float (Number (settings, prefix + "lineGapLength", 0.3)) }
                         : std::vector<float> {};
        }
    }
    for (auto& point : layer.points) {
        const float size = float (number ("pointSize", point.sizePixels));
        point.sizeMetres *= size / point.sizePixels;
        point.sizePixels = size;
        point.rgba = colour ("pointColor", point.rgba);
    }
    for (auto& text : layer.texts) {
        const float size = float (number ("textSize", text.sizePixels));
        text.sizeMetres *= size / text.sizePixels;
        text.sizePixels = size;
        text.rgba = colour ("textColor", text.rgba);
        text.haloPixels = float (number ("textHalo", text.haloPixels));
        text.haloRgba = colour ("textHaloColor", text.haloRgba);
        text.minProjectedPixels = float (number ("textHideDistance", text.minProjectedPixels));
    }
    for (auto& dimension : layer.dimensions) {
        const float size = float (number ("textSize", dimension.textSizePixels));
        dimension.textSizeMetres *= size / dimension.textSizePixels;
        dimension.textSizePixels = size;
        dimension.textRgba = colour ("textColor", dimension.textRgba);
        dimension.haloPixels = float (number ("textHalo", dimension.haloPixels));
        dimension.haloRgba = colour ("textHaloColor", dimension.haloRgba);
        dimension.textMinProjectedPixels = float (number ("textHideDistance", dimension.textMinProjectedPixels));
        dimension.rgba = Alpha (colour ("lineColor", dimension.rgba), number ("lineOpacity", 1));
        dimension.widthPixels = float (number ("lineThickness", dimension.widthPixels));
    }
    return layer;
}
std::string Encode (const Snapshot& settings)
{
    namespace json = evp::nodegraph::json;
    using J = json::JsonValue;
    json::JsonObject values;
    for (const auto& [key, value] : settings.values) {
        const auto& d = settings.definitions.at (key);
        J encoded = J::Double (value.number);
        if (d.kind == Kind::Colour) {
            std::ostringstream hex;
            hex << '#' << std::hex << std::uppercase << std::setw (8) << std::setfill ('0') << uint32_t (value.number);
            encoded = J::String (hex.str ());
        }
        else if (d.kind == Kind::Boolean)
            encoded = J::Bool (value.number != 0);
        values[key] = J::Object ({ { "enabled", J::Bool (value.enabled) },
                                   { "value", std::move (encoded) },
                                   { "units", J::String (d.units) },
                                   { "group", J::String (d.group) } });
    }
    return json::Write (
               J::Object (
                   { { "schema", J::String ("tapioca.graphics-settings") },
                     { "version", J::Integer (1) },
                     { "revision", J::Integer (int64_t (settings.revision)) },
                     { "disabledPolicy",
                       J::String (
                           "Use each producer's authored style; disabled values are reference seeds, not overrides.") },
                     { "settings", J::Object (std::move (values)) } }),
               2) +
           "\n";
}
SaveResult Save (const Snapshot& settings, const std::filesystem::path& logs)
{
    SaveResult result;
    std::filesystem::path temporary;
    if (logs.empty ()) {
        result.error = "Logs folder is unavailable.";
        return result;
    }
    try {
        std::filesystem::create_directories (logs);
        static std::atomic<uint64_t> sequence { 0 };
        const auto now = std::chrono::system_clock::now ().time_since_epoch ();
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds> (now).count ();
        result.path =
            logs / ("graphics-settings-" + std::to_string (stamp) + "-" + std::to_string (++sequence) + ".json");
        temporary = result.path;
        temporary += ".tmp";
        std::ofstream file (temporary, std::ios::binary);
        file << Encode (settings);
        file.flush ();
        const bool written = bool (file);
        file.close ();
        if (!written || file.fail ()) {
            std::filesystem::remove (temporary);
            result.error = "Could not write graphics settings.";
            return result;
        }
        std::filesystem::rename (temporary, result.path);
        result.saved = true;
    }
    catch (const std::exception& error) {
        result.error = error.what ();
        if (!temporary.empty ()) {
            std::error_code ignored;
            std::filesystem::remove (temporary, ignored);
        }
    }
    return result;
}
SaveResult SaveToLogs (const Snapshot& settings)
{
#ifdef _WIN32
    const auto* root = _wgetenv (L"LOCALAPPDATA");
#else
    const auto* root = std::getenv ("LOCALAPPDATA");
#endif
    if (!root || !*root)
        return { false, {}, "LOCALAPPDATA is unavailable." };
    return Save (settings, std::filesystem::path (root) / "Tapioca" / "logs");
}
} // namespace geomsrv::archviz::graphicssettings
