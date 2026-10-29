#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_MATERIAL_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_MATERIAL_HPP
#include "Edit/EditMaterial.hpp"
#include <memory>
namespace geomsrv::archviz::edithud {
struct Session;
struct MaterialPickerRequest {
    tapioca::geometry::Id object = 0;
    uint64_t revision = 0;
};
bool RequestMaterialPicker (Session& session, tapioca::geometry::Id object);
bool ApplyPickedMaterial (Session& session, const std::shared_ptr<const MaterialPickerRequest>& request,
                          const tapioca::geometry::MaterialSpec& material);
} // namespace geomsrv::archviz::edithud
#endif
