#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/HardscapeMaterial.hpp"

namespace tapioca::materialui {
bool ReadCatalog (geometry::MaterialCatalog& catalog, std::string& error)
{
    geometry::MaterialCatalog result;
    for (const auto kind : { geometry::MaterialAttributeKind::Fill, geometry::MaterialAttributeKind::Surface }) {
        GS::Array<API_Attribute> attributes;
        const auto type = kind == geometry::MaterialAttributeKind::Fill ? API_FilltypeID : API_MaterialID;
        if (ACAPI_Attribute_GetAttributesByType (type, attributes) != NoError) {
            error = "The current project's fill/surface attributes could not be read.";
            return false;
        }
        for (const auto& attribute : attributes) {
            std::string guid = APIGuidToString (attribute.header.guid).ToCStr (0, MaxUSize, CC_UTF8).Get ();
            if (guid.size () == 38 && guid.front () == '{' && guid.back () == '}')
                guid = guid.substr (1, 36);
            std::string canonical;
            const std::string name = GS::UniString (attribute.header.name).ToCStr (0, MaxUSize, CC_UTF8).Get ();
            if (!name.empty () && geometry::CanonicalLandscapeGuid (guid, canonical))
                result.attributes.push_back ({ kind, attribute.header.index.ToInt32_Deprecated (), canonical, name });
        }
    }
    catalog = std::move (result);
    error.clear ();
    return true;
}
} // namespace tapioca::materialui
