#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorMaterial.hpp"
#include <algorithm>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;
bool RequestMaterialPicker (Session& s, lg::Id object)
{
    const auto& doc = s.edits.Doc ();
    if (s.close || !doc.landscape || s.materialPicker || s.edits.InGesture () || GizmoModeLocked (s) ||
        !s.drawing.empty () ||
        std::none_of (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                      [=] (const auto& o) { return o.id == object; })) {
        s.message = "Finish current input and select a hardscape object before choosing its material.";
        return false;
    }
    s.materialPicker = std::make_shared<const MaterialPickerRequest> (MaterialPickerRequest { object, doc.revision });
    return true;
}
bool ApplyPickedMaterial (Session& s, const std::shared_ptr<const MaterialPickerRequest>& request,
                          const lg::MaterialSpec& material)
{
    if (s.close || !request || s.materialPicker != request || s.edits.Doc ().revision != request->revision ||
        s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty ()) {
        s.message = "Material choice cancelled: the editor or target changed while the picker was open.";
        return false;
    }
    const bool result = s.edits.Apply ([id = request->object, material] (edit::Document& doc, std::string& error) {
        return lg::SetHardscapeMaterial (doc, id, material, error);
    });
    s.message = s.edits.Message ();
    return result;
}
} // namespace geomsrv::archviz::edithud
