#ifndef EVP_NATIVECOMMANDS_MODELACCESSUTILS_HPP
#define EVP_NATIVECOMMANDS_MODELACCESSUTILS_HPP

#include "ObjectState.hpp"
#include "UniString.hpp"

#include <CoordinateSystem.hpp>
#include <ModelElement.hpp>

class Box3D;

namespace ModelerAPI {
    class Model;
    class Vertex;
    class Vector;
    class Color;
    class Material;
    class Texture;
    class AttributeIndex;
    struct TextureCoordinateSystem;
}

// Shared plumbing for the ModelerAPI-based model-read domains:
//   ModelGeometryCommands   (model / element / mesh-body BREP)
//   ModelAppearanceCommands (materials, colors, textures, lights, UVs)
//   NurbsCommands           (NURBS bodies, point clouds)
//
// WHY THIS IS NOT CommandUtils. CommandUtils is the cross-domain helper home and
// its header is deliberately kept to ACAPI + ObjectState, because every single
// NativeCommands/*.cpp includes it. Everything below needs the GSModelDevLib
// headers (`ModelElement.hpp`, `CoordinateSystem.hpp`, and a forward-declared
// `Box3D`), which is exactly the weight CommandUtils exists to keep out of the
// twenty domain files that never touch the modeler. Same rule, applied one level
// down: these moved here on their SECOND consumer, not speculatively, and the
// only files that include this header are the model-access domains themselves.
//
// The C API ModelAccess path (`ACAPI_ModelAccess_*`, Component3DCommands) shares
// nothing with these — it speaks API_Component3D, not ModelerAPI — so it
// deliberately does not include this header.
//
// MAIN THREAD ONLY, like every ModelerAPI read.
namespace geomsrv {

// ---------------------------------------------------------------------------
// Coordinate system: "world" (default) or "local" (the element's own frame).
//
// Every ModelerAPI geometry getter takes one, and the answer is materially
// different — a door's local frame is the door, world is the project. Parsed in
// one place so the spelling can never drift between commands. An unrecognised
// value falls back to World, which is what a caller that did not think about it
// wants.
// ---------------------------------------------------------------------------
ModelerAPI::CoordinateSystem ParseCoordinateSystem (const GS::ObjectState& params,
                                                    const char* key = "coordinateSystem");

// The name to echo back so a response says which frame it is in.
GS::UniString CoordinateSystemName (ModelerAPI::CoordinateSystem cs);

// ---------------------------------------------------------------------------
// `include` — opt-in payload sections
//
// The body reads can return megabytes (every edge, every polygon contour, every
// convex sub-triangle). `include:["vertices","polygons"]` keeps a caller that
// wants counts from paying for the lot. An ABSENT `include` means "the default
// set", which is why the default is per-section and not a single global flag.
// ---------------------------------------------------------------------------
bool WantsSection (const GS::ObjectState& params, const char* section, bool byDefault);

// ---------------------------------------------------------------------------
// ObjectState conversions. Nested records per E16.0 — `{"x":…,"y":…,"z":…}` for
// single points (Tapir's convention), flat arrays for bulk vertex/index data.
// ---------------------------------------------------------------------------
GS::ObjectState PointToObjectState (double x, double y, double z);
GS::ObjectState VertexToObjectState (const ModelerAPI::Vertex& v);
GS::ObjectState VectorToObjectState (const ModelerAPI::Vector& v);
GS::ObjectState ColorToObjectState (const ModelerAPI::Color& c);
GS::ObjectState BoxToObjectState (const Box3D& box);

// An AttributeIndex has THREE numbers on it and they are not interchangeable:
// `index` is what the rest of EvP already uses (GeometryExtractor's triMaterial),
// `originalModelerIndex` / `originalIndex` identify it in the modeler's and
// Archicad's own attribute tables. Report all three rather than guessing which
// one a caller will need to join on.
GS::ObjectState AttributeIndexToObjectState (const ModelerAPI::AttributeIndex& index);

// The full ModelerAPI::Material record (type, colours, reflections, texture
// link). Shared by the material listing and the NURBS body read, which carries
// its own material.
GS::ObjectState MaterialToObjectState (const ModelerAPI::Material& material);

// The full ModelerAPI::Texture record — METADATA ONLY, never the pixel map. A
// pixel map is up to `GetPixelMapBufferSize()` bytes and would have to cross the
// JSON bus; `checksum`/`fingerprint` identify the image and `EvP.GetTexturePixels`
// is the deliberate opt-in for the samples themselves.
GS::ObjectState TextureToObjectState (const ModelerAPI::Texture& texture);

// Box / cylindrical / spherical / NURBS-parametric UV frame.
GS::ObjectState TextureCoordSysToObjectState (const ModelerAPI::TextureCoordinateSystem& cs);

// ---------------------------------------------------------------------------
// Element identity
// ---------------------------------------------------------------------------

// `ModelerAPI::Element::Type` as a name a script can read and switch on. The raw
// enum value travels too (`type`), because this list is the AC29 one and a newer
// Archicad can return a value that is not in it.
GS::UniString ElementTypeName (ModelerAPI::Element::Type type);

// A model element's GUID in the same string form every other EvP command speaks.
GS::UniString ElementGuidString (const ModelerAPI::Element& elem);

// ---------------------------------------------------------------------------
// The empty-model diagnosis, in one place
//
// A sight whose model has never been GENERATED answers every attribute query
// (surfaces, colours, lights all come back populated) and reports ZERO elements.
// That combination is not an error from ACAPI's point of view, so every command
// here would otherwise fail further downstream with a misleading message about
// the caller's guid — which is exactly what happened on the first live run.
// Say the real thing instead, and name the C-API path that works anyway.
// ---------------------------------------------------------------------------
GS::UniString EmptyModelHint ();

// Resolve the element a command was pointed at: `guid:"…"` (preferred) or
// `elementIndex:N` (1-based, the modeler's own numbering — what GetModelElements
// hands back and what the C API's elemIdx is derived from).
//
// Returns false with `err` set, never a silent miss: "that element has no 3D
// representation" and "you passed a bad guid" are different problems and a
// caller cannot tell them apart from an empty result. `elemIndex` is the
// 1-based modeler index of what was found.
bool ResolveModelElement (const ModelerAPI::Model& model, const GS::ObjectState& params,
                          ModelerAPI::Element& elem, Int32& elemIndex, GS::UniString& err);

} // namespace geomsrv

#endif
