#ifndef TAPIOCA_EDIT_MATERIAL_HPP
#define TAPIOCA_EDIT_MATERIAL_HPP
#include "Edit/EditLandscape.hpp"

namespace tapioca::geometry {
enum class MaterialAttributeKind : uint8_t { Fill, Surface };
struct MaterialAttribute {
    MaterialAttributeKind kind = MaterialAttributeKind::Fill;
    int32_t index = 0; // Current project only; never persisted in MaterialSpec.
    std::string guid, name;
};
struct MaterialCatalog {
    std::vector<MaterialAttribute> attributes;
};
enum class MaterialResolutionState : uint8_t { NotAssigned, Resolved, Missing, Ambiguous, NeedsConfirmation };
struct MaterialResolution {
    MaterialResolutionState state = MaterialResolutionState::NotAssigned;
    std::optional<MaterialAttribute> attribute;
    std::string message;
};
MaterialResolution ResolveMaterialAttribute (const MaterialCatalog& catalog, MaterialAttributeKind kind,
                                             const std::string& guid, const std::string& name);
bool ValidateMaterialAvailability (const MaterialSpec& material, const MaterialCatalog& catalog, std::string& error);
// Explicit user selection only. Stores GUID/name, never a project-local index.
bool PickMaterialAttribute (MaterialSpec& material, const MaterialAttribute& attribute, std::string& error);
} // namespace tapioca::geometry
#endif
