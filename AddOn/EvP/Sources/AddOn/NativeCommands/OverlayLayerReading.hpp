#ifndef EVP_NATIVECOMMANDS_OVERLAYLAYERREADING_HPP
#define EVP_NATIVECOMMANDS_OVERLAYLAYERREADING_HPP

// NativeCommands/OverlayLayerReading -- Tapioca.SetOverlayLayer's parameters read into a
// layer: polylines, points, meshes (styles, heatmaps), texts (at a point, on the view,
// on a plane), dimensions, legends and HUD panels. Apart from the verbs
// (OverlayLayerCommands.cpp) because the vocabulary outgrew one file with them.
//
// ⚠️ WHAT THE SCHEMA CANNOT SAY, THIS CHECKS: colours are the shared #Color, 6 or 8 hex
// digits (the validator has no `pattern`), a text is anchored `at` a model point or on the `screen`, never
// both, and the layer's own `Validate` has the last word. Every number is read through
// CommandUtils' ReadReal/ReadReals, so a JSON whole number is a number too.

#include "ObjectState.hpp"

#include <cstdint>

#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {
struct Layer;
struct HiddenLine;
struct Colormap;
enum class Behind : uint8_t;
} // namespace overlaylayers
} // namespace archviz

namespace overlayreading {

// The layer the parameters describe, validated; false with `error` a sentence a
// caller can act on.
bool ReadLayer (const GS::ObjectState& params, archviz::overlaylayers::Layer& layer, std::string& error);

// A string field as UTF-8; empty when absent.
std::string StringOf (const GS::ObjectState& item, const char* key);

// The shared #Color at `key` into 0xRRGGBBAA, untouched when absent; false with `error`
// when it is not 6 or 8 hex digits. Every overlay verb reads its colours here.
bool ReadColour (const GS::ObjectState& item, const char* key, uint32_t& rgba, std::string& error);

// The font named at `key` -- an installed family or a font file -- as the file's path,
// untouched when absent; false with `error` when no such font is installed.
bool ReadFont (const GS::ObjectState& item, const char* key, std::string& path, std::string& error);

// A dash pattern in metres at `key` -- on, off, on, off... -- untouched when absent.
void ReadDash (const GS::ObjectState& item, const char* key, std::vector<float>& lengths);

// A colour ramp: a `preset` or `stops`, with an optional `min` and `max`.
bool ReadRamp (const GS::ObjectState& item, archviz::overlaylayers::Colormap& colormap, std::string& error);

// A line's `hidden` style: its colour, width and pattern behind the building.
bool ReadHiddenLine (const GS::ObjectState& item, archviz::overlaylayers::HiddenLine& hidden, std::string& error);

// `occlusion` ("always", "hide", "fade", "dash"); `fallback` when absent.
archviz::overlaylayers::Behind OcclusionOf (const GS::ObjectState& item, archviz::overlaylayers::Behind fallback);

} // namespace overlayreading
} // namespace geomsrv

#endif
