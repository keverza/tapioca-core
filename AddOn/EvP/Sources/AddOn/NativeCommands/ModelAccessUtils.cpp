#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ModelAccessUtils.hpp"

#include <Model.hpp>
#include <ModelMaterial.hpp>
#include <ModelColor.hpp>
#include <ModelVector.hpp>
#include <Vertex.hpp>
#include <Texture.hpp>
#include <TextureCoordinate.hpp>
#include <AttributeIndex.hpp>
#include <Box3DData.h>

namespace geomsrv {

// ---------------------------------------------------------------------------
// Coordinate system
// ---------------------------------------------------------------------------

ModelerAPI::CoordinateSystem ParseCoordinateSystem (const GS::ObjectState& params, const char* key)
{
    GS::UniString name;
    if (params.Get (key, name)) {
        if (name == "local" || name == "elemLocal" || name == "ElemLocal")
            return ModelerAPI::CoordinateSystem::ElemLocal;
    }
    return ModelerAPI::CoordinateSystem::World;
}


GS::UniString CoordinateSystemName (ModelerAPI::CoordinateSystem cs)
{
    return cs == ModelerAPI::CoordinateSystem::ElemLocal ? GS::UniString ("local")
                                                         : GS::UniString ("world");
}


bool WantsSection (const GS::ObjectState& params, const char* section, bool byDefault)
{
    GS::Array<GS::UniString> include;
    if (!params.Get ("include", include))
        return byDefault;               // no list at all -> the section's own default
    for (const GS::UniString& s : include) {
        if (s == section || s == "all")
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ObjectState conversions
// ---------------------------------------------------------------------------

GS::ObjectState PointToObjectState (double x, double y, double z)
{
    GS::ObjectState os;
    os.Add ("x", x);
    os.Add ("y", y);
    os.Add ("z", z);
    return os;
}


GS::ObjectState VertexToObjectState (const ModelerAPI::Vertex& v)
{
    return PointToObjectState (v.x, v.y, v.z);
}


GS::ObjectState VectorToObjectState (const ModelerAPI::Vector& v)
{
    return PointToObjectState (v.x, v.y, v.z);
}


GS::ObjectState ColorToObjectState (const ModelerAPI::Color& c)
{
    GS::ObjectState os;
    os.Add ("red", c.red);          // 0..1, NOT 0..255 — ModelerAPI::Color is double RGB
    os.Add ("green", c.green);
    os.Add ("blue", c.blue);
    return os;
}


GS::ObjectState BoxToObjectState (const Box3D& box)
{
    GS::ObjectState os;
    os.Add ("xMin", box.GetMinX ());
    os.Add ("yMin", box.GetMinY ());
    os.Add ("zMin", box.GetMinZ ());
    os.Add ("xMax", box.GetMaxX ());
    os.Add ("yMax", box.GetMaxY ());
    os.Add ("zMax", box.GetMaxZ ());
    return os;
}


GS::ObjectState AttributeIndexToObjectState (const ModelerAPI::AttributeIndex& index)
{
    GS::ObjectState os;
    os.Add ("index", (GS::Int32) index.GetIndex ());
    os.Add ("originalModelerIndex", (GS::Int32) index.GetOriginalModelerIndex ());
    os.Add ("originalIndex", (GS::Int32) index.GetOriginalIndex ());
    os.Add ("valid", index.IsValid ());
    return os;
}


static GS::UniString MaterialTypeName (ModelerAPI::Material::Type type)
{
    switch (type) {
        case ModelerAPI::Material::General:  return "general";
        case ModelerAPI::Material::Simple:   return "simple";
        case ModelerAPI::Material::Matte:    return "matte";
        case ModelerAPI::Material::Metal:    return "metal";
        case ModelerAPI::Material::Plastic:  return "plastic";
        case ModelerAPI::Material::Glass:    return "glass";
        case ModelerAPI::Material::Glowing:  return "glowing";
        case ModelerAPI::Material::Constant: return "constant";
    }
    // ⚠️ Values OUTSIDE the published enum exist in the wild. Confirmed live: four
    // surfaces in a stock project report type 20, which is in neither
    // ModelerAPI::Material::Type (ModelMaterial.hpp) nor API_MaterTypeID
    // (APIdefs_Attributes.h) — both stop at 7. Almost certainly a CineRender /
    // physical material with no public constant. "unmapped", not "unknown", so a
    // caller can tell "outside the published enum" from "the call failed"; the raw
    // `type` int travels beside this and stays authoritative.
    return "unmapped";
}


GS::ObjectState MaterialToObjectState (const ModelerAPI::Material& material)
{
    GS::ObjectState os;
    os.Add ("type", (GS::Int32) material.GetType ());
    os.Add ("typeName", MaterialTypeName (material.GetType ()));
    os.Add ("name", material.GetName ());
    os.Add ("surfaceColor", ColorToObjectState (material.GetSurfaceColor ()));
    os.Add ("ambientReflection", material.GetAmbientReflection ());
    os.Add ("diffuseReflection", material.GetDiffuseReflection ());
    os.Add ("specularReflection", material.GetSpecularReflection ());
    os.Add ("specularColor", ColorToObjectState (material.GetSpecularColor ()));
    os.Add ("transparency", material.GetTransparency ());
    os.Add ("transparencyAttenuation", material.GetTransparencyAttenuation ());
    os.Add ("shining", material.GetShining ());
    os.Add ("emissionColor", ColorToObjectState (material.GetEmissionColor ()));
    os.Add ("emissionAttenuation", material.GetEmissionAttenuation ());
    os.Add ("externalReference", (GS::Int32) material.GetExternalReference ());

    os.Add ("hasTexture", material.HasTexture ());
    if (material.HasTexture ()) {
        os.Add ("textureName", material.GetTextureName ());
        os.Add ("textureRotationAngle", material.GetTextureRotationAngle ());
        ModelerAPI::AttributeIndex textureIndex;
        material.GetTextureIndex (textureIndex);
        os.Add ("textureIndex", AttributeIndexToObjectState (textureIndex));
    }

    ModelerAPI::AttributeIndex fillIndex, fillColorIndex;
    material.GetFillIndex (fillIndex);
    material.GetFillColorIndex (fillColorIndex);
    os.Add ("fillIndex", AttributeIndexToObjectState (fillIndex));
    os.Add ("fillColorIndex", AttributeIndexToObjectState (fillColorIndex));
    return os;
}


static GS::UniString PixelTypeName (ModelerAPI::Texture::PixelType type)
{
    switch (type) {
        case ModelerAPI::Texture::ARGBPixelType:          return "argb";
        case ModelerAPI::Texture::Grayscale8PixelType:    return "grayscale8";
        case ModelerAPI::Texture::BlackAndWhitePixelType: return "blackAndWhite";
    }
    return "unknown";
}


GS::ObjectState TextureToObjectState (const ModelerAPI::Texture& texture)
{
    GS::ObjectState os;
    os.Add ("name", texture.GetName ());
    os.Add ("available", texture.IsAvailable ());
    os.Add ("hasAlphaChannel", texture.HasAlphaChannel ());
    os.Add ("gdlStatus", (GS::Int32) texture.GetGDLStatus ());

    // The GDL status bits, spelled out. A caller deciding "is this a bump map"
    // should not have to know Texture::Status' hex values.
    os.Add ("transparentPattern", texture.IsTransparentPattern ());
    os.Add ("bumpMapPattern", texture.IsBumpMapPattern ());
    os.Add ("diffusePattern", texture.IsDiffusePattern ());
    os.Add ("specularPattern", texture.IsSpecularPattern ());
    os.Add ("ambientPattern", texture.IsAmbientPattern ());
    os.Add ("surfacePattern", texture.IsSurfacePattern ());
    os.Add ("shiftedRandomly", texture.IsShiftedRandomly ());
    os.Add ("mirroredInX", texture.IsMirroredInX ());
    os.Add ("mirroredInY", texture.IsMirroredInY ());

    os.Add ("xSize", texture.GetXSize ());          // world metres the image covers
    os.Add ("ySize", texture.GetYSize ());
    os.Add ("pixelMapXSize", (GS::Int32) texture.GetPixelMapXSize ());
    os.Add ("pixelMapYSize", (GS::Int32) texture.GetPixelMapYSize ());
    os.Add ("pixelMapSize", (GS::Int32) texture.GetPixelMapSize ());
    os.Add ("pixelMapBufferSize", (GS::Int32) texture.GetPixelMapBufferSize ());
    os.Add ("pixelType", (GS::Int32) texture.GetPixelType ());
    os.Add ("pixelTypeName", PixelTypeName (texture.GetPixelType ()));

    // Identity without the bytes: two textures with the same checksum are the
    // same image, which is what a caller building an export cache needs.
    os.Add ("checksum", texture.GetPixelMapCheckSum ());
    os.Add ("fingerprint", texture.GetFingerprint ());
    return os;
}


static GS::UniString TextureModeName (ModelerAPI::TextureCoordinateSystem::TransformationMode mode)
{
    switch (mode) {
        case ModelerAPI::TextureCoordinateSystem::BoxMode:        return "box";
        case ModelerAPI::TextureCoordinateSystem::CylindricMode:  return "cylindric";
        case ModelerAPI::TextureCoordinateSystem::SphericMode:    return "spheric";
        case ModelerAPI::TextureCoordinateSystem::NurbsParamMode: return "nurbsParam";
        case ModelerAPI::TextureCoordinateSystem::InvalidMode:    return "invalid";
    }
    return "unknown";
}


GS::ObjectState TextureCoordSysToObjectState (const ModelerAPI::TextureCoordinateSystem& cs)
{
    GS::ObjectState os;
    os.Add ("mode", (GS::Int32) cs.transformationMode);
    os.Add ("modeName", TextureModeName (cs.transformationMode));
    os.Add ("origo", VertexToObjectState (cs.origo));
    os.Add ("xAxis", VectorToObjectState (cs.xAxis));
    os.Add ("yAxis", VectorToObjectState (cs.yAxis));
    os.Add ("zAxis", VectorToObjectState (cs.zAxis));
    return os;
}

// ---------------------------------------------------------------------------
// Element identity
// ---------------------------------------------------------------------------

GS::UniString ElementTypeName (ModelerAPI::Element::Type type)
{
    switch (type) {
        case ModelerAPI::Element::UndefinedElement:          return "undefined";
        case ModelerAPI::Element::WallElement:               return "wall";
        case ModelerAPI::Element::SlabElement:               return "slab";
        case ModelerAPI::Element::RoofElement:               return "roof";
        case ModelerAPI::Element::CurtainWallElement:        return "curtainWall";
        case ModelerAPI::Element::CWFrameElement:            return "cwFrame";
        case ModelerAPI::Element::CWPanelElement:            return "cwPanel";
        case ModelerAPI::Element::CWJunctionElement:         return "cwJunction";
        case ModelerAPI::Element::CWAccessoryElement:        return "cwAccessory";
        case ModelerAPI::Element::CWSegmentElement:          return "cwSegment";
        case ModelerAPI::Element::ShellElement:              return "shell";
        case ModelerAPI::Element::SkylightElement:           return "skylight";
        case ModelerAPI::Element::FreeshapeElement:          return "freeshape";
        case ModelerAPI::Element::DoorElement:               return "door";
        case ModelerAPI::Element::WindowElement:             return "window";
        case ModelerAPI::Element::ObjectElement:             return "object";
        case ModelerAPI::Element::LightElement:              return "light";
        case ModelerAPI::Element::ColumnElement:             return "column";
        case ModelerAPI::Element::MeshElement:               return "mesh";
        case ModelerAPI::Element::BeamElement:               return "beam";
        case ModelerAPI::Element::RoomElement:               return "room";
        case ModelerAPI::Element::StairElement:              return "stair";
        case ModelerAPI::Element::RiserElement:              return "riser";
        case ModelerAPI::Element::TreadElement:              return "tread";
        case ModelerAPI::Element::StairStructureElement:     return "stairStructure";
        case ModelerAPI::Element::RailingElement:            return "railing";
        case ModelerAPI::Element::ToprailElement:            return "toprail";
        case ModelerAPI::Element::HandrailElement:           return "handrail";
        case ModelerAPI::Element::RailElement:               return "rail";
        case ModelerAPI::Element::RailingPostElement:        return "railingPost";
        case ModelerAPI::Element::InnerPostElement:          return "innerPost";
        case ModelerAPI::Element::BalusterElement:           return "baluster";
        case ModelerAPI::Element::RailingPanelElement:       return "railingPanel";
        case ModelerAPI::Element::RailingSegmentElement:     return "railingSegment";
        case ModelerAPI::Element::RailingNodeElement:        return "railingNode";
        case ModelerAPI::Element::RailPatternElement:        return "railPattern";
        case ModelerAPI::Element::InnerTopRailEndElement:    return "innerTopRailEnd";
        case ModelerAPI::Element::InnerHandRailEndElement:   return "innerHandRailEnd";
        case ModelerAPI::Element::RailFinishingObjectElement: return "railFinishingObject";
        case ModelerAPI::Element::TopRailConnectionElement:  return "topRailConnection";
        case ModelerAPI::Element::HandRailConnectionElement: return "handRailConnection";
        case ModelerAPI::Element::RailConnectionElement:     return "railConnection";
        case ModelerAPI::Element::RailEndElement:            return "railEnd";
        case ModelerAPI::Element::BalusterSetElement:        return "balusterSet";
        case ModelerAPI::Element::Opening:                   return "opening";
        case ModelerAPI::Element::Openingframeinfill:        return "openingFrameInfill";
        case ModelerAPI::Element::Openingpatchinfill:        return "openingPatchInfill";
        case ModelerAPI::Element::ColumnSegmentElement:      return "columnSegment";
        case ModelerAPI::Element::BeamSegmentElement:        return "beamSegment";
        case ModelerAPI::Element::OtherElement:              return "other";
    }
    return "unknown";
}


GS::UniString ElementGuidString (const ModelerAPI::Element& elem)
{
    return GS::UniString (APIGuidToString (GSGuid2APIGuid (elem.GetElemGuid ())).ToCStr ());
}


GS::UniString EmptyModelHint ()
{
    return "the 3D model has NO elements. Its attribute pools (surfaces, colours, "
           "lights) are still readable, which is the signature of a sight that "
           "exists but has never been GENERATED — open the 3D window once (or hit "
           "Rebuild) and run again. If you need geometry without the 3D window, use "
           "EvP.GetElement3DInfo + EvP.GetBodyComponents: that path converts the "
           "element to 3D on demand.";
}


bool ResolveModelElement (const ModelerAPI::Model& model, const GS::ObjectState& params,
                          ModelerAPI::Element& elem, Int32& elemIndex, GS::UniString& err)
{
    const Int32 total = model.GetElementCount ();

    // Diagnose the EMPTY MODEL before blaming the caller's guid. Observed live
    // 2026-08-02: a first run reported "no 3D model element for guid …" three
    // times over a perfectly valid selection, because the model held zero
    // elements — the guid was never the problem and the message sent the reader
    // hunting for a composite sub-part that did not exist.
    if (total <= 0) {
        err = EmptyModelHint ();
        return false;
    }

    GS::UniString guidString;
    if (params.Get ("guid", guidString) && !guidString.IsEmpty ()) {
        const GS::Guid guid = APIGuid2GSGuid (APIGuidFromString (guidString.ToCStr ().Get ()));
        const std::optional<Int32> found = model.GetElementIndex (guid);
        if (!found.has_value ()) {
            // Not the same failure as a bad guid: a Zone, a 2D-only element or an
            // element on a hidden layer legitimately has no model representation.
            err = "no 3D model element for guid \"" + guidString +
                  "\". It may be 2D-only, hidden by the current 3D filter, or a "
                  "composite whose geometry lives on its sub-parts.";
            return false;
        }
        elemIndex = *found;
        model.GetElement (elemIndex, &elem);
        return true;
    }

    GS::Int32 requested = 0;
    if (params.Get ("elementIndex", requested)) {
        if (requested < 1 || requested > total) {
            err = GS::UniString::Printf ("elementIndex %d is out of range (1..%d)",
                                         (int) requested, (int) total);
            return false;
        }
        elemIndex = (Int32) requested;
        model.GetElement (elemIndex, &elem);
        return true;
    }

    err = "need guid=\"…\" or elementIndex=N (1-based, from EvP.GetModelElements)";
    return false;
}

} // namespace geomsrv
