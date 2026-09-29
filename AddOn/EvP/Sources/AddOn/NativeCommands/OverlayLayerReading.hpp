#ifndef EVP_NATIVECOMMANDS_OVERLAYLAYERREADING_HPP
#define EVP_NATIVECOMMANDS_OVERLAYLAYERREADING_HPP

// NativeCommands/OverlayLayerReading -- Tapioca.SetOverlayLayer's parameters read into a
// layer: polylines, points, meshes (styles, heatmaps), texts (at a point or on the
// view), dimensions and legends. Apart from the verbs (OverlayLayerCommands.cpp), so the
// vocabulary can grow without the verbs' file growing with it.
//
// ⚠️ WHAT THE SCHEMA CANNOT SAY, THIS CHECKS: colours are 8 hex digits (the validator
// has no `pattern`), a text is anchored `at` a model point or on the `screen`, never
// both, and the layer's own `Validate` has the last word. Every number is read through
// CommandUtils' ReadReal/ReadReals, so a JSON whole number is a number too.

#include "ObjectState.hpp"

#include <string>

namespace geomsrv {
namespace archviz {
namespace overlaylayers {
struct Layer;
}
} // namespace archviz

namespace overlayreading {

// The layer the parameters describe, validated; false with `error` a sentence a
// caller can act on.
bool ReadLayer (const GS::ObjectState& params, archviz::overlaylayers::Layer& layer, std::string& error);

// A string field as UTF-8; empty when absent.
std::string StringOf (const GS::ObjectState& item, const char* key);

} // namespace overlayreading
} // namespace geomsrv

#endif
