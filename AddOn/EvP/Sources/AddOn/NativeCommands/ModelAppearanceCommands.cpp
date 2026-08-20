#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ModelAppearanceCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/ModelAccessUtils.hpp"

#include "Geometry/GeometryExtractor.hpp"     // AcquireCurrentModel

#include <Model.hpp>
#include <ModelElement.hpp>
#include <ModelMeshBody.hpp>
#include <ModelMaterial.hpp>
#include <ModelColor.hpp>
#include <Polygon.hpp>
#include <Light.hpp>
#include <Texture.hpp>
#include <TextureCoordinate.hpp>
#include <Parameter.hpp>
#include <ParameterList.hpp>

#include <exception>
#include <optional>
#include <AttributeIndex.hpp>
#include <Vertex.hpp>

namespace geomsrv {

namespace {

void AddElementId (const GS::UniString& guid, GS::ObjectState& record)
{
    GS::ObjectState elementId;
    elementId.Add ("guid", guid);
    record.Add ("elementId", elementId);
}

bool ResolveElement (const ModelerAPI::Model& model, const GS::ObjectState& params,
                     ModelerAPI::Element& elem, Int32& elemIndex, GS::UniString& err)
{
    GS::ObjectState elementId;
    GS::UniString guid;
    if (params.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ()) {
        const std::optional<Int32> found = model.GetElementIndex (
            APIGuid2GSGuid (APIGuidFromString (guid.ToCStr ().Get ())));
        if (!found.has_value ()) {
            err = "no 3D model element for elementId.guid \"" + guid + "\"";
            return false;
        }
        elemIndex = *found;
        model.GetElement (elemIndex, &elem);
        return true;
    }
    return ResolveModelElement (model, params, elem, elemIndex, err);
}

// ===========================================================================
// E24 — the appearance pool behind the geometry.
//
// EvP.GetBodyGeometry hands back a `materialIndex` per polygon and a
// `colorIndex` per edge; those are indices INTO THIS MODEL'S pools, and until
// now nothing could resolve them. GeometryExtractor pushed the same integer into
// `triMaterial` and every consumer had to treat it as an opaque group key —
// good enough to split a mesh by surface, useless for "what colour is it",
// "is it glass", "where does the texture sit".
//
// These are reads. Gate, no undo scope.
//
// ⚠️ MODEL INDICES ARE NOT ATTRIBUTE INDICES. The numbers here index the 3D
// MODEL's own pool, which contains only the surfaces the model actually uses,
// renumbered. They are NOT Archicad attribute indices — `EvP.GetAttributeInfo`
// and the surface pickers speak those, and the two do not interchange.
// `originalIndex` on each record is the bridge back when there is one.
// ===========================================================================

// Which pool indices to report: the caller's `indices`, or 1..count.
GS::Array<GS::Int32> RequestedIndices (const GS::ObjectState& params, Int32 count)
{
    GS::Array<GS::Int32> wanted;
    if (params.Get ("indices", wanted) && !wanted.IsEmpty ())
        return wanted;
    for (Int32 i = 1; i <= count; ++i)
        wanted.Push ((GS::Int32) i);
    return wanted;
}


// Light `GetExtraParameters` / material extras: GDL-defined name/value pairs.
// Scalars and strings are emitted; an ARRAY parameter is reported by name and
// type only, because flattening a 2D GDL array into this response would invent a
// convention nothing else in EvP uses.
//
// ⚠️ INDEX BASE IS NOT DOCUMENTED for ParameterList. Everything else in
// ModelerAPI is 1-based, so that is what we try first; `GetParameter` returns
// false rather than throwing on a bad index, so if a 1-based walk of a non-empty
// list yields nothing we retry from 0 instead of shipping a silently empty
// `parameters` array. Delete the fallback once a probe run settles which it is.
GS::Array<GS::ObjectState> DescribeParameters (const ModelerAPI::ParameterList& list)
{
    GS::Array<GS::ObjectState> out;
    const Int32 count = list.GetParameterCount ();
    const auto anyReadable = [&list, count] (Int32 base) {
        for (Int32 i = base; i < base + count; ++i) {
            ModelerAPI::Parameter probe;
            if (list.GetParameter (i, &probe))
                return true;
        }
        return false;
    };
    const Int32 base = anyReadable (1) ? 1 : 0;

    for (Int32 i = base; i < base + count; ++i) {
        ModelerAPI::Parameter parameter;
        if (!list.GetParameter (i, &parameter))
            continue;
        GS::ObjectState p;
        p.Add ("name", parameter.GetName ());
        p.Add ("type", (GS::Int32) parameter.GetType ());
        p.Add ("isArray", parameter.IsArray ());
        switch (parameter.GetType ()) {
            case ModelerAPI::Parameter::NumericType:
                p.Add ("value", (double) parameter);
                break;
            case ModelerAPI::Parameter::StringType: {
                GS::UniString text;
                parameter.GetStringValue (text);
                p.Add ("value", text);
                break;
            }
            default: {
                Int32 d1 = 0, d2 = 0;
                if (parameter.IsArray ()) {
                    parameter.GetArrayDimensions (&d1, &d2);
                    p.Add ("dimension1", (GS::Int32) d1);
                    p.Add ("dimension2", (GS::Int32) d2);
                }
                break;
            }
        }
        out.Push (p);
    }
    return out;
}


// ---------------------------------------------------------------------------
// EvP.GetModelMaterials { indices?:[…] }
//   -> { ok, count, materials:[{ modelIndex, type, typeName, name, surfaceColor,
//        ambientReflection, diffuseReflection, specularReflection, specularColor,
//        transparency, transparencyAttenuation, shining, emissionColor,
//        emissionAttenuation, hasTexture, textureName?, textureIndex?,
//        fillIndex, fillColorIndex, externalReference }] }
//
// The model's surface pool, resolved. This is what turns a snapshot's
// `triMaterial` group key into "Glass - Clear, 85% transparent" — the read that
// makes a per-surface export (glTF, OBJ+MTL, a takeoff by finish) possible at
// all.
//
// `transparency` is 0..1 where 1 is fully transparent; the colours are 0..1 RGB
// doubles, NOT 0..255 bytes. Both are ModelerAPI's conventions, passed through.
// ---------------------------------------------------------------------------
class GetModelMaterialsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelMaterials"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetModelMaterials"));
        }

        const Int32 count = model.GetMaterialCount ();
        GS::Array<GS::ObjectState> materials;
        for (GS::Int32 i : RequestedIndices (params, count)) {
            if (i < 1 || i > count)
                continue;               // out-of-pool index: skipped, `count` says why
            const ModelerAPI::AttributeIndex index (ModelerAPI::AttributeIndex::MaterialIndex, (Int32) i);
            ModelerAPI::Material material;
            model.GetMaterial (index, &material);

            GS::ObjectState record = MaterialToObjectState (material);
            record.Add ("modelIndex", i);
            materials.Push (record);
        }

        os.Add ("materialCount", (GS::Int32) count);
        os.Add ("count", (GS::Int32) materials.GetSize ());
        os.Add ("materials", materials);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetModelColors { indices?:[…] }
//   -> { ok, colorCount, count, colors:[{ modelIndex, red, green, blue }] }
//
// The pen-colour pool the body/edge `colorIndex` points at. Small, and the only
// way to draw a wireframe in the colours Archicad would.
// ---------------------------------------------------------------------------
class GetModelColorsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelColors"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetModelColors"));
        }

        const Int32 count = model.GetColorCount ();
        GS::Array<GS::ObjectState> colors;
        for (GS::Int32 i : RequestedIndices (params, count)) {
            if (i < 1 || i > count)
                continue;
            const ModelerAPI::AttributeIndex index (ModelerAPI::AttributeIndex::PenColorIndex, (Int32) i);
            ModelerAPI::Color color;
            model.GetColor (index, &color);

            GS::ObjectState record = ColorToObjectState (color);
            record.Add ("modelIndex", i);
            colors.Push (record);
        }

        os.Add ("colorCount", (GS::Int32) count);
        os.Add ("count", (GS::Int32) colors.GetSize ());
        os.Add ("colors", colors);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetModelTextures { indices?:[…], usedOnly?:bool }
//   -> { ok, textureCount, count, textures:[{ modelIndex, used, name, available,
//        pixelMapXSize, pixelMapYSize, xSize, ySize, checksum, fingerprint,
//        bumpMapPattern, … }] }
//
// Texture METADATA — never the pixels (EvP.GetTexturePixels is the explicit
// opt-in for those, and it is capped). `checksum` and `fingerprint` identify the
// image, which is what an exporter needs to write each bitmap once and reference
// it N times.
//
// `usedOnly` filters to textures the model actually references
// (Model::IsTextureUsed); an unused entry is real but usually noise.
// ---------------------------------------------------------------------------
class GetModelTexturesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelTextures"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetModelTextures"));
        }

        bool usedOnly = false;
        params.Get ("usedOnly", usedOnly);

        const Int32 count = model.GetTextureCount ();
        GS::Array<GS::ObjectState> textures;
        for (GS::Int32 i : RequestedIndices (params, count)) {
            if (i < 1 || i > count)
                continue;
            const ModelerAPI::AttributeIndex index (ModelerAPI::AttributeIndex::TextureIndex, (Int32) i);
            const bool used = model.IsTextureUsed (index);
            if (usedOnly && !used)
                continue;

            ModelerAPI::Texture texture;
            model.GetTexture (index, &texture);

            GS::ObjectState record = TextureToObjectState (texture);
            record.Add ("modelIndex", i);
            record.Add ("used", used);
            textures.Push (record);
        }

        os.Add ("textureCount", (GS::Int32) count);
        os.Add ("count", (GS::Int32) textures.GetSize ());
        os.Add ("textures", textures);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetTexturePixels { index | name, x?, y?, width?, height?, maxPixels? }
//   -> { ok, name, x, y, width, height, pixelMapXSize, pixelMapYSize,
//        truncated, pixels:[a,r,g,b, a,r,g,b, …] }
//
// The actual samples, as ARGB bytes, for a REGION of one texture.
//
// ⚠️ CAPPED ON PURPOSE, and the cap is the point. A 2048x2048 texture is 4.2M
// pixels; as JSON integers that is well over a hundred megabytes crossing the
// bus for one call, which is not a slow response, it is a hung Archicad. So:
// the region defaults to the whole image but `maxPixels` (default 65536) stops
// there and sets `truncated`. Read a thumbnail, or walk the image in tiles.
//
// Rows are emitted top-to-bottom, left-to-right, four bytes per pixel in ARGB
// order — ModelerAPI::Texture::ARGBPixel's own field order, not RGBA.
// ---------------------------------------------------------------------------
class GetTexturePixelsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetTexturePixels"; }

    static constexpr GS::Int32 DefaultMaxPixels = 65536;   // 256x256

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetTexturePixels"));
        }

        ModelerAPI::Texture texture;
        GS::UniString name;
        GS::Int32 index = 0;
        if (params.Get ("name", name) && !name.IsEmpty ()) {
            model.GetTexture (name, &texture);
        } else if (params.Get ("index", index)) {
            const Int32 count = model.GetTextureCount ();
            if (index < 1 || index > count) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    GS::UniString::Printf ("texture index %d is out of range (1..%d)",
                                           (int) index, (int) count),
                    "indices come from EvP.GetModelTextures"));
            }
            model.GetTexture (ModelerAPI::AttributeIndex (ModelerAPI::AttributeIndex::TextureIndex,
                                                          (Int32) index), &texture);
        } else {
            return NativeCommandResult::Failure (
                EVP_FAIL ("need index=N or name=\"…\"", "EvP.GetTexturePixels"));
        }

        if (!texture.IsAvailable ()) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("that texture's pixel map is not available",
                          "the image file may be missing from the loaded libraries"));
        }

        const Int32 fullWidth  = texture.GetPixelMapXSize ();
        const Int32 fullHeight = texture.GetPixelMapYSize ();

        GS::Int32 x = 0, y = 0, width = (GS::Int32) fullWidth, height = (GS::Int32) fullHeight;
        params.Get ("x", x);
        params.Get ("y", y);
        params.Get ("width", width);
        params.Get ("height", height);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (width  <= 0 || x + width  > fullWidth)  width  = fullWidth  - x;
        if (height <= 0 || y + height > fullHeight) height = fullHeight - y;
        if (width <= 0 || height <= 0) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("the requested region is outside the pixel map",
                          GS::UniString::Printf ("map is %dx%d", (int) fullWidth, (int) fullHeight)));
        }

        GS::Int32 maxPixels = DefaultMaxPixels;
        params.Get ("maxPixels", maxPixels);
        if (maxPixels <= 0)
            maxPixels = DefaultMaxPixels;

        // Truncate by whole ROWS so the result is still a rectangle a caller can
        // decode — a partial row would need a stride convention nobody asked for.
        bool truncated = false;
        if ((GS::Int64) width * height > maxPixels) {
            GS::Int32 rows = maxPixels / width;
            if (rows < 1) rows = 1;
            if (rows < height) {
                height = rows;
                truncated = true;
            }
        }

        GS::Array<GS::Int32> pixels;
        for (GS::Int32 row = 0; row < height; ++row) {
            for (GS::Int32 col = 0; col < width; ++col) {
                ModelerAPI::Texture::ARGBPixel pixel;
                texture.GetPixel ((Int32) (x + col), (Int32) (y + row), &pixel);
                pixels.Push ((GS::Int32) pixel.alpha);
                pixels.Push ((GS::Int32) pixel.red);
                pixels.Push ((GS::Int32) pixel.green);
                pixels.Push ((GS::Int32) pixel.blue);
            }
        }

        os.Add ("name", texture.GetName ());
        os.Add ("pixelMapXSize", (GS::Int32) fullWidth);
        os.Add ("pixelMapYSize", (GS::Int32) fullHeight);
        os.Add ("x", x);
        os.Add ("y", y);
        os.Add ("width", width);
        os.Add ("height", height);
        os.Add ("truncated", truncated);
        os.Add ("pixels", pixels);           // flat ARGB bytes, row-major
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetModelLights { guid?, elementIndex?, coordinateSystem?, specials?:bool,
//                      include?:["parameters"] }
//   -> { ok, count, lights:[{ scope, elementIndex?, guid?, lightIndex, type,
//        typeName, castsShadow, color, position, direction, upVector, radius,
//        angleFalloff, falloffAngle1, falloffAngle2, distanceFalloff,
//        minDistance, maxDistance, parameters? }] }
//
// Every light in the model, or just one element's. With `specials` (default
// true) the three synthetic lights — ambient, camera, sun — come too; the sun is
// the one most callers are actually after, because it is the shadow direction
// and nothing else in EvP exposes it.
//
// Cone angles are in the API's own units (the falloff angles are radians as
// modelled); they are passed through rather than converted, so a caller sees
// exactly what Archicad has.
// ---------------------------------------------------------------------------
class GetModelLightsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelLights"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetModelLights"));
        }

        const ModelerAPI::CoordinateSystem cs = ParseCoordinateSystem (params);
        const bool wantParameters = WantsSection (params, "parameters", false);

        GS::Array<GS::ObjectState> lights;

        // Lights that could not be described, by index, with the reason. See
        // DescribeOrSkip: ONE unreadable lamp must not cost the caller the other 255.
        GS::Array<GS::ObjectState> skipped;

        // Element-scoped, when the caller named one. An element's lights are its
        // own (a lamp object's emitters), indexed within the element.
        GS::Int32 elementIndex = 0;
        GS::ObjectState requestedElementId;
        if (params.Get ("elementId", requestedElementId) ||
            params.Get ("elementIndex", elementIndex)) {
            ModelerAPI::Element elem;
            Int32 resolved = 0;
            GS::UniString err;
            if (!ResolveElement (model, params, elem, resolved, err))
                return NativeCommandResult::Failure (
                    EVP_FAIL (err, "EvP.GetModelLights element lookup"));
            const Int32 count = elem.GetLightCount ();
            for (Int32 i = 1; i <= count; ++i) {
                GS::ObjectState record;
                if (!DescribeOrSkip (elem, i, cs, wantParameters, record, skipped))
                    continue;
                record.Add ("scope", GS::UniString ("element"));
                record.Add ("elementIndex", (GS::Int32) resolved);
                AddElementId (ElementGuidString (elem), record);
                record.Add ("lightIndex", (GS::Int32) i);
                lights.Push (record);
            }
        } else {
            const Int32 count = model.GetLightCount ();
            for (Int32 i = 1; i <= count; ++i) {
                GS::ObjectState record;
                if (!DescribeOrSkip (model, i, cs, wantParameters, record, skipped))
                    continue;
                record.Add ("scope", GS::UniString ("model"));
                record.Add ("lightIndex", (GS::Int32) i);
                // Documented in Light.hpp, not inferred: AMBIENT_LIGHT_INDEX 1,
                // CAMERA_LIGHT_INDEX 2, SUN_LIGHT_INDEX 3. The model list's first
                // three entries ARE the specials, which is why `specials:true`
                // appears to list them twice — the two scopes are different
                // accessors onto the same lights, not two sets to be added up.
                if (i <= 3)
                    record.Add ("isSpecialIndex", true);
                lights.Push (record);
            }

            // ⚠️ THE REAL LAMPS ARE NOT IN THE MODEL-SCOPE LIST. PROVEN, NOT GUESSED.
            //
            // `GetLightCount()` counts every light in the model — 262 in the test
            // project — but `Model::GetLight(i)` only ever serves the three
            // synthetic ones. Every index >= 4 throws GS::IllegalArgumentException
            // from the FETCH itself (no accessor is reached, so the coordinate
            // system and every getter are excluded), and Light.hpp fixes
            // AMBIENT/CAMERA/SUN at exactly 1/2/3.
            //
            // Confirmed live 2026-08-03: 259 model-scope indices threw, while an
            // ELEMENT-scoped read on lamp elements returned them without a single
            // failure — spot and point lights with real positions. So the lights
            // are reachable; they are just only reachable through their element.
            //
            // This sweep closes that gap: when the model-scope walk lost lights,
            // collect them from the elements that own them. Off with
            // `sweepElements:false` — it walks every element in the model, which is
            // cheap per element (no gate hops, all in-process) but not free on a
            // 13k-element project.
            bool sweepElements = true;
            params.Get ("sweepElements", sweepElements);
            if (sweepElements && !skipped.IsEmpty ()) {
                const Int32 elemCount = model.GetElementCount ();
                GS::Int32 recovered = 0;
                for (Int32 e = 1; e <= elemCount; ++e) {
                    ModelerAPI::Element elem;
                    model.GetElement (e, &elem);
                    const Int32 elemLights = elem.GetLightCount ();
                    if (elemLights <= 0)
                        continue;
                    for (Int32 k = 1; k <= elemLights; ++k) {
                        GS::ObjectState record;
                        if (!DescribeOrSkip (elem, k, cs, wantParameters, record, skipped))
                            continue;
                        record.Add ("scope", GS::UniString ("element"));
                        record.Add ("elementIndex", (GS::Int32) e);
                        AddElementId (ElementGuidString (elem), record);
                        record.Add ("lightIndex", (GS::Int32) k);
                        // So a caller can tell these apart from the model-scope
                        // three, and knows they were RECOVERED rather than listed.
                        record.Add ("viaElementSweep", true);
                        lights.Push (record);
                        ++recovered;
                    }
                }
                os.Add ("sweptElements", true);
                os.Add ("recoveredFromElements", recovered);
            }

            bool specials = true;
            params.Get ("specials", specials);
            if (specials) {
                AddSpecial (model, ModelerAPI::Model::AmbientLightType, "ambient", cs, wantParameters, lights, skipped);
                AddSpecial (model, ModelerAPI::Model::CameraLightType,  "camera",  cs, wantParameters, lights, skipped);
                AddSpecial (model, ModelerAPI::Model::SunLightType,     "sun",     cs, wantParameters, lights, skipped);
            }
        }

        os.Add ("coordinateSystem", CoordinateSystemName (cs));
        os.Add ("count", (GS::Int32) lights.GetSize ());
        os.Add ("lights", lights);
        os.Add ("skippedCount", (GS::Int32) skipped.GetSize ());
        if (!skipped.IsEmpty ())
            os.Add ("skipped", skipped);     // [{lightIndex, reason}] — never silent
        return os;
    }

private:
    static GS::UniString LightTypeName (ModelerAPI::Light::Type type)
    {
        switch (type) {
            case ModelerAPI::Light::UndefinedLight: return "undefined";
            case ModelerAPI::Light::DirectionLight: return "direction";
            case ModelerAPI::Light::SpotLight:      return "spot";
            case ModelerAPI::Light::PointLight:     return "point";
            case ModelerAPI::Light::SunLight:       return "sun";
            case ModelerAPI::Light::EyeLight:       return "eye";
            case ModelerAPI::Light::AmbientLight:   return "ambient";
            case ModelerAPI::Light::CameraLight:    return "camera";
        }
        return "unknown";
    }

    // ⚠️ ONE BAD LIGHT MUST NOT COST THE WHOLE LIST.
    //
    // Confirmed live 2026-08-03: on a 3769-element project reporting 256 lights,
    // this command threw and returned NOTHING — the caller lost all 256, and the
    // exception's message was empty, so it did not even say which light or why.
    // Every project tested before that had exactly 3 lights, and Light.hpp
    // documents why that never bit: AMBIENT/CAMERA/SUN are model indices 1, 2 and
    // 3, so a 3-light model's loop only ever touched the synthetic lights. A real
    // lamp was reached here for the first time.
    //
    // ModelerAPI::Light holds a `ConstSharedPtr<ILightRelay>` and its accessors
    // are not documented to throw or to say whether the relay is populated, so
    // there is nothing to test up front — ⚠️ the CAUSE remains unverified. What is
    // fixed is the blast radius: each light is described inside its own try, and a
    // failure is recorded by INDEX with its reason and skipped. The caller gets
    // 255 lights and the name of the one that did not work, instead of nothing.
    template <typename Owner>
    static bool DescribeOrSkip (const Owner& owner, Int32 index,
                                ModelerAPI::CoordinateSystem cs, bool wantParameters,
                                GS::ObjectState& out, GS::Array<GS::ObjectState>& skipped)
    {
        GS::UniString reason;
        try {
            ModelerAPI::Light light;
            owner.GetLight (index, &light);

            // Per-FIELD containment. A light is only skipped now if fetching it
            // throws outright, or if every field failed — a partial record is
            // worth more than nothing, and it names the accessor that broke.
            GS::Array<GS::UniString> failedFields;
            GS::UniString fieldReason;
            out = Describe (light, cs, wantParameters, failedFields, fieldReason);
            if (!failedFields.IsEmpty ()) {
                out.Add ("failedFields", failedFields);
                out.Add ("failedReason", fieldReason);
                GS::ObjectState note;
                note.Add ("lightIndex", (GS::Int32) index);
                note.Add ("reason", fieldReason);
                note.Add ("failedFields", failedFields);
                note.Add ("stage", GS::UniString ("fields"));
                note.Add ("partial", true);   // the light IS in `lights`, just incomplete
                skipped.Push (note);
            }
            return true;
        } catch (const GS::Exception& ex) {
            reason = GS::UniString (ex.GetName () != nullptr ? ex.GetName () : "GS::Exception");
            if (!ex.GetMessage ().IsEmpty ())
                reason += GS::UniString (": ") + ex.GetMessage ();
        } catch (const std::exception& ex) {
            reason = GS::UniString ("std::exception: ") +
                     GS::UniString (ex.what () != nullptr ? ex.what () : "(no message)");
        } catch (...) {
            reason = "unknown exception";
        }

        // ⚠️ `stage: "fetch"` means GetLight ITSELF threw — no field accessor ever
        // ran, so the coordinate system, the parameter list and every property
        // getter are all excluded as causes. Reading the absence of `failedFields`
        // to infer this worked, but only for someone who knew the code; say it.
        //
        // Observed 2026-08-03 on three projects: EVERY model light index >= 4
        // fails here with GS::IllegalArgumentException while GetLightCount()
        // happily reports 256. Light.hpp fixes AMBIENT/CAMERA/SUN at 1/2/3, and
        // lamp ELEMENTS in the same models report lightCount 1 each — so the open
        // question is whether Model::GetLight serves only the three specials and
        // real lamps are reachable only through Element::GetLight. The probe asks
        // that directly; do not implement a fallback until it answers.
        GS::ObjectState note;
        note.Add ("lightIndex", (GS::Int32) index);
        note.Add ("reason", reason);
        note.Add ("stage", GS::UniString ("fetch"));
        skipped.Push (note);
        return false;
    }

    // One accessor, contained. Returns false and records the field name if it
    // threw — see Describe for why every single field is wrapped.
    template <typename Fn>
    static bool Field (GS::ObjectState& record, const char* name, Fn read,
                       GS::Array<GS::UniString>& failed, GS::UniString& firstReason)
    {
        try {
            read (record);
            return true;
        } catch (const GS::Exception& ex) {
            if (firstReason.IsEmpty ()) {
                firstReason = GS::UniString (ex.GetName () != nullptr ? ex.GetName ()
                                                                      : "GS::Exception");
                if (!ex.GetMessage ().IsEmpty ())
                    firstReason += GS::UniString (": ") + ex.GetMessage ();
            }
        } catch (...) {
            if (firstReason.IsEmpty ())
                firstReason = "unknown exception";
        }
        failed.Push (GS::UniString (name));
        return false;
    }

    // ⚠️ EVERY FIELD IS WRAPPED INDIVIDUALLY, and that is the point.
    //
    // Reproduced on two independent projects 2026-08-03: every REAL lamp (model
    // index >= 4, i.e. everything past the synthetic ambient/camera/sun) throws
    // GS::IllegalArgumentException — 5 of 8 lights in one project, 253 of 256 in
    // another, with the survivors always exactly indices 1..3.
    //
    // Wrapping the whole record told us THAT it throws. It could not tell us
    // WHICH accessor throws, and Light.hpp documents none of them as throwing, so
    // there is nothing to read and no way to narrow it but to ask each one
    // separately. Now the response names the field.
    //
    // The second benefit matters more day to day: a lamp that cannot report its
    // direction still reports its type, colour and radius. Losing one accessor is
    // no longer the same as losing the light.
    static GS::ObjectState Describe (const ModelerAPI::Light& light,
                                     ModelerAPI::CoordinateSystem cs, bool wantParameters,
                                     GS::Array<GS::UniString>& failedFields,
                                     GS::UniString& firstReason)
    {
        GS::ObjectState record;

        Field (record, "type", [&] (GS::ObjectState& r) {
            r.Add ("type", (GS::Int32) light.GetType ());
            r.Add ("typeName", LightTypeName (light.GetType ()));
        }, failedFields, firstReason);
        Field (record, "castsShadow", [&] (GS::ObjectState& r) {
            r.Add ("castsShadow", light.CastsShadow ()); }, failedFields, firstReason);
        Field (record, "color", [&] (GS::ObjectState& r) {
            r.Add ("color", ColorToObjectState (light.GetColor ())); }, failedFields, firstReason);

        // The three coordinate-system accessors — the prime suspects, since `cs`
        // is the only ARGUMENT any of these take and the exception is an
        // ILLEGAL ARGUMENT one.
        Field (record, "position", [&] (GS::ObjectState& r) {
            r.Add ("position", VertexToObjectState (light.GetPosition (cs))); },
            failedFields, firstReason);
        Field (record, "direction", [&] (GS::ObjectState& r) {
            r.Add ("direction", VectorToObjectState (light.GetDirection (cs))); },
            failedFields, firstReason);
        Field (record, "upVector", [&] (GS::ObjectState& r) {
            r.Add ("upVector", VectorToObjectState (light.GetUpVector (cs))); },
            failedFields, firstReason);

        Field (record, "radius", [&] (GS::ObjectState& r) {
            r.Add ("radius", light.GetRadius ()); }, failedFields, firstReason);
        Field (record, "angleFalloff", [&] (GS::ObjectState& r) {
            r.Add ("angleFalloff", light.GetAngleFalloff ()); }, failedFields, firstReason);
        Field (record, "falloffAngle1", [&] (GS::ObjectState& r) {
            r.Add ("falloffAngle1", light.GetFalloffAngle1 ()); }, failedFields, firstReason);
        Field (record, "falloffAngle2", [&] (GS::ObjectState& r) {
            r.Add ("falloffAngle2", light.GetFalloffAngle2 ()); }, failedFields, firstReason);
        Field (record, "distanceFalloff", [&] (GS::ObjectState& r) {
            r.Add ("distanceFalloff", light.GetDistanceFalloff ()); }, failedFields, firstReason);
        Field (record, "minDistance", [&] (GS::ObjectState& r) {
            r.Add ("minDistance", light.GetMinDistance ()); }, failedFields, firstReason);
        Field (record, "maxDistance", [&] (GS::ObjectState& r) {
            r.Add ("maxDistance", light.GetMaxDistance ()); }, failedFields, firstReason);

        if (wantParameters) {
            Field (record, "parameters", [&] (GS::ObjectState& r) {
                ModelerAPI::ParameterList extras;
                light.GetExtraParameters (&extras);
                r.Add ("parameters", DescribeParameters (extras));
            }, failedFields, firstReason);
        }
        return record;
    }

    // Same containment as DescribeOrSkip, and for the same reason: the sun is the
    // single most-wanted record here (it is the shadow direction, and nothing else
    // in EvP exposes it), so losing it because the CAMERA light misbehaved would be
    // the worst possible trade.
    static void AddSpecial (const ModelerAPI::Model& model, ModelerAPI::Model::SpecialLightType type,
                            const char* scope, ModelerAPI::CoordinateSystem cs, bool wantParameters,
                            GS::Array<GS::ObjectState>& out, GS::Array<GS::ObjectState>& skipped)
    {
        GS::UniString reason;
        try {
            ModelerAPI::Light light;
            model.GetLight (type, &light);
            GS::Array<GS::UniString> failedFields;
            GS::UniString fieldReason;
            GS::ObjectState record = Describe (light, cs, wantParameters,
                                               failedFields, fieldReason);
            if (!failedFields.IsEmpty ()) {
                record.Add ("failedFields", failedFields);
                record.Add ("failedReason", fieldReason);
            }
            record.Add ("scope", GS::UniString (scope));
            record.Add ("lightIndex", (GS::Int32) type);
            out.Push (record);
            return;
        } catch (const GS::Exception& ex) {
            reason = GS::UniString (ex.GetName () != nullptr ? ex.GetName () : "GS::Exception");
            if (!ex.GetMessage ().IsEmpty ())
                reason += GS::UniString (": ") + ex.GetMessage ();
        } catch (const std::exception& ex) {
            reason = GS::UniString ("std::exception: ") +
                     GS::UniString (ex.what () != nullptr ? ex.what () : "(no message)");
        } catch (...) {
            reason = "unknown exception";
        }

        GS::ObjectState note;
        note.Add ("scope", GS::UniString (scope));
        note.Add ("reason", reason);
        skipped.Push (note);
    }
};


// ---------------------------------------------------------------------------
// EvP.GetTextureCoordinates { guid | elementIndex, body?:1, polygon:N,
//                             points:[x,y,z, …], source?:"tessellated"|"mesh" }
//   -> { ok, guid, bodyIndex, polygonIndex, count, u:[…], v:[…] }
//
// Where a world-space point on a polygon lands in its texture's UV space —
// ModelerAPI::Polygon::GetTextureCoordinate, the C++ path.
//
// ⚠️ THE POINTS MUST BE IN WORLD COORDINATES and must lie ON that polygon; the
// API projects rather than validates, so a point off the face returns a UV that
// looks plausible and is meaningless. Take the points from the same body's
// `vertices` (world) and you cannot get this wrong.
//
// `points` is flat [x,y,z, …] — bulk numerics, and it keeps the request
// symmetric with the `vertices` array it is normally fed from.
//
// The C API has a second route to the same answer, `ACAPI_ModelAccess_GetTextureCoord`,
// which takes the (elemIdx, bodyIdx, pgonIndex) triple from API_BodyType and a
// LOCAL-coordinate point. That one lives with the rest of the C API path in
// EvP.GetBodyComponents' domain, because it needs those indices and they only
// exist there. Same question, two coordinate conventions; do not mix them.
// ---------------------------------------------------------------------------
class GetTextureCoordinatesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetTextureCoordinates"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetTextureCoordinates"));
        }

        ModelerAPI::Element elem;
        Int32 elemIndex = 0;
        GS::UniString err;
        if (!ResolveElement (model, params, elem, elemIndex, err))
            return NativeCommandResult::Failure (
                EVP_FAIL (err, "EvP.GetTextureCoordinates element lookup"));

        GS::Array<double> points;
        if (!params.Get ("points", points) || points.GetSize () < 3 || points.GetSize () % 3 != 0) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("need points=[x,y,z, …] (a multiple of 3, world coordinates)",
                          "EvP.GetTextureCoordinates"));
        }

        GS::UniString source ("tessellated");
        params.Get ("source", source);
        const bool useMeshBody = (source == "mesh");
        const Int32 bodyCount = useMeshBody ? elem.GetMeshBodyCount () : elem.GetTessellatedBodyCount ();

        GS::Int32 bodyIndex = 1, polygonIndex = 0;
        params.Get ("body", bodyIndex);
        if (!params.Get ("polygon", polygonIndex)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("need polygon=N (1-based, from EvP.GetBodyGeometry)",
                          "EvP.GetTextureCoordinates"));
        }
        if (bodyIndex < 1 || bodyIndex > bodyCount) {
            return NativeCommandResult::Failure (EVP_FAIL (
                GS::UniString::Printf ("body %d is out of range (1..%d)", (int) bodyIndex, (int) bodyCount),
                "body indices are 1-based"));
        }

        ModelerAPI::MeshBody body;
        if (useMeshBody) elem.GetMeshBody ((Int32) bodyIndex, &body);
        else             elem.GetTessellatedBody ((Int32) bodyIndex, &body);

        const Int32 polygonCount = body.GetPolygonCount ();
        if (polygonIndex < 1 || polygonIndex > polygonCount) {
            return NativeCommandResult::Failure (EVP_FAIL (
                GS::UniString::Printf ("polygon %d is out of range (1..%d)",
                                       (int) polygonIndex, (int) polygonCount),
                "polygon indices are 1-based"));
        }

        GS::Array<double> us, vs;
        try {
            ModelerAPI::Polygon polygon;
            body.GetPolygon ((Int32) polygonIndex, &polygon);
            for (UIndex i = 0; i + 2 < points.GetSize (); i += 3) {
                const ModelerAPI::Vertex point (points[i], points[i + 1], points[i + 2]);
                ModelerAPI::TextureCoordinate uv { 0.0, 0.0 };
                polygon.GetTextureCoordinate (&point, &uv);
                us.Push (uv.u);
                vs.Push (uv.v);
            }
        } catch (const GS::Exception& ex) {
            return NativeCommandResult::Failure (
                EVP_FAIL (GS::UniString ("GetTextureCoordinate threw: ") + ex.GetMessage (),
                          "the polygon may be degenerate, or have no texture"));
        }

        AddElementId (ElementGuidString (elem), os);
        os.Add ("elementIndex", (GS::Int32) elemIndex);
        os.Add ("source", source);
        os.Add ("bodyIndex", bodyIndex);
        os.Add ("polygonIndex", polygonIndex);
        os.Add ("count", (GS::Int32) us.GetSize ());
        os.Add ("u", us);
        os.Add ("v", vs);
        return os;
    }
};

constexpr const char kPoolInput[] = R"json({"type":"object","properties":{"indices":{"type":"array","items":{"type":"integer","minimum":1}}},"additionalProperties":false})json";
constexpr const char kTexturePoolInput[] = R"json({"type":"object","properties":{"indices":{"type":"array","items":{"type":"integer","minimum":1}},"usedOnly":{"type":"boolean"}},"additionalProperties":false})json";
constexpr const char kMaterialsOutput[] = R"json(
{"type":"object","properties":{"materialCount":{"type":"integer","minimum":0},"count":{"type":"integer","minimum":0},"materials":{"type":"array","items":{"$ref":"#/$defs/material"}}},"additionalProperties":false,"required":["materialCount","count","materials"],"$defs":{"color":{"type":"object","properties":{"red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"}},"additionalProperties":false,"required":["red","green","blue"]},"attributeIndex":{"type":"object","properties":{"index":{"type":"integer"},"originalModelerIndex":{"type":"integer"},"originalIndex":{"type":"integer"},"valid":{"type":"boolean"}},"additionalProperties":false,"required":["index","originalModelerIndex","originalIndex","valid"]},"material":{"type":"object","properties":{"modelIndex":{"type":"integer","minimum":1},"type":{"type":"integer"},"typeName":{"type":"string"},"name":{"type":"string"},"surfaceColor":{"$ref":"#/$defs/color"},"ambientReflection":{"type":"number"},"diffuseReflection":{"type":"number"},"specularReflection":{"type":"number"},"specularColor":{"$ref":"#/$defs/color"},"transparency":{"type":"number"},"transparencyAttenuation":{"type":"number"},"shining":{"type":"number"},"emissionColor":{"$ref":"#/$defs/color"},"emissionAttenuation":{"type":"number"},"externalReference":{"type":"integer"},"hasTexture":{"type":"boolean"},"textureName":{"type":"string"},"textureRotationAngle":{"type":"number"},"textureIndex":{"$ref":"#/$defs/attributeIndex"},"fillIndex":{"$ref":"#/$defs/attributeIndex"},"fillColorIndex":{"$ref":"#/$defs/attributeIndex"}},"additionalProperties":false,"required":["modelIndex","type","typeName","name","surfaceColor","ambientReflection","diffuseReflection","specularReflection","specularColor","transparency","transparencyAttenuation","shining","emissionColor","emissionAttenuation","externalReference","hasTexture","fillIndex","fillColorIndex"]}}}
)json";
constexpr const char kColorsOutput[] = R"json({"type":"object","properties":{"colorCount":{"type":"integer","minimum":0},"count":{"type":"integer","minimum":0},"colors":{"type":"array","items":{"type":"object","properties":{"modelIndex":{"type":"integer","minimum":1},"red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"}},"additionalProperties":false,"required":["modelIndex","red","green","blue"]}}},"additionalProperties":false,"required":["colorCount","count","colors"]})json";
constexpr const char kTexturesOutput[] = R"json({"type":"object","properties":{"textureCount":{"type":"integer","minimum":0},"count":{"type":"integer","minimum":0},"textures":{"type":"array","items":{"type":"object","properties":{"modelIndex":{"type":"integer","minimum":1},"used":{"type":"boolean"},"name":{"type":"string"},"available":{"type":"boolean"},"hasAlphaChannel":{"type":"boolean"},"gdlStatus":{"type":"integer"},"transparentPattern":{"type":"boolean"},"bumpMapPattern":{"type":"boolean"},"diffusePattern":{"type":"boolean"},"specularPattern":{"type":"boolean"},"ambientPattern":{"type":"boolean"},"surfacePattern":{"type":"boolean"},"shiftedRandomly":{"type":"boolean"},"mirroredInX":{"type":"boolean"},"mirroredInY":{"type":"boolean"},"xSize":{"type":"number"},"ySize":{"type":"number"},"pixelMapXSize":{"type":"integer","minimum":0},"pixelMapYSize":{"type":"integer","minimum":0},"pixelMapSize":{"type":"integer","minimum":0},"pixelMapBufferSize":{"type":"integer","minimum":0},"pixelType":{"type":"integer"},"pixelTypeName":{"type":"string"},"checksum":{"type":"string"},"fingerprint":{"type":"string"}},"additionalProperties":false,"required":["modelIndex","used","name","available","hasAlphaChannel","gdlStatus","transparentPattern","bumpMapPattern","diffusePattern","specularPattern","ambientPattern","surfacePattern","shiftedRandomly","mirroredInX","mirroredInY","xSize","ySize","pixelMapXSize","pixelMapYSize","pixelMapSize","pixelMapBufferSize","pixelType","pixelTypeName","checksum","fingerprint"]}}},"additionalProperties":false,"required":["textureCount","count","textures"]})json";
constexpr const char kPixelsInput[] = R"json({"type":"object","properties":{"index":{"type":"integer","minimum":1},"name":{"type":"string","minLength":1},"x":{"type":"integer","minimum":0},"y":{"type":"integer","minimum":0},"width":{"type":"integer"},"height":{"type":"integer"},"maxPixels":{"type":"integer","minimum":1}},"additionalProperties":false,"oneOf":[{"required":["index"]},{"required":["name"]}]})json";
constexpr const char kPixelsOutput[] = R"json({"type":"object","properties":{"name":{"type":"string"},"pixelMapXSize":{"type":"integer","minimum":0},"pixelMapYSize":{"type":"integer","minimum":0},"x":{"type":"integer","minimum":0},"y":{"type":"integer","minimum":0},"width":{"type":"integer","minimum":1},"height":{"type":"integer","minimum":1},"truncated":{"type":"boolean"},"pixels":{"type":"array","description":"Packed ARGB bytes in top-to-bottom row-major order; stride 4 per pixel.","items":{"type":"integer","minimum":0,"maximum":255}}},"additionalProperties":false,"required":["name","pixelMapXSize","pixelMapYSize","x","y","width","height","truncated","pixels"]})json";
constexpr const char kLightsInput[] = R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"coordinateSystem":{"type":"string","enum":["world","local"]},"specials":{"type":"boolean"},"sweepElements":{"type":"boolean"},"include":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["parameters","all"]}}},"additionalProperties":false,"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kLightsOutput[] = R"json(
{"type":"object","properties":{"coordinateSystem":{"type":"string","enum":["world","local"]},"count":{"type":"integer","minimum":0},"lights":{"type":"array","items":{"$ref":"#/$defs/light"}},"skippedCount":{"type":"integer","minimum":0},"skipped":{"type":"array","items":{"$ref":"#/$defs/skipped"}},"sweptElements":{"type":"boolean"},"recoveredFromElements":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["coordinateSystem","count","lights","skippedCount"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"color":{"type":"object","properties":{"red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"}},"additionalProperties":false,"required":["red","green","blue"]},"parameter":{"type":"object","properties":{"name":{"type":"string"},"type":{"type":"integer"},"isArray":{"type":"boolean"},"value":{"oneOf":[{"type":"number"},{"type":"string"}]},"dimension1":{"type":"integer","minimum":0},"dimension2":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["name","type","isArray"]},"light":{"type":"object","properties":{"scope":{"type":"string"},"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"lightIndex":{"type":"integer"},"isSpecialIndex":{"type":"boolean"},"viaElementSweep":{"type":"boolean"},"type":{"type":"integer"},"typeName":{"type":"string"},"castsShadow":{"type":"boolean"},"color":{"$ref":"#/$defs/color"},"position":{"$ref":"#/$defs/point"},"direction":{"$ref":"#/$defs/point"},"upVector":{"$ref":"#/$defs/point"},"radius":{"type":"number"},"angleFalloff":{"type":"number"},"falloffAngle1":{"type":"number"},"falloffAngle2":{"type":"number"},"distanceFalloff":{"type":"number"},"minDistance":{"type":"number"},"maxDistance":{"type":"number"},"parameters":{"type":"array","items":{"$ref":"#/$defs/parameter"}},"failedFields":{"type":"array","items":{"type":"string"}},"failedReason":{"type":"string"}},"additionalProperties":false,"required":["scope","lightIndex"]},"skipped":{"type":"object","properties":{"scope":{"type":"string"},"lightIndex":{"type":"integer"},"reason":{"type":"string"},"failedFields":{"type":"array","items":{"type":"string"}},"stage":{"type":"string","enum":["fetch","fields"]},"partial":{"type":"boolean"}},"additionalProperties":false,"required":["reason"]}}}
)json";
constexpr const char kTextureCoordinatesInput[] = R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"body":{"type":"integer","minimum":1},"polygon":{"type":"integer","minimum":1},"points":{"type":"array","minItems":3,"description":"Packed world xyz points; stride 3.","items":{"type":"number"}},"source":{"type":"string","enum":["tessellated","mesh"]}},"additionalProperties":false,"required":["polygon","points"],"anyOf":[{"required":["elementId"]},{"required":["elementIndex"]}],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kTextureCoordinatesOutput[] = R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"source":{"type":"string","enum":["tessellated","mesh"]},"bodyIndex":{"type":"integer","minimum":1},"polygonIndex":{"type":"integer","minimum":1},"count":{"type":"integer","minimum":0},"u":{"type":"array","description":"Packed U coordinates positionally aligned with v and input points.","items":{"type":"number"}},"v":{"type":"array","description":"Packed V coordinates positionally aligned with u and input points.","items":{"type":"number"}}},"additionalProperties":false,"required":["elementId","elementIndex","source","bodyIndex","polygonIndex","count","u","v"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}}})json";

const NativeCommandRegistration commandRegistrations[] = {
    { "GetModelMaterials",     &MakeRegisteredNativeCommand<GetModelMaterialsCommand>,     false, kPoolInput,               kMaterialsOutput },
    { "GetModelColors",        &MakeRegisteredNativeCommand<GetModelColorsCommand>,        false, kPoolInput,               kColorsOutput },
    { "GetModelTextures",      &MakeRegisteredNativeCommand<GetModelTexturesCommand>,      false, kTexturePoolInput,        kTexturesOutput },
    { "GetTexturePixels",      &MakeRegisteredNativeCommand<GetTexturePixelsCommand>,      false, kPixelsInput,             kPixelsOutput },
    { "GetModelLights",        &MakeRegisteredNativeCommand<GetModelLightsCommand>,        false, kLightsInput,             kLightsOutput },
    { "GetTextureCoordinates", &MakeRegisteredNativeCommand<GetTextureCoordinatesCommand>, false, kTextureCoordinatesInput, kTextureCoordinatesOutput },
};

}   // namespace

NativeCommandRegistrations GetModelAppearanceCommandRegistrations ()
{
    return MakeRegistrationView (commandRegistrations);
}

} // namespace geomsrv
