#include "Edit/EditMaterial.hpp"

namespace tapioca::geometry {
MaterialResolution ResolveMaterialAttribute (const MaterialCatalog& catalog, MaterialAttributeKind kind,
                                             const std::string& guid, const std::string& name)
{
    MaterialResolution result;
    if (guid.empty () && name.empty ())
        return result;
    for (const auto& attribute : catalog.attributes) {
        std::string canonical;
        if (attribute.index <= 0 || attribute.name.empty () || !CanonicalLandscapeGuid (attribute.guid, canonical))
            continue;
        if (attribute.kind != kind || (guid.empty () ? attribute.name != name : canonical != guid))
            continue;
        if (result.attribute) {
            result.state = MaterialResolutionState::Ambiguous;
            result.message = "Material attribute is ambiguous; choose an explicit replacement.";
            return result;
        }
        result.attribute = attribute;
    }
    if (!result.attribute) {
        result.state = MaterialResolutionState::Missing;
        result.message = "Missing material attribute: " + (name.empty () ? guid : name) +
                         ". Choose a replacement or explicitly clear it.";
    }
    else if (guid.empty ()) {
        result.state = MaterialResolutionState::NeedsConfirmation;
        result.message = "Name-only material needs explicit confirmation in the shared picker.";
    }
    else
        result.state = MaterialResolutionState::Resolved;
    return result;
}

bool ValidateMaterialAvailability (const MaterialSpec& material, const MaterialCatalog& catalog, std::string& error)
{
    for (const auto kind : { MaterialAttributeKind::Fill, MaterialAttributeKind::Surface }) {
        const auto result = ResolveMaterialAttribute (
            catalog, kind, kind == MaterialAttributeKind::Fill ? material.fillGuid : material.surfaceGuid,
            kind == MaterialAttributeKind::Fill ? material.fillName : material.surfaceName);
        if (result.state != MaterialResolutionState::Resolved && result.state != MaterialResolutionState::NotAssigned) {
            error = result.message;
            return false;
        }
    }
    error.clear ();
    return true;
}

bool PickMaterialAttribute (MaterialSpec& material, const MaterialAttribute& attribute, std::string& error)
{
    std::string canonical;
    if (attribute.kind > MaterialAttributeKind::Surface || attribute.index <= 0 || attribute.name.empty () ||
        !CanonicalLandscapeGuid (attribute.guid, canonical)) {
        error = "Choose an existing named fill/surface with a valid GUID and current project index.";
        return false;
    }
    if (attribute.kind == MaterialAttributeKind::Fill) {
        material.fillGuid = canonical;
        material.fillName = attribute.name;
    }
    else {
        material.surfaceGuid = canonical;
        material.surfaceName = attribute.name;
    }
    error.clear ();
    return true;
}
} // namespace tapioca::geometry
